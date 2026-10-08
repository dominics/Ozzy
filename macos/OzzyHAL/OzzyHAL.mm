#include "OzzyHAL.h"
#include "../Devices/Ploytec/PloytecCodec.h"
#include "../Devices/Ploytec/PloytecProtocol.h"
#include "../Shared/OzzyLog.h"
#include "../Devices/Dynacord/DynacordCodec.h"
#include "../Devices/Dynacord/DynacordRing.h"
#include <algorithm>
#include <cstring>
#include <mach/mach_time.h>
#include <CoreAudio/CoreAudio.h>

static CFStringRef SafeCreateString(const char* str) {
    if (!str || str[0] == 0) return nullptr;
    CFStringRef s = CFStringCreateWithCString(NULL, str, kCFStringEncodingUTF8);
    if (!s) s = CFStringCreateWithCString(NULL, str, kCFStringEncodingMacRoman);
    if (!s) s = CFStringCreateWithCString(NULL, str, kCFStringEncodingASCII);
    return s;
}

/* The segment is world-writable and its strings carry no NUL guarantee. */
struct SHMName { char s[65]; };
static SHMName CopyName(const char (&src)[64]) {
    SHMName n;
    size_t len = strnlen(src, sizeof(src));
    memcpy(n.s, src, len);
    n.s[len] = 0;
    return n;
}

static uint64_t MsToHostTicks(uint64_t ms) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return ms * 1000000ull * tb.denom / tb.numer;
}

static UInt32 GetAudioBufferListSize(UInt32 numBuffers) {
    return (UInt32)(offsetof(AudioBufferList, mBuffers) + (numBuffers * sizeof(AudioBuffer)));
}

static bool IsOutputScope(AudioObjectPropertyScope scope) {
    return (scope == kAudioObjectPropertyScopeOutput || scope == kAudioObjectPropertyScopeGlobal);
}

// --- Write Helpers ---
static OSStatus WriteCFString(UInt32 inMax, UInt32* outSize, void* outData, CFStringRef s) {
    if (outData && inMax < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
    if (outData) { *(CFStringRef*)outData = s; CFRetain(s); }
    if (outSize) *outSize = sizeof(CFStringRef);
    return kAudioHardwareNoError;
}

static OSStatus WriteClassID(UInt32 inMax, UInt32* outSize, void* outData, AudioClassID v) {
    if (outData && inMax < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
    if (outData) *(AudioClassID*)outData = v;
    if (outSize) *outSize = sizeof(AudioClassID);
    return kAudioHardwareNoError;
}

static OSStatus WriteUInt32(UInt32 inMax, UInt32* outSize, void* outData, UInt32 v) {
    if (outData && inMax < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
    if (outData) *(UInt32*)outData = v;
    if (outSize) *outSize = sizeof(UInt32);
    return kAudioHardwareNoError;
}

static OSStatus WriteFloat64(UInt32 inMax, UInt32* outSize, void* outData, Float64 v) {
    if (outData && inMax < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
    if (outData) *(Float64*)outData = v;
    if (outSize) *outSize = sizeof(Float64);
    return kAudioHardwareNoError;
}

static OSStatus WriteObjectID(UInt32 inMax, UInt32* outSize, void* outData, AudioObjectID v) {
    if (outData && inMax < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
    if (outData) *(AudioObjectID*)outData = v;
    if (outSize) *outSize = sizeof(AudioObjectID);
    return kAudioHardwareNoError;
}

OzzyHAL& OzzyHAL::Get() {
    static OzzyHAL instance;
    return instance;
}

void OzzyHAL::Init(AudioServerPlugInHostRef host) {
    LogOzzyHAL("init");
    mHost = host;

    mDeviceUID = CFSTR("OzzyHAL.device");
    mModelUID = CFSTR("Ozzy Audio Device");
    mManufacturerUID = CFSTR("Ozzy");
    mZeroTimestampPeriod = 640;

    mCurrentStreamFormat.mSampleRate = 96000.0;
    mCurrentStreamFormat.mFormatID = kAudioFormatLinearPCM;
    mCurrentStreamFormat.mFormatFlags = kAudioFormatFlagIsFloat;
    mCurrentStreamFormat.mBytesPerPacket = 32;
    mCurrentStreamFormat.mFramesPerPacket = 1;
    mCurrentStreamFormat.mBytesPerFrame = 32;
    mCurrentStreamFormat.mChannelsPerFrame = 8;
    mCurrentStreamFormat.mBitsPerChannel = 32;

    mAvailableSampleRates.mMinimum = 96000.0; 
    mAvailableSampleRates.mMaximum = 96000.0;

    RebuildStreamConfigs();

    // Try to map shared memory immediately to detect already-connected devices
    MapSharedMemory();
    
    // Start monitoring thread for connection changes
    mMonitorRunning = true;
    pthread_create(&mMonitorThread, NULL, MonitorEntry, this);
}

void OzzyHAL::Cleanup() {
    mMonitorRunning = false;
    pthread_join(mMonitorThread, NULL);
    UnmapSharedMemory();
}

void OzzyHAL::ClearOutputBuffer() {
    if (!mOutputBufferAddr || !mSHM || !mClearOutput) return;
    const uint32_t bufferSize = kOzzyMaxPacketSize * kOzzyNumPackets;
    mClearOutput(mOutputBufferAddr, bufferSize);
}

void OzzyHAL::RebuildStreamConfigs() {
    const uint32_t kDefaultIOFrames = 512;
    const uint32_t kBytesPerFrame = mCurrentStreamFormat.mBytesPerFrame;

    mInputConfig.mNumberBuffers = 1;
    mInputConfig.mBuffers[0].mNumberChannels = mChannels;
    mInputConfig.mBuffers[0].mDataByteSize = kDefaultIOFrames * kBytesPerFrame;
    mInputConfig.mBuffers[0].mData = nullptr;

    mOutputConfig.mNumberBuffers = 1;
    mOutputConfig.mBuffers[0].mNumberChannels = mChannels;
    mOutputConfig.mBuffers[0].mDataByteSize = kDefaultIOFrames * kBytesPerFrame;
    mOutputConfig.mBuffers[0].mData = nullptr;
}

/* Layout v1 (Xone): 8x8 at 96 kHz in 80-frame packet rings. Returns false, leaving state untouched, if the segment's values are unusable. */
bool OzzyHAL::ApplyPacketLayout() {
    const uint32_t fpp = mSHM->audio.framesPerPacket;
    const uint32_t outBytes = mSHM->audio.outputBytesPerFrame;
    const uint32_t inBytes = mSHM->audio.inputBytesPerFrame;
    /* The Xone codecs index 80-frame logical packets at kOzzyMaxPacketSize strides, so fpp <= 80 keeps every index inside the buffers. */
    if (fpp < 1 || fpp > 80) { LogOzzyHALError("bad shm framesPerPacket %{public}u", fpp); return false; }
    if (outBytes != 48) { LogOzzyHALError("bad shm outputBytesPerFrame %{public}u", outBytes); return false; }
    if (inBytes != 64) { LogOzzyHALError("bad shm inputBytesPerFrame %{public}u", inBytes); return false; }

    mIsDynacord = false;
    mFramesPerPacket = fpp;
    mRingSize = kOzzyNumPackets * fpp;
    mRingSizeIn = mRingSize;
    mOutputBytesPerFrame = outBytes;
    mInputBytesPerFrame = inBytes;
    mZeroTimestampPeriod = 2 * fpp * 4;
    mSafetyOffsetOut = mSafetyOffsetIn = 2 * fpp;
    mChannels = 8;
    mCurrentStreamFormat.mSampleRate = 96000.0;
    mAvailableSampleRates.mMinimum = mAvailableSampleRates.mMaximum = 96000.0;
    mCurrentStreamFormat.mChannelsPerFrame = mChannels;
    mCurrentStreamFormat.mBytesPerFrame = mCurrentStreamFormat.mBytesPerPacket = 4 * mChannels;
    RebuildStreamConfigs();
    return true;
}

/* Layout v2 (Dynacord, kext or daemon): everything comes from the segment. Returns false, leaving state untouched, if any value is unusable. */
bool OzzyHAL::ApplyFrameLayout() {
    const uint32_t vendor = mSHM->vendorID;
    const uint32_t rate = mSHM->audio.sampleRate.load();
    const uint32_t outBytes = mSHM->audio.outputBytesPerFrame;
    const uint32_t inBytes = mSHM->audio.inputBytesPerFrame;
    const uint32_t ringOut = mSHM->audio.ringFramesOut;
    const uint32_t ringIn = mSHM->audio.ringFramesIn;
    const uint32_t period = mSHM->audio.zeroTimestampPeriod;
    const uint32_t safeOut = mSHM->audio.safetyOffsetOut;
    const uint32_t safeIn = mSHM->audio.safetyOffsetIn;
    const uint32_t bufBytes = kOzzyMaxPacketSize * kOzzyNumPackets;

    if (vendor != DYNACORD_VENDOR_ID) { LogOzzyHALError("v2 shm with vendorID 0x%{public}04X", vendor); return false; }
    if (rate != 44100 && rate != 48000 && rate != 96000) { LogOzzyHALError("bad shm sampleRate %{public}u", rate); return false; }
    if (outBytes != kDynacordOutFrameBytes) { LogOzzyHALError("bad shm outputBytesPerFrame %{public}u", outBytes); return false; }
    if (inBytes != kDynacordInFrameBytes) { LogOzzyHALError("bad shm inputBytesPerFrame %{public}u", inBytes); return false; }
    if (ringOut < 1 || ringOut > bufBytes / kDynacordOutFrameBytes) { LogOzzyHALError("bad shm ringFramesOut %{public}u", ringOut); return false; }
    if (ringIn < 1 || ringIn > bufBytes / kDynacordInFrameBytes) { LogOzzyHALError("bad shm ringFramesIn %{public}u", ringIn); return false; }
    /* CoreAudio stalls if the anchor period is shorter than its IO buffer (512 frames by default); 1024 leaves room. */
    if (period < 1024 || period > std::min(ringOut, ringIn)) { LogOzzyHALError("bad shm zeroTimestampPeriod %{public}u", period); return false; }
    if (safeOut >= ringOut) { LogOzzyHALError("bad shm safetyOffsetOut %{public}u", safeOut); return false; }
    if (safeIn >= ringIn) { LogOzzyHALError("bad shm safetyOffsetIn %{public}u", safeIn); return false; }

    mIsDynacord = true;
    mRingSize = ringOut;
    mRingSizeIn = ringIn;
    mOutputBytesPerFrame = outBytes;
    mInputBytesPerFrame = inBytes;
    mZeroTimestampPeriod = period;
    mSafetyOffsetOut = safeOut;
    mSafetyOffsetIn = safeIn;
    mChannels = DYNACORD_CHANNELS;
    mCurrentStreamFormat.mSampleRate = (Float64)rate;
    mAvailableSampleRates.mMinimum = mAvailableSampleRates.mMaximum = (Float64)rate;
    mCurrentStreamFormat.mChannelsPerFrame = mChannels;
    mCurrentStreamFormat.mBytesPerFrame = mCurrentStreamFormat.mBytesPerPacket = 4 * mChannels;
    RebuildStreamConfigs();
    return true;
}

/* Release the mapping now, or if IO may still be running, only mark it dead so the IO path stops touching it; the monitor unmaps after StopIO. */
void OzzyHAL::DropMapping() {
    mMappingDead.store(true);   // first: StartIO and the IO path see this before they touch the segment
    if (mIOClients.load() == 0) UnmapSharedMemory();
}

void OzzyHAL::MapSharedMemory() {
    if (mSHM) return;
    
    // Close any stale kext connection first
    if (mKextConnect != IO_OBJECT_NULL) {
        IOServiceClose(mKextConnect);
        mKextConnect = IO_OBJECT_NULL;
        mKextMapAddress = 0;
        mKextMapSize = 0;
    }

    // --- 1. Try OzzyKext (IOKit) ---
    CFMutableDictionaryRef matching = IOServiceMatching("OzzyKext");
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, matching);
    
    if (service) {
        LogOzzyHAL("found OzzyKext service, connecting...");
        kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &mKextConnect);
        IOObjectRelease(service);

        if (kr == KERN_SUCCESS) {
            kr = IOConnectMapMemory(mKextConnect, 0, mach_task_self(), &mKextMapAddress, &mKextMapSize, kIOMapAnywhere);
            if (kr == KERN_SUCCESS) {
                mSHM = (OzzySharedMemory*)mKextMapAddress;
                mIsKextMapping = true;
                LogOzzyHAL("attached to kext (session: 0x%{public}08X)", mSHM->sessionID);
            } else {
                LogOzzyHALError("failed to map kext memory (0x%{public}x)", kr);
                IOServiceClose(mKextConnect);
                mKextConnect = IO_OBJECT_NULL;
            }
        }
    }

    // --- 2. Fallback to Daemon (shm_open) ---
    if (!mSHM) {
        mIsKextMapping = false;
        mSHMFd = shm_open(kOzzySharedMemName, O_RDWR, 0666);
        if (mSHMFd != -1) {
            struct stat sb;
            if (fstat(mSHMFd, &sb) == 0 && (size_t)sb.st_size >= sizeof(OzzySharedMemory)) {
                mSHMInode = sb.st_ino;
                void* ptr = mmap(NULL, sizeof(OzzySharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, mSHMFd, 0);
                if (ptr != MAP_FAILED) {
                    mSHM = (OzzySharedMemory*)ptr;
                    LogOzzyHAL("attached to daemon via shm_open");
                } else {
                    close(mSHMFd); mSHMFd = -1;
                }
            } else {
                close(mSHMFd); mSHMFd = -1;
            }
        }
    }

    // --- Validation & Config ---
    if (mSHM) {
        if (mSHM->magic != kOzzyMagic) {
            LogOzzyHALError("SHM magic mismatch");
            UnmapSharedMemory();
            return;
        }
        if (mSHM->version != kOzzyLayoutPackets && mSHM->version != kOzzyLayoutFrames) {
            LogOzzyHALError("unknown shm layout version %{public}u", mSHM->version);
            UnmapSharedMemory();
            return;
        }
        // The kext or daemon publishes its identity before the engine has run
        // init and described the rings; come back on the next poll.
        if (mSHM->version == kOzzyLayoutFrames && mSHM->audio.ringFramesOut == 0) {
            UnmapSharedMemory();
            return;
        }

        if (mSHM->vendorID == 0) {
            UnmapSharedMemory();
            return;
        }

        bool layoutOK = (mSHM->version == kOzzyLayoutFrames) ? ApplyFrameLayout() : ApplyPacketLayout();
        if (!layoutOK) {
            UnmapSharedMemory();
            return;
        }

        mLastSessionID = mSHM->sessionID;
        mInputBufferAddr = mSHM->audio.inputBuffer;
        mOutputBufferAddr = mSHM->audio.outputBuffer;
        mMappingDead.store(false);

        mLastHeartbeat = mSHM->heartbeat.load(std::memory_order_relaxed);
        mHeartbeatSeenAt = mach_absolute_time();

        if (mIsDynacord) {
            mEncode = nullptr;
            mDecode = nullptr;
            mWriteOutput = DynacordWriteOutput;
            mReadInput = DynacordReadInput;
            mClearOutput = DynacordClearOutput;
            LogOzzyHAL("using Dynacord codec at %{public}u Hz", mSHM->audio.sampleRate.load());
        } else {
            bool isBulk = (mSHM->audio.deviceFlags & 1);
            mEncode = PloytecEncodePCM;
            mDecode = PloytecDecodePCM;
            mWriteOutput = isBulk ? PloytecWriteOutputBulk : PloytecWriteOutputInterrupt;
            mReadInput = PloytecReadInput;
            mClearOutput = isBulk ? PloytecClearOutputBulk : PloytecClearOutputInterrupt;
            LogOzzyHAL("using Ploytec codec (%{public}s) for VID:0x%{public}04X",
                       isBulk ? "bulk" : "interrupt", mSHM->vendorID);
        }
    }
}

void OzzyHAL::UnmapSharedMemory() {
    if (!mSHM) return;

    // Hide the mapping from the IO path before it goes away.
    OzzySharedMemory* shm = mSHM;
    mMappingDead.store(true);
    mSHM = nullptr;
    mInputBufferAddr = nullptr;
    mOutputBufferAddr = nullptr;

    if (mIsKextMapping) {
        // Cleanup Kext
        mach_vm_address_t addr = mKextMapAddress;
        io_connect_t conn = mKextConnect;
        mKextMapAddress = 0;
        mKextConnect = IO_OBJECT_NULL;
        if (addr) IOConnectUnmapMemory(conn, 0, mach_task_self(), addr);
        if (conn != IO_OBJECT_NULL) IOServiceClose(conn);
    } else {
        // Cleanup Daemon
        int fd = mSHMFd;
        mSHMFd = -1;
        munmap(shm, sizeof(OzzySharedMemory));
        if (fd != -1) close(fd);
        mSHMInode = 0;
    }

    mIsKextMapping = false;
    mMappingDead.store(false);
    mLastAnchorSample = 0;
    mLastAnchorHost = 0;
}

void* OzzyHAL::MonitorEntry(void* arg) {
    ((OzzyHAL*)arg)->MonitorLoop();
    return NULL;
}

void OzzyHAL::MonitorLoop() {
    // Check initial state if already mapped
    if (mSHM) {
        bool hwReady = mSHM->audio.hardwarePresent.load();
        bool drvReady = mSHM->audio.driverReady.load();
        SHMName product = CopyName(mSHM->productName);
        SHMName manufacturer = CopyName(mSHM->manufacturerName);
        SHMName serial = CopyName(mSHM->serialNumber);
        bool metaReady = (product.s[0] != 0);
        bool initiallyConnected = (hwReady && drvReady && metaReady);
        
        if (initiallyConnected) {
            LogOzzyHAL("device already connected at startup");
            mLastConnectedState = true;
            
            // Update device metadata
            if (product.s[0] != 0) {
                if (mModelUID) CFRelease(mModelUID);
                mModelUID = SafeCreateString(product.s);
            }
            if (manufacturer.s[0] != 0) {
                if (mManufacturerUID) CFRelease(mManufacturerUID);
                mManufacturerUID = SafeCreateString(manufacturer.s);
            }
            if (serial.s[0] != 0) {
                if (mDeviceUID) CFRelease(mDeviceUID);
                char uidBuf[128];
                snprintf(uidBuf, sizeof(uidBuf), "OzzyHAL.%s", serial.s);
                mDeviceUID = SafeCreateString(uidBuf);
            }
            
            // Notify CoreAudio
            if (mHost) {
                AudioObjectPropertyAddress addr = { kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
                mHost->PropertiesChanged(mHost, kAudioObjectPlugInObject, 1, &addr);
            }
        }
    }
    
    while (mMonitorRunning) {
        if (!mSHM) {
            if (!mLoggedWaitingForDriver) {
                LogOzzyHAL("waiting for driver...");
                mLoggedWaitingForDriver = true;
            }
            MapSharedMemory();
            if (!mSHM) {
                usleep(100000); // 100ms
                continue;
            }
            mLoggedWaitingForDriver = false;
        } else if (mMappingDead.load()) {
            // Dropped while IO was running: unmap once CoreAudio has stopped it.
            if (mIOClients.load() == 0) UnmapSharedMemory();
        } else {
            // Check Health
            if (mIsKextMapping) {
                // Check if shared memory is still valid and hardware is present
                bool magicValid = (mSHM->magic == kOzzyMagic);
                bool hwPresent = magicValid && mSHM->audio.hardwarePresent.load(std::memory_order_relaxed);
                
                if (!magicValid || !hwPresent) {
                     LogOzzyHAL("device disconnected (magic: %{public}d, hw: %{public}d)", magicValid, hwPresent);
                     DropMapping();
                     // Don't continue - let state update happen below
                }
            } else {
                // If using Daemon, check inode and hardware presence
                struct stat sb;
                if (fstat(mSHMFd, &sb) != 0 || sb.st_nlink == 0 || sb.st_ino != mSHMInode) {
                    LogOzzyHALError("shared memory unlinked (daemon died)");
                    DropMapping();
                    // Don't continue - let state update happen below
                } else {
                    // Also check if hardware is still present
                    bool magicValid = (mSHM->magic == kOzzyMagic);
                    bool hwPresent = magicValid && mSHM->audio.hardwarePresent.load(std::memory_order_relaxed);

                    if (!magicValid || !hwPresent) {
                        LogOzzyHAL("device disconnected (magic: %{public}d, hw: %{public}d)", magicValid, hwPresent);
                        DropMapping();
                        // Don't continue - let state update happen below
                    }
                    if (mSHM && !mMappingDead.load() && mSHM->version == kOzzyLayoutFrames) {
                        uint32_t hb = mSHM->heartbeat.load(std::memory_order_relaxed);
                        uint64_t now = mach_absolute_time();
                        if (hb != mLastHeartbeat) {
                            mLastHeartbeat = hb;
                            mHeartbeatSeenAt = now;
                        } else if (now - mHeartbeatSeenAt > MsToHostTicks(500)) {
                            LogOzzyHALError("daemon heartbeat stopped");
                            DropMapping();
                        }
                    }
                    if (mSHM && !mMappingDead.load() && mSHM->sessionID != mLastSessionID) {
                        LogOzzyHAL("session changed: 0x%{public}08X -> 0x%{public}08X", mLastSessionID, mSHM->sessionID);
                        mLastSessionID = mSHM->sessionID;
                    }
                }
            }
        }

        bool live = (mSHM && !mMappingDead.load());
        SHMName product = {}, manufacturer = {}, serial = {};
        if (live) {
            product = CopyName(mSHM->productName);
            manufacturer = CopyName(mSHM->manufacturerName);
            serial = CopyName(mSHM->serialNumber);
        }
        bool hwReady = (live && mSHM->audio.hardwarePresent.load());
        bool drvReady = (live && mSHM->audio.driverReady.load());
        bool metaReady = (live && product.s[0] != 0);
        bool currentlyConnected = (hwReady && drvReady && metaReady);

        if (currentlyConnected != mLastConnectedState) {
            if (currentlyConnected) {
                // The engine may have restarted with new rings; re-read and re-check them.
                if (!(mIsDynacord ? ApplyFrameLayout() : ApplyPacketLayout())) {
                    DropMapping();
                    usleep(250000);
                    continue;
                }
                if (product.s[0] != 0) {
                    if (mModelUID) CFRelease(mModelUID);
                    mModelUID = SafeCreateString(product.s);
                }
                if (manufacturer.s[0] != 0) {
                    if (mManufacturerUID) CFRelease(mManufacturerUID);
                    mManufacturerUID = SafeCreateString(manufacturer.s);
                }
                if (serial.s[0] != 0) {
                    if (mDeviceUID) CFRelease(mDeviceUID);
                    char uidBuf[128];
                    snprintf(uidBuf, sizeof(uidBuf), "OzzyHAL.%s", serial.s);
                    mDeviceUID = SafeCreateString(uidBuf);
                }
                LogOzzyHAL("device ready: '%{public}s'", product.s);
            }

            mLastConnectedState = currentlyConnected;
            LogOzzyHAL("connection: %{public}s", currentlyConnected ? "UP" : "DOWN");
            
            // Stop I/O if device disconnected
            if (!currentlyConnected && mIOStarted) {
                mIOStarted = false;
                mTimestampSeed++;
                LogOzzyHAL("auto-stopped I/O due to disconnect");
            }
            
            if (mHost) {
                if (currentlyConnected) {
                    // Device connected - notify about device properties first, then add to list
                    AudioObjectPropertyAddress devAddr = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
                    mHost->PropertiesChanged(mHost, kOzzyDeviceID, 1, &devAddr);
                    devAddr.mSelector = kAudioObjectPropertyManufacturer;
                    mHost->PropertiesChanged(mHost, kOzzyDeviceID, 1, &devAddr);
                    devAddr.mSelector = kAudioDevicePropertyDeviceUID;
                    mHost->PropertiesChanged(mHost, kOzzyDeviceID, 1, &devAddr);
                    
                    AudioObjectPropertyAddress listAddr = { kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
                    mHost->PropertiesChanged(mHost, kAudioObjectPlugInObject, 1, &listAddr);
                } else {
                    // Device disconnected - notify that device is no longer alive, then remove from list
                    AudioObjectPropertyAddress aliveAddr = { kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
                    mHost->PropertiesChanged(mHost, kOzzyDeviceID, 1, &aliveAddr);
                    
                    AudioObjectPropertyAddress listAddr = { kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
                    mHost->PropertiesChanged(mHost, kAudioObjectPlugInObject, 1, &listAddr);
                }
            }
        }
        usleep(250000);
    }
}

bool OzzyHAL::IsConnected() { return mLastConnectedState; }

kern_return_t OzzyHAL::StartIO() {
    if (!IsConnected()) return kAudioHardwareNotRunningError;
    // Count first, then look at the mapping: the monitor marks it dead first
    // and then reads the count, so one of the two sides always sees the other.
    mIOClients.fetch_add(1);
    if (mMappingDead.load() || !mSHM) {
        mIOClients.fetch_sub(1);
        return kAudioHardwareNotRunningError;
    }
    mSHM->audio.halWritePosition.store(0, std::memory_order_relaxed);
    LogOzzyHAL("start I/O");
    mIOStarted = true;
    mTimestampSeed++;
    return kIOReturnSuccess;
}

kern_return_t OzzyHAL::StopIO() {
    LogOzzyHAL("stop I/O");
    mIOStarted = false;
    mTimestampSeed++;
    ClearOutputBuffer();
    // Never below zero.
    uint32_t n = mIOClients.load();
    while (n > 0 && !mIOClients.compare_exchange_weak(n, n - 1)) {}
    return kIOReturnSuccess;
}

HRESULT OzzyHAL::GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed) {
    if (!mSHM || mMappingDead.load()) return kAudioHardwareNotRunningError;

    auto& ts = mSHM->audio.timestamp;
    uint64_t sTime = 0, hTime = 0;
    bool got = false;

    for (int retries = 0; retries < 100; ++retries) {
        uint32_t seq1 = ts.sequence.load(std::memory_order_acquire);
        if (seq1 & 1) continue;   // writer mid-update: retry
        sTime = ts.sampleTime.load(std::memory_order_relaxed);
        hTime = ts.hostTime.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        uint32_t seq2 = ts.sequence.load(std::memory_order_relaxed);
        if (seq1 == seq2) { got = true; break; }
    }
    if (!got) {
        // Never got a consistent read: serve the last anchor we had.
        sTime = mLastAnchorSample;
        hTime = mLastAnchorHost;
    }

    if (hTime == 0 && mLastAnchorHost != 0) {
        // The engine is restarting and has no anchor yet: keep serving the last one.
        sTime = mLastAnchorSample;
        hTime = mLastAnchorHost;
    } else if (hTime != 0) {
        if (sTime < mLastAnchorSample) mTimestampSeed++;   // a new timeline started
        mLastAnchorSample = sTime;
        mLastAnchorHost = hTime;
    }
    if (outSampleTime) *outSampleTime = (Float64)sTime;
    if (outHostTime) *outHostTime = hTime;
    if (outSeed) *outSeed = mTimestampSeed;
    
    return kAudioHardwareNoError;
}

OSStatus OzzyHAL::ioOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer) {
    if (!mSHM || mMappingDead.load() || !mInputBufferAddr || !mOutputBufferAddr) return kAudioHardwareNoError;

    if (inOperationID == kAudioServerPlugInIOOperationWriteMix) {
        if (mWriteOutput) {
            uint64_t sampleTime = (uint64_t)inIOCycleInfo->mOutputTime.mSampleTime;
            mWriteOutput(mOutputBufferAddr, (const float*)ioMainBuffer, sampleTime, inIOBufferFrameSize, mRingSize, mOutputBytesPerFrame);
            mSHM->audio.halWritePosition.store(sampleTime + inIOBufferFrameSize, std::memory_order_release);
        }
    }
    else if (inOperationID == kAudioServerPlugInIOOperationReadInput) {
        if (mReadInput) {
            uint64_t sampleTime = (uint64_t)inIOCycleInfo->mInputTime.mSampleTime;
            mReadInput((float*)ioMainBuffer, mInputBufferAddr, sampleTime, inIOBufferFrameSize, mRingSizeIn, mInputBytesPerFrame);
        }
    }
    return kAudioHardwareNoError;
}

CFStringRef OzzyHAL::GetDeviceName() const { return mModelUID; }
CFStringRef OzzyHAL::GetManufacturer() const { return mManufacturerUID; }
CFStringRef OzzyHAL::GetDeviceUID() const { return mDeviceUID; }
CFStringRef OzzyHAL::GetModelUID() const { return mModelUID; }
UInt32 OzzyHAL::GetTransportType() const { return kAudioDeviceTransportTypeUSB; }
UInt32 OzzyHAL::GetClockDomain() const { return 0x504C4F59; }
Float64 OzzyHAL::GetNominalSampleRate() const { return mCurrentStreamFormat.mSampleRate; }
AudioValueRange OzzyHAL::GetAvailableSampleRates() const { return mAvailableSampleRates; }
AudioBufferList* OzzyHAL::GetInputStreamConfiguration() const { return const_cast<AudioBufferList*>(&mInputConfig); }
AudioBufferList* OzzyHAL::GetOutputStreamConfiguration() const { return const_cast<AudioBufferList*>(&mOutputConfig); }
AudioObjectID OzzyHAL::GetInputStreamID() const { return kOzzyInputStreamID; }
AudioObjectID OzzyHAL::GetOutputStreamID() const { return kOzzyOutputStreamID; }
AudioStreamBasicDescription OzzyHAL::GetStreamFormat() const { return mCurrentStreamFormat; }
AudioStreamRangedDescription OzzyHAL::GetStreamRangedDescription() const {
    AudioStreamRangedDescription outDesc; outDesc.mFormat = GetStreamFormat();
    outDesc.mSampleRateRange.mMinimum = mCurrentStreamFormat.mSampleRate; outDesc.mSampleRateRange.mMaximum = mCurrentStreamFormat.mSampleRate;
    return outDesc;
}
UInt32 OzzyHAL::GetZeroTimestampPeriod() const { return mZeroTimestampPeriod; }

AudioChannelLayout* OzzyHAL::GetPreferredChannelLayout(AudioObjectPropertyScope inScope) {
    AudioChannelLayout* layout = (AudioChannelLayout*)mChannelLayoutBuffer;
    layout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
    layout->mChannelBitmap = 0;
    layout->mNumberChannelDescriptions = mChannels;
    for (UInt32 i = 0; i < mChannels; ++i) {
        layout->mChannelDescriptions[i].mChannelLabel = kAudioChannelLabel_Unknown;
        layout->mChannelDescriptions[i].mChannelFlags = kAudioChannelFlags_AllOff;
    }
    return layout;
}

UInt32 OzzyHAL::GetPreferredChannelLayoutSize(AudioObjectPropertyScope inScope) const {
    return offsetof(AudioChannelLayout, mChannelDescriptions) + (mChannels * sizeof(AudioChannelDescription));
}

extern "C" HRESULT OzzyInitialize(AudioServerPlugInDriverRef, AudioServerPlugInHostRef inHost) {
    OzzyHAL::Get().Init(inHost);
    return kAudioHardwareNoError;
}

extern "C" HRESULT OzzyCreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo*, AudioObjectID*) { return kAudioHardwareNoError; }
extern "C" HRESULT OzzyDestroyDevice(AudioServerPlugInDriverRef, AudioObjectID) { return kAudioHardwareNoError; }
extern "C" HRESULT OzzyAddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*) { return kAudioHardwareNoError; }
extern "C" HRESULT OzzyRemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*) { return kAudioHardwareNoError; }
extern "C" HRESULT OzzyPerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*) { return kAudioHardwareNoError; }
extern "C" HRESULT OzzyAbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*) { return kAudioHardwareNoError; }

extern "C" Boolean OzzyHasProperty(AudioServerPlugInDriverRef, AudioObjectID inObjectID, pid_t, const AudioObjectPropertyAddress* inAddress) {
    if (inObjectID == kAudioObjectPlugInObject) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass:
            case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer:
            case kAudioPlugInPropertyDeviceList: case kAudioObjectPropertyOwnedObjects:
            case kAudioObjectPropertyControlList: return true;
        }
        return false;
    }
    if (inObjectID == kOzzyDeviceID) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass:
            case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer:
            case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID:
            case kAudioDevicePropertyTransportType: case kAudioDevicePropertyIsHidden:
            case kAudioDevicePropertyDeviceIsAlive: case kAudioDevicePropertyDeviceIsRunning:
            case kAudioDevicePropertyDeviceIsRunningSomewhere: case kAudioDevicePropertyNominalSampleRate:
            case kAudioDevicePropertyAvailableNominalSampleRates: case kAudioDevicePropertyStreams:
            case kAudioDevicePropertyStreamConfiguration: case kAudioDevicePropertyLatency:
            case kAudioDevicePropertySafetyOffset: case kAudioDevicePropertyPreferredChannelLayout:
            case kAudioDevicePropertyZeroTimeStampPeriod: return true;
        }
        return false;
    }
    if (inObjectID == kOzzyOutputStreamID || inObjectID == kOzzyInputStreamID) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass:
            case kAudioObjectPropertyName: case kAudioObjectPropertyOwner:
            case kAudioStreamPropertyDirection: case kAudioStreamPropertyTerminalType:
            case kAudioStreamPropertyStartingChannel: case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat: case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats: return true;
        }
        return false;
    }
    return false;
}

extern "C" HRESULT OzzyIsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, Boolean* outIsSettable) {
    *outIsSettable = false; return kAudioHardwareNoError;
}

extern "C" HRESULT OzzyGetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID inObjectID, pid_t, const AudioObjectPropertyAddress* inAddress, UInt32, const void*, UInt32* outDataSize) {
    if (!outDataSize) return kAudioHardwareIllegalOperationError;
    *outDataSize = 0;

    if (inObjectID == kAudioObjectPlugInObject) {
        switch (inAddress->mSelector) {
            case kAudioPlugInPropertyDeviceList: case kAudioObjectPropertyOwnedObjects:
                *outDataSize = OzzyHAL::Get().IsConnected() ? sizeof(AudioObjectID) : 0; return kAudioHardwareNoError;
            case kAudioObjectPropertyControlList: *outDataSize = 0; return kAudioHardwareNoError;
            case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer: *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
            case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
            default: return kAudioHardwareUnknownPropertyError;
        }
    }
    if (inObjectID == kOzzyDeviceID) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer:
            case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID:
                *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
            case kAudioObjectPropertyControlList: *outDataSize = 0; return kAudioHardwareNoError;
            case kAudioObjectPropertyOwner: *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
            case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
            case kAudioDevicePropertyTransportType: case kAudioDevicePropertyIsHidden:
            case kAudioDevicePropertyDeviceIsAlive: case kAudioDevicePropertyDeviceIsRunning:
            case kAudioDevicePropertyDeviceIsRunningSomewhere: case kAudioDevicePropertyNominalSampleRate:
                *outDataSize = sizeof(Float64); return kAudioHardwareNoError;
            case kAudioDevicePropertyAvailableNominalSampleRates: *outDataSize = sizeof(AudioValueRange); return kAudioHardwareNoError;
            case kAudioDevicePropertyStreams:
                if (inAddress->mScope == kAudioObjectPropertyScopeInput) *outDataSize = sizeof(AudioObjectID);
                else if (IsOutputScope(inAddress->mScope)) *outDataSize = sizeof(AudioObjectID);
                else if (inAddress->mScope == kAudioObjectPropertyScopeGlobal) *outDataSize = 2 * sizeof(AudioObjectID);
                else *outDataSize = 0;
                return kAudioHardwareNoError;
            case kAudioDevicePropertyStreamConfiguration:
                *outDataSize = GetAudioBufferListSize((IsOutputScope(inAddress->mScope) ? OzzyHAL::Get().GetOutputStreamConfiguration() : OzzyHAL::Get().GetInputStreamConfiguration())->mNumberBuffers);
                return kAudioHardwareNoError;
            case kAudioDevicePropertySafetyOffset: case kAudioDevicePropertyLatency: case kAudioDevicePropertyZeroTimeStampPeriod: *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
            case kAudioDevicePropertyPreferredChannelLayout: *outDataSize = OzzyHAL::Get().GetPreferredChannelLayoutSize(inAddress->mScope); return kAudioHardwareNoError;
            default: return kAudioHardwareUnknownPropertyError;
        }
    }
    if (inObjectID == kOzzyOutputStreamID || inObjectID == kOzzyInputStreamID) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
            case kAudioObjectPropertyName: *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
            case kAudioObjectPropertyOwner: *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
            case kAudioStreamPropertyDirection: case kAudioStreamPropertyTerminalType: case kAudioStreamPropertyStartingChannel: case kAudioStreamPropertyLatency:
                *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
            case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat: *outDataSize = sizeof(AudioStreamBasicDescription); return kAudioHardwareNoError;
            case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats: *outDataSize = sizeof(AudioStreamRangedDescription); return kAudioHardwareNoError;
            default: return kAudioHardwareUnknownPropertyError;
        }
    }
    return kAudioHardwareBadObjectError;
}

extern "C" HRESULT OzzyGetPropertyData(AudioServerPlugInDriverRef, AudioObjectID inObjectID, pid_t, const AudioObjectPropertyAddress* inAddress, UInt32, const void*, UInt32 inMax, UInt32* outSize, void* outData) {
    if (inObjectID == kAudioObjectPlugInObject) {
        switch (inAddress->mSelector) {
            case kAudioPlugInPropertyDeviceList: case kAudioObjectPropertyOwnedObjects:
                if (!OzzyHAL::Get().IsConnected()) { if (outSize) *outSize = 0; return kAudioHardwareNoError; }
                return WriteObjectID(inMax, outSize, outData, kOzzyDeviceID);
            case kAudioObjectPropertyControlList: if (outSize) *outSize = 0; return kAudioHardwareNoError;
            case kAudioObjectPropertyName: return WriteCFString(inMax, outSize, outData, CFSTR("Ozzy HAL"));
            case kAudioObjectPropertyManufacturer: return WriteCFString(inMax, outSize, outData, CFSTR("Ozzy"));
            case kAudioObjectPropertyBaseClass: return WriteClassID(inMax, outSize, outData, kAudioObjectClassID);
            case kAudioObjectPropertyClass: return WriteClassID(inMax, outSize, outData, kAudioPlugInClassID);
            default: return kAudioHardwareUnknownPropertyError;
        }
    }
    if (inObjectID == kOzzyDeviceID) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass: return WriteClassID(inMax, outSize, outData, kAudioObjectClassID);
            case kAudioObjectPropertyClass: return WriteClassID(inMax, outSize, outData, kAudioDeviceClassID);
            case kAudioObjectPropertyControlList: if (outSize) *outSize = 0; return kAudioHardwareNoError;
            case kAudioObjectPropertyOwner: return WriteObjectID(inMax, outSize, outData, kAudioObjectPlugInObject);
            case kAudioObjectPropertyName: return WriteCFString(inMax, outSize, outData, OzzyHAL::Get().GetDeviceName());
            case kAudioObjectPropertyManufacturer: return WriteCFString(inMax, outSize, outData, OzzyHAL::Get().GetManufacturer());
            case kAudioDevicePropertyDeviceUID: return WriteCFString(inMax, outSize, outData, OzzyHAL::Get().GetDeviceUID());
            case kAudioDevicePropertyModelUID: return WriteCFString(inMax, outSize, outData, OzzyHAL::Get().GetModelUID());
            case kAudioDevicePropertyTransportType: return WriteUInt32(inMax, outSize, outData, OzzyHAL::Get().GetTransportType());
            case kAudioDevicePropertyIsHidden: return WriteUInt32(inMax, outSize, outData, 0);
            case kAudioDevicePropertyDeviceIsAlive: return WriteUInt32(inMax, outSize, outData, OzzyHAL::Get().IsConnected() ? 1 : 0);
            case kAudioDevicePropertyDeviceIsRunning: case kAudioDevicePropertyDeviceIsRunningSomewhere: return WriteUInt32(inMax, outSize, outData, OzzyHAL::Get().IsRunning() ? 1 : 0);
            case kAudioDevicePropertyNominalSampleRate: return WriteFloat64(inMax, outSize, outData, OzzyHAL::Get().GetNominalSampleRate());
            case kAudioDevicePropertyAvailableNominalSampleRates: if (outSize) *outSize = sizeof(AudioValueRange); if (outData) { if (inMax < sizeof(AudioValueRange)) return kAudioHardwareBadPropertySizeError; *(AudioValueRange*)outData = OzzyHAL::Get().GetAvailableSampleRates(); } return kAudioHardwareNoError;
            case kAudioDevicePropertyStreams: {
                UInt32 streamList[] = { kOzzyInputStreamID, kOzzyOutputStreamID };
                if (inAddress->mScope == kAudioObjectPropertyScopeInput) return WriteObjectID(inMax, outSize, outData, kOzzyInputStreamID);
                else if (inAddress->mScope == kAudioObjectPropertyScopeOutput) return WriteObjectID(inMax, outSize, outData, kOzzyOutputStreamID);
                else { if (outData && inMax < sizeof(streamList)) return kAudioHardwareBadPropertySizeError; if (outData) memcpy(outData, streamList, sizeof(streamList)); if (outSize) *outSize = sizeof(streamList); return kAudioHardwareNoError; }
            }
            case kAudioObjectPropertyOwnedObjects: {
                if (inMax < 2 * sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                AudioObjectID* list = (AudioObjectID*)outData; if (list) { list[0] = kOzzyOutputStreamID; list[1] = kOzzyInputStreamID; }
                if (outSize) *outSize = 2 * sizeof(AudioObjectID); return kAudioHardwareNoError;
            }
            case kAudioDevicePropertyStreamConfiguration: {
                AudioBufferList* config = (inAddress->mScope == kAudioObjectPropertyScopeInput) ? OzzyHAL::Get().GetInputStreamConfiguration() : OzzyHAL::Get().GetOutputStreamConfiguration();
                UInt32 actualSize = GetAudioBufferListSize(config->mNumberBuffers); if (outSize) *outSize = actualSize;
                if (outData) { if (inMax < actualSize) return kAudioHardwareBadPropertySizeError; memcpy(outData, config, actualSize); } return kAudioHardwareNoError;
            }
            case kAudioDevicePropertyClockDomain: return WriteUInt32(inMax, outSize, outData, OzzyHAL::Get().GetClockDomain());
            case kAudioDevicePropertyLatency: return WriteUInt32(inMax, outSize, outData, OzzyHAL::Get().GetLatency());
            case kAudioDevicePropertySafetyOffset: return WriteUInt32(inMax, outSize, outData, OzzyHAL::Get().GetSafetyOffset(inAddress->mScope));
            case kAudioDevicePropertyZeroTimeStampPeriod: return WriteUInt32(inMax, outSize, outData, OzzyHAL::Get().GetZeroTimestampPeriod());
            case kAudioDevicePropertyPreferredChannelLayout: {
                UInt32 size = OzzyHAL::Get().GetPreferredChannelLayoutSize(inAddress->mScope);
                if (outSize) *outSize = size;
                if (outData) { if (inMax < size) return kAudioHardwareBadPropertySizeError; memcpy(outData, OzzyHAL::Get().GetPreferredChannelLayout(inAddress->mScope), size); }
                return kAudioHardwareNoError;
            }
            default: return kAudioHardwareUnknownPropertyError;
        }
    }
    if (inObjectID == kOzzyOutputStreamID || inObjectID == kOzzyInputStreamID) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass: return WriteClassID(inMax, outSize, outData, kAudioObjectClassID);
            case kAudioObjectPropertyClass: return WriteClassID(inMax, outSize, outData, kAudioStreamClassID);
            case kAudioObjectPropertyName: return WriteCFString(inMax, outSize, outData, (inObjectID == kOzzyOutputStreamID ? CFSTR("Ozzy PCM Output Stream") : CFSTR("Ozzy PCM Input Stream")));
            case kAudioObjectPropertyOwner: return WriteObjectID(inMax, outSize, outData, kOzzyDeviceID);
            case kAudioStreamPropertyDirection: return WriteUInt32(inMax, outSize, outData, (inObjectID == kOzzyOutputStreamID ? 0 : 1));
            case kAudioStreamPropertyTerminalType: return WriteUInt32(inMax, outSize, outData, kAudioStreamTerminalTypeSpeaker);
            case kAudioStreamPropertyStartingChannel: return WriteUInt32(inMax, outSize, outData, 1);
            case kAudioStreamPropertyLatency: return WriteUInt32(inMax, outSize, outData, OzzyHAL::Get().GetLatency());
            case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat:
                if (outData && inMax < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
                if (outData) *(AudioStreamBasicDescription*)outData = OzzyHAL::Get().GetStreamFormat();
                if (outSize) *outSize = sizeof(AudioStreamBasicDescription); return kAudioHardwareNoError;
            case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats:
                if (outData && inMax < sizeof(AudioStreamRangedDescription)) return kAudioHardwareBadPropertySizeError;
                if (outData) *(AudioStreamRangedDescription*)outData = OzzyHAL::Get().GetStreamRangedDescription();
                if (outSize) *outSize = sizeof(AudioStreamRangedDescription); return kAudioHardwareNoError;
            default: return kAudioHardwareUnknownPropertyError;
        }
    }
    return kAudioHardwareBadObjectError;
}

extern "C" HRESULT OzzySetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, const void*) { return kAudioHardwareUnsupportedOperationError; }
extern "C" HRESULT OzzyStartIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32) { return OzzyHAL::Get().StartIO(); }
extern "C" HRESULT OzzyStopIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32) { return OzzyHAL::Get().StopIO(); }
extern "C" HRESULT OzzyGetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed) { return OzzyHAL::Get().GetZeroTimeStamp(inDriver, inDeviceObjectID, inClientID, outSampleTime, outHostTime, outSeed); }
extern "C" OSStatus OzzyWillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32 inOperationID, Boolean* outWillDo, Boolean* outWillDoInPlace) { bool willDo = false; bool willDoInPlace = true; switch (inOperationID) { case kAudioServerPlugInIOOperationWriteMix: case kAudioServerPlugInIOOperationReadInput: willDo = true; break; default: willDo = false; break; } if (outWillDo) *outWillDo = willDo; if (outWillDoInPlace) *outWillDoInPlace = willDoInPlace; return kAudioHardwareNoError; }
extern "C" OSStatus OzzyBeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*) { return 0; }
extern "C" OSStatus OzzyDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer) { return OzzyHAL::Get().ioOperation(inDriver, inDeviceObjectID, inStreamObjectID, inClientID, inOperationID, inIOBufferFrameSize, inIOCycleInfo, ioMainBuffer, ioSecondaryBuffer); }
extern "C" OSStatus OzzyEndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*) { return 0; }
extern "C" HRESULT OzzyQueryInterface(void*, REFIID inREFIID, LPVOID* outInterface) { extern AudioServerPlugInDriverRef gOzzyDriverRef; CFUUIDBytes bytes = CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID); if (outInterface && memcmp(&inREFIID, &bytes, sizeof(REFIID)) == 0) { *outInterface = gOzzyDriverRef; return kAudioHardwareNoError; } return kAudioHardwareUnknownPropertyError; }
extern "C" ULONG OzzyAddRef(void*) { return 1; }
extern "C" ULONG OzzyRelease(void*) { return 1; }

static AudioServerPlugInDriverInterface gOzzyInterface = { NULL, OzzyQueryInterface, OzzyAddRef, OzzyRelease, OzzyInitialize, OzzyCreateDevice, OzzyDestroyDevice, OzzyAddDeviceClient, OzzyRemoveDeviceClient, OzzyPerformDeviceConfigurationChange, OzzyAbortDeviceConfigurationChange, OzzyHasProperty, OzzyIsPropertySettable, OzzyGetPropertyDataSize, OzzyGetPropertyData, OzzySetPropertyData, OzzyStartIO, OzzyStopIO, OzzyGetZeroTimeStamp, OzzyWillDoIOOperation, OzzyBeginIOOperation, OzzyDoIOOperation, OzzyEndIOOperation };
static AudioServerPlugInDriverInterface* gOzzyInterfacePtr = &gOzzyInterface;
AudioServerPlugInDriverRef gOzzyDriverRef = &gOzzyInterfacePtr;

extern "C" __attribute__((visibility("default"))) void* OzzyPluginFactory(CFAllocatorRef, CFUUIDRef inRequestedTypeUUID) {
	if (CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID)) return (void*)gOzzyDriverRef;
	return NULL;
}