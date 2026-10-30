#include "OzzyHAL.h"
#include "../Devices/Ploytec/PloytecCodec.h"
#include "../Devices/Ploytec/PloytecProtocol.h"
#include "../Shared/OzzyLog.h"
#include "../Devices/Dynacord/DynacordCodec.h"
#include "../Devices/Dynacord/DynacordRing.h"
#include "OzzyVolume.h"
#include <algorithm>
#include <cstring>
#include <mach/mach_time.h>
#include <CoreAudio/CoreAudio.h>
#include <SystemConfiguration/SystemConfiguration.h>

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

/* The daemon's segment is world-writable by name, so any local user could
 * create it first. Only map one owned by the console user, who runs the
 * daemon. */
static bool OwnedByConsoleUser(const struct stat& sb) {
    uid_t uid = 0;
    CFStringRef name = SCDynamicStoreCopyConsoleUser(NULL, &uid, NULL);
    if (!name) return false;
    CFRelease(name);
    return sb.st_uid == uid;
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

static OSStatus WriteFloat32(UInt32 inMax, UInt32* outSize, void* outData, Float32 v) {
    if (outData && inMax < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
    if (outData) *(Float32*)outData = v;
    if (outSize) *outSize = sizeof(Float32);
    return kAudioHardwareNoError;
}

static OSStatus WriteObjectIDs(UInt32 inMax, UInt32* outSize, void* outData, const AudioObjectID* ids, UInt32 count) {
    UInt32 n = outData ? std::min<UInt32>(inMax / sizeof(AudioObjectID), count) : count;
    if (outData) memcpy(outData, ids, n * sizeof(AudioObjectID));
    if (outSize) *outSize = n * sizeof(AudioObjectID);
    return kAudioHardwareNoError;
}

static const AudioObjectID kOzzyControls[] = { kOzzyVolumeControlID, kOzzyMuteControlID };
static const AudioObjectID kOzzyDeviceOwned[] = { kOzzyOutputStreamID, kOzzyInputStreamID, kOzzyVolumeControlID, kOzzyMuteControlID };

static bool IsControl(AudioObjectID obj) { return obj == kOzzyVolumeControlID || obj == kOzzyMuteControlID; }

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
    LoadControlState();

    mDeviceUID = CFSTR("OzzyHAL.device");
    mModelUID = CFSTR("Ozzy Audio Device");
    mManufacturerUID = CFSTR("Ozzy");
    RebuildDeviceStrings();
    mZeroTimestampPeriod = 640;

    mCurrentStreamFormat.mSampleRate = 96000.0;
    mCurrentStreamFormat.mFormatID = kAudioFormatLinearPCM;
    mCurrentStreamFormat.mFormatFlags = kAudioFormatFlagIsFloat;
    mCurrentStreamFormat.mBytesPerPacket = 32;
    mCurrentStreamFormat.mFramesPerPacket = 1;
    mCurrentStreamFormat.mBytesPerFrame = 32;
    mCurrentStreamFormat.mChannelsPerFrame = 8;
    mCurrentStreamFormat.mBitsPerChannel = 32;

    mNumSampleRates = 1;
    mSampleRateList[0] = { 96000.0, 96000.0 };

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

static const CFStringRef kVolumeStorageKey = CFSTR("output volume");
static const CFStringRef kMuteStorageKey = CFSTR("output mute");
static const CFStringRef kInputStereoPairStorageKey = CFSTR("input stereo pair");
static const CFStringRef kOutputStereoPairStorageKey = CFSTR("output stereo pair");

/* Reads a stereo pair stored as an array of two channel numbers. */
static bool CopyStereoPairFromStorage(AudioServerPlugInHostRef host, CFStringRef key, OzzyStereoPair* outPair) {
    CFPropertyListRef v = nullptr;
    if (host->CopyFromStorage(host, key, &v) != kAudioHardwareNoError || !v) return false;
    bool ok = false;
    if (CFGetTypeID(v) == CFArrayGetTypeID() && CFArrayGetCount((CFArrayRef)v) == 2) {
        SInt32 ch[2];
        ok = true;
        for (CFIndex i = 0; i < 2; i++) {
            CFTypeRef n = CFArrayGetValueAtIndex((CFArrayRef)v, i);
            ok = ok && CFGetTypeID(n) == CFNumberGetTypeID() && CFNumberGetValue((CFNumberRef)n, kCFNumberSInt32Type, &ch[i]) && ch[i] > 0;
        }
        if (ok) *outPair = { (uint32_t)ch[0], (uint32_t)ch[1] };
    }
    CFRelease(v);
    return ok;
}

static void WriteStereoPairToStorage(AudioServerPlugInHostRef host, CFStringRef key, OzzyStereoPair pair) {
    SInt32 ch[2] = { (SInt32)pair.left, (SInt32)pair.right };
    CFNumberRef n[2] = { CFNumberCreate(NULL, kCFNumberSInt32Type, &ch[0]), CFNumberCreate(NULL, kCFNumberSInt32Type, &ch[1]) };
    if (n[0] && n[1]) {
        CFArrayRef a = CFArrayCreate(NULL, (const void**)n, 2, &kCFTypeArrayCallBacks);
        if (a) { host->WriteToStorage(host, key, a); CFRelease(a); }
    }
    if (n[0]) CFRelease(n[0]);
    if (n[1]) CFRelease(n[1]);
}

/* Restores the volume and mute last set, from the host's per-plug-in storage. */
void OzzyHAL::LoadControlState() {
    if (!mHost) return;
    CFPropertyListRef v = nullptr;
    if (mHost->CopyFromStorage(mHost, kVolumeStorageKey, &v) == kAudioHardwareNoError && v) {
        Float32 f;
        if (CFGetTypeID(v) == CFNumberGetTypeID() && CFNumberGetValue((CFNumberRef)v, kCFNumberFloat32Type, &f))
            mVolumeScalar.store(OzzyVolumeClampScalar(f));
        CFRelease(v);
    }
    v = nullptr;
    if (mHost->CopyFromStorage(mHost, kMuteStorageKey, &v) == kAudioHardwareNoError && v) {
        if (CFGetTypeID(v) == CFBooleanGetTypeID())
            mMuted.store(CFBooleanGetValue((CFBooleanRef)v));
        CFRelease(v);
    }
    LogOzzyHAL("output volume %{public}.3f%{public}s", mVolumeScalar.load(), mMuted.load() ? " (muted)" : "");

    OzzyStereoPair pair;
    if (CopyStereoPairFromStorage(mHost, kInputStereoPairStorageKey, &pair)) mStereoPairIn.store(OzzyStereoPairPack(pair));
    if (CopyStereoPairFromStorage(mHost, kOutputStereoPairStorageKey, &pair)) mStereoPairOut.store(OzzyStereoPairPack(pair));
}

void OzzyHAL::SaveControlState() {
    if (!mHost) return;
    Float32 f = mVolumeScalar.load();
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberFloat32Type, &f);
    if (n) { mHost->WriteToStorage(mHost, kVolumeStorageKey, n); CFRelease(n); }
    mHost->WriteToStorage(mHost, kMuteStorageKey, mMuted.load() ? kCFBooleanTrue : kCFBooleanFalse);
}

void OzzyHAL::NotifyControlChanged(AudioObjectID control, const AudioObjectPropertySelector* selectors, UInt32 count) {
    if (!mHost) return;
    AudioObjectPropertyAddress addrs[2];
    count = std::min<UInt32>(count, 2);
    for (UInt32 i = 0; i < count; i++)
        addrs[i] = { selectors[i], kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    mHost->PropertiesChanged(mHost, control, count, addrs);
}

void OzzyHAL::NotifyDeviceChanged(AudioObjectID device, AudioObjectPropertySelector selector, AudioObjectPropertyScope scope) {
    if (!mHost) return;
    AudioObjectPropertyAddress addr = { selector, scope, kAudioObjectPropertyElementMain };
    mHost->PropertiesChanged(mHost, device, 1, &addr);
}

/* Tells the host a global property changed on every device the attached hardware publishes. */
void OzzyHAL::NotifyAllDevicesChanged(AudioObjectPropertySelector selector) {
    for (UInt32 i = 0; i < OzzyDeviceCount(true, mIsDynacord); i++)
        NotifyDeviceChanged(kOzzyDevices[i].device, selector, kAudioObjectPropertyScopeGlobal);
}

/* Takes the product, manufacturer and serial number the engine published,
 * leaving any that are empty as they were. */
void OzzyHAL::SetIdentity(const char* product, const char* manufacturer, const char* serial) {
    if (product[0] != 0) {
        if (mModelUID) CFRelease(mModelUID);
        mModelUID = SafeCreateString(product);
    }
    if (manufacturer[0] != 0) {
        if (mManufacturerUID) CFRelease(mManufacturerUID);
        mManufacturerUID = SafeCreateString(manufacturer);
    }
    if (serial[0] != 0) {
        if (mDeviceUID) CFRelease(mDeviceUID);
        char uidBuf[128];
        snprintf(uidBuf, sizeof(uidBuf), "OzzyHAL.%s", serial);
        mDeviceUID = SafeCreateString(uidBuf);
    }
    RebuildDeviceStrings();
}

/* Derives each device's UID and model UID from the main device's, so the
 * mono devices keep a stable UID across replugs. */
void OzzyHAL::RebuildDeviceStrings() {
    for (UInt32 i = 0; i < kOzzyMaxDevices; i++) {
        DeviceState& d = mDevices[i];
        if (!d.name && kOzzyDevices[i].name) d.name = SafeCreateString(kOzzyDevices[i].name);
        if (d.uid) CFRelease(d.uid);
        if (d.model) CFRelease(d.model);
        d.uid = mDeviceUID ? CFStringCreateWithFormat(NULL, NULL, CFSTR("%@%s"), mDeviceUID, kOzzyDevices[i].uidSuffix) : nullptr;
        d.model = mModelUID ? CFStringCreateWithFormat(NULL, NULL, CFSTR("%@%s"), mModelUID, kOzzyDevices[i].uidSuffix) : nullptr;
    }
}

void OzzyHAL::SetVolumeScalar(Float32 scalar) {
    scalar = OzzyVolumeClampScalar(scalar);
    if (mVolumeScalar.exchange(scalar) == scalar) return;
    static const AudioObjectPropertySelector changed[] = { kAudioLevelControlPropertyScalarValue, kAudioLevelControlPropertyDecibelValue };
    NotifyControlChanged(kOzzyVolumeControlID, changed, 2);
    SaveControlState();
}

void OzzyHAL::SetMuted(bool muted) {
    if (mMuted.exchange(muted) == muted) return;
    static const AudioObjectPropertySelector changed[] = { kAudioBooleanControlPropertyValue };
    NotifyControlChanged(kOzzyMuteControlID, changed, 1);
    SaveControlState();
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

    mMonoInputConfig.mNumberBuffers = 1;
    mMonoInputConfig.mBuffers[0].mNumberChannels = 1;
    mMonoInputConfig.mBuffers[0].mDataByteSize = kDefaultIOFrames * sizeof(Float32);
    mMonoInputConfig.mBuffers[0].mData = nullptr;

    mNoConfig.mNumberBuffers = 0;
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
    mNumSampleRates = 1;
    mSampleRateList[0] = { 96000.0, 96000.0 };
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
    mNumSampleRates = 3;
    mSampleRateList[0] = { 44100.0, 44100.0 };
    mSampleRateList[1] = { 48000.0, 48000.0 };
    mSampleRateList[2] = { 96000.0, 96000.0 };
    mCurrentStreamFormat.mChannelsPerFrame = mChannels;
    mCurrentStreamFormat.mBytesPerFrame = mCurrentStreamFormat.mBytesPerPacket = 4 * mChannels;
    RebuildStreamConfigs();
    return true;
}

/* True if the segment still describes the frame layout the HAL is using. */
bool OzzyHAL::FrameLayoutUnchanged() const {
    return mSHM->audio.sampleRate.load() == mCurrentStreamFormat.mSampleRate &&
           mSHM->audio.ringFramesOut == mRingSize &&
           mSHM->audio.ringFramesIn == mRingSizeIn &&
           mSHM->audio.zeroTimestampPeriod == mZeroTimestampPeriod &&
           mSHM->audio.safetyOffsetOut == mSafetyOffsetOut &&
           mSHM->audio.safetyOffsetIn == mSafetyOffsetIn;
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
            bool sized = fstat(mSHMFd, &sb) == 0 && (size_t)sb.st_size >= sizeof(OzzySharedMemory);
            if (sized && OwnedByConsoleUser(sb)) {
                void* ptr = mmap(NULL, sizeof(OzzySharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, mSHMFd, 0);
                if (ptr != MAP_FAILED) {
                    mSHM = (OzzySharedMemory*)ptr;
                    LogOzzyHAL("attached to daemon via shm_open");
                } else {
                    close(mSHMFd); mSHMFd = -1;
                }
            } else {
                // Logged once per owner, not on every 100 ms retry.
                static uid_t lastRefused = (uid_t)-1;
                if (sized && sb.st_uid != lastRefused) {
                    LogOzzyHALError("ignoring /ozzy_shared_mem owned by uid %{public}u, not the console user", sb.st_uid);
                    lastRefused = sb.st_uid;
                }
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
    }

    mIsKextMapping = false;
    mMappingDead.store(false);
    for (DeviceState& d : mDevices) d.anchorSample = d.anchorHost = 0;
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
            SetIdentity(product.s, manufacturer.s, serial.s);
            
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
                // If using Daemon, check the fd and hardware presence. macOS reports
                // st_nlink and st_ino as 0 for POSIX shm, so a daemon exit is seen
                // through magic and the heartbeat watchdog.
                struct stat sb;
                if (fstat(mSHMFd, &sb) != 0) {
                    LogOzzyHALError("shared memory fd no longer valid");
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
                }
            }
            if (mSHM && !mMappingDead.load() && mSHM->sessionID != mLastSessionID) {
                LogOzzyHAL("session changed: 0x%{public}08X -> 0x%{public}08X", mLastSessionID, mSHM->sessionID);
                mLastSessionID = mSHM->sessionID;
            }
            // The engine publishes a restart's layout some time after its new
            // session, and a rate change it applied late (after
            // PerformDeviceConfigurationChange gave up) brings a new one: check every
            // poll and reattach so the DOWN/UP path re-applies it. A read torn
            // by a publish in progress costs one extra reattach at most.
            if (mSHM && !mMappingDead.load() && mIsDynacord && !mRateChangeActive.load() && !FrameLayoutUnchanged()) {
                LogOzzyHAL("layout changed; reattaching");
                DropMapping();
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
                // The engine may have restarted with new rings. Re-read them, but
                // never under running IO: a changed layout there means IO must restart.
                bool layoutOK;
                if (mIOClients.load() > 0)
                    layoutOK = mIsDynacord ? FrameLayoutUnchanged() : true;
                else
                    layoutOK = mIsDynacord ? ApplyFrameLayout() : ApplyPacketLayout();
                if (!layoutOK) {
                    LogOzzyHALError("layout changed or invalid on reconnect");
                    DropMapping();
                    usleep(250000);
                    continue;
                }
                SetIdentity(product.s, manufacturer.s, serial.s);
                LogOzzyHAL("device ready: '%{public}s'", product.s);
            }

            mLastConnectedState = currentlyConnected;
            LogOzzyHAL("connection: %{public}s", currentlyConnected ? "UP" : "DOWN");
            
            // The timeline ends with the device; CoreAudio stops IO as it goes away
            if (!currentlyConnected && mIOClients.load() > 0) {
                for (DeviceState& d : mDevices) d.seed++;
                LogOzzyHAL("I/O lost its device");
            }
            
            if (mHost) {
                if (currentlyConnected) {
                    // Device connected - notify about device properties first, then add to list
                    NotifyAllDevicesChanged(kAudioObjectPropertyName);
                    NotifyAllDevicesChanged(kAudioObjectPropertyManufacturer);
                    NotifyAllDevicesChanged(kAudioDevicePropertyDeviceUID);

                    AudioObjectPropertyAddress listAddr = { kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
                    mHost->PropertiesChanged(mHost, kAudioObjectPlugInObject, 1, &listAddr);
                } else {
                    // Device disconnected - notify that device is no longer alive, then remove from list
                    NotifyAllDevicesChanged(kAudioDevicePropertyDeviceIsAlive);

                    AudioObjectPropertyAddress listAddr = { kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
                    mHost->PropertiesChanged(mHost, kAudioObjectPlugInObject, 1, &listAddr);
                }
            }
        }
        usleep(250000);
    }
}

bool OzzyHAL::IsConnected() { return mLastConnectedState; }

/* Decrements a client count, never below zero; returns the count it saw. */
static uint32_t ReleaseClient(std::atomic<uint32_t>& count) {
    uint32_t n = count.load();
    while (n > 0 && !count.compare_exchange_weak(n, n - 1)) {}
    return n;
}

/* Every device runs on the one engine stream; only the main device writes
 * output, so only its clients touch the output side. */
kern_return_t OzzyHAL::StartIO(AudioObjectID device) {
    const int i = DeviceIndex(device);
    if (i < 0) return kAudioHardwareBadObjectError;
    if (!IsConnected()) return kAudioHardwareNotRunningError;
    // Count first, then look at the mapping: the monitor marks it dead first
    // and then reads the count, so one of the two sides always sees the other.
    mIOClients.fetch_add(1);
    if (mMappingDead.load() || !mSHM || !IsConnected()) {
        mIOClients.fetch_sub(1);
        return kAudioHardwareNotRunningError;
    }
    if (mDevices[i].clients.fetch_add(1) == 0 && i == 0)
        mSHM->audio.halWritePosition.store(0, std::memory_order_relaxed);
    LogOzzyHAL("start I/O on device %{public}u", (unsigned)device);
    mDevices[i].seed++;
    return kIOReturnSuccess;
}

kern_return_t OzzyHAL::StopIO(AudioObjectID device) {
    const int i = DeviceIndex(device);
    if (i < 0) return kAudioHardwareBadObjectError;
    LogOzzyHAL("stop I/O on device %{public}u", (unsigned)device);
    mDevices[i].seed++;
    // Clear while this client is still counted: the monitor only unmaps a
    // dropped mapping once the count reaches zero, so the buffer stays mapped.
    // The engine keeps playing the ring while any other device runs, so the
    // main device's last client silences it.
    if (ReleaseClient(mDevices[i].clients) <= 1 && i == 0)
        ClearOutputBuffer();
    ReleaseClient(mIOClients);
    return kIOReturnSuccess;
}

/* Ask the host to schedule a rate change on the main device; it calls back
 * into PerformDeviceConfigurationChange once IO is quiesced. */
OSStatus OzzyHAL::RequestSampleRate(Float64 rate) {
    if (!mIsDynacord || !mSHM) return kAudioHardwareUnsupportedOperationError;
    if (rate == mCurrentStreamFormat.mSampleRate) return kAudioHardwareNoError;
    if (rate != 44100.0 && rate != 48000.0 && rate != 96000.0) return kAudioDeviceUnsupportedFormatError;
    return mHost->RequestDeviceConfigurationChange(mHost, kOzzyDeviceID, (UInt64)rate, nullptr);
}

/* On the main device: hand the rate to the engine (kext or daemon) and wait for
 * it to restart the stream at that rate in a new session, pick up the new ring
 * and timing layout, then schedule the same change on the mono devices, which
 * share the engine. On a mono device the engine has already switched, so
 * only its own timeline and rate are refreshed. */
OSStatus OzzyHAL::PerformDeviceConfigurationChange(AudioObjectID device, UInt64 rate) {
    if (!mIsDynacord) return kAudioHardwareNoError;
    const int index = DeviceIndex(device);
    if (index < 0) return kAudioHardwareBadObjectError;
    if (index > 0) {
        mDevices[index].anchorSample = mDevices[index].anchorHost = 0;
        mDevices[index].seed++;
        NotifyDeviceChanged(device, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal);
        return kAudioHardwareNoError;
    }
    // Counted as an IO client for the duration, so the monitor defers any
    // unmap until we return; and its session check stands aside meanwhile.
    struct Hold {
        OzzyHAL* h;
        ~Hold() { h->mRateChangeActive.store(false); h->mIOClients.fetch_sub(1); }
    };
    mIOClients.fetch_add(1);
    mRateChangeActive.store(true);
    Hold hold { this };
    if (mMappingDead.load() || !mSHM) return kAudioHardwareNotRunningError;
    uint32_t oldSession = mSHM->sessionID;
    mSHM->audio.requestedRate = (uint32_t)rate;
    for (int i = 0; i < 300; i++) {
        usleep(10000);
        if (mMappingDead.load() || !mSHM) return kAudioHardwareNotRunningError;
        if (mSHM->sessionID != oldSession && mSHM->audio.sampleRate.load() == rate &&
            mSHM->audio.timestamp.hostTime.load() != 0) {
            if (!ApplyFrameLayout()) {
                LogOzzyHALError("unusable layout after switching to %{public}llu Hz", rate);
                return kAudioHardwareUnspecifiedError;
            }
            mLastSessionID = mSHM->sessionID;
            // Every timeline restarts. A running mono device's IO thread owns its
            // anchor: it sees the sample time go back and moves on by itself.
            mDevices[0].anchorSample = mDevices[0].anchorHost = 0;
            for (DeviceState& d : mDevices) d.seed++;
            LogOzzyHAL("rate changed to %{public}llu Hz", rate);
            NotifyDeviceChanged(kOzzyDeviceID, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal);
            for (UInt32 i = 1; i < OzzyDeviceCount(true, mIsDynacord); i++)
                mHost->RequestDeviceConfigurationChange(mHost, kOzzyDevices[i].device, rate, nullptr);
            return kAudioHardwareNoError;
        }
    }
    LogOzzyHALError("engine did not switch to %{public}llu Hz within 3 s", rate);
    return kAudioHardwareUnspecifiedError;
}

HRESULT OzzyHAL::GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed) {
    const int index = DeviceIndex(inDeviceObjectID);
    if (index < 0) return kAudioHardwareBadObjectError;
    if (!mSHM || mMappingDead.load()) return kAudioHardwareNotRunningError;
    DeviceState& dev = mDevices[index];

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
        sTime = dev.anchorSample;
        hTime = dev.anchorHost;
    }

    // No anchor ever published: never hand CoreAudio a (0, 0) anchor.
    if (hTime == 0 && dev.anchorHost == 0) return kAudioHardwareNotRunningError;
    if (hTime == 0 && dev.anchorHost != 0) {
        // The engine is restarting and has no anchor yet: keep serving the last one.
        sTime = dev.anchorSample;
        hTime = dev.anchorHost;
    } else if (hTime != 0) {
        if (sTime < dev.anchorSample) dev.seed++;   // a new timeline started
        dev.anchorSample = sTime;
        dev.anchorHost = hTime;
    }
    if (outSampleTime) *outSampleTime = (Float64)sTime;
    if (outHostTime) *outHostTime = hTime;
    if (outSeed) *outSeed = dev.seed.load();
    
    return kAudioHardwareNoError;
}

OSStatus OzzyHAL::ioOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer) {
    if (!mSHM || mMappingDead.load() || !mInputBufferAddr || !mOutputBufferAddr) return kAudioHardwareNoError;

    if (inOperationID == kAudioServerPlugInIOOperationWriteMix) {
        if (mWriteOutput && inStreamObjectID == kOzzyOutputStreamID) {
            uint64_t sampleTime = (uint64_t)inIOCycleInfo->mOutputTime.mSampleTime;
            const float gain = mMuted.load() ? 0.0f : OzzyVolumeScalarToGain(mVolumeScalar.load());
            OzzyApplyGainRamp((float*)ioMainBuffer, inIOBufferFrameSize, mChannels, mAppliedGain, gain);
            mAppliedGain = gain;
            mWriteOutput(mOutputBufferAddr, (const float*)ioMainBuffer, sampleTime, inIOBufferFrameSize, mRingSize, mOutputBytesPerFrame);
            mSHM->audio.halWritePosition.store(sampleTime + inIOBufferFrameSize, std::memory_order_release);
        }
    }
    else if (inOperationID == kAudioServerPlugInIOOperationReadInput) {
        const int index = DeviceIndex(inStreamObjectID);
        uint64_t sampleTime = (uint64_t)inIOCycleInfo->mInputTime.mSampleTime;
        if (index == 0 && mReadInput) {
            mReadInput((float*)ioMainBuffer, mInputBufferAddr, sampleTime, inIOBufferFrameSize, mRingSizeIn, mInputBytesPerFrame);
        } else if (index > 0) {
            // A rate change restarts the engine under a running mono device
            // before the host gets to stop it: hand it silence meanwhile.
            if (mRateChangeActive.load())
                memset(ioMainBuffer, 0, (size_t)inIOBufferFrameSize * sizeof(float));
            else
                DynacordReadInputChannel((float*)ioMainBuffer, mInputBufferAddr, sampleTime, inIOBufferFrameSize, mRingSizeIn, mInputBytesPerFrame, kOzzyDevices[index].inputChannel);
        }
    }
    return kAudioHardwareNoError;
}

CFStringRef OzzyHAL::GetDeviceName(int device) const { return mDevices[device].name ? mDevices[device].name : mModelUID; }
CFStringRef OzzyHAL::GetManufacturer() const { return mManufacturerUID; }
CFStringRef OzzyHAL::GetDeviceUID(int device) const { return mDevices[device].uid; }
CFStringRef OzzyHAL::GetModelUID(int device) const { return mDevices[device].model; }
UInt32 OzzyHAL::GetTransportType() const { return kAudioDeviceTransportTypeUSB; }
UInt32 OzzyHAL::GetClockDomain() const { return 0x504C4F59; }
Float64 OzzyHAL::GetNominalSampleRate() const { return mCurrentStreamFormat.mSampleRate; }
AudioBufferList* OzzyHAL::GetInputStreamConfiguration(int device) const {
    return const_cast<AudioBufferList*>(kOzzyDevices[device].inputChannel ? &mMonoInputConfig : &mInputConfig);
}
AudioBufferList* OzzyHAL::GetOutputStreamConfiguration(int device) const {
    return const_cast<AudioBufferList*>(kOzzyDevices[device].outputStream ? &mOutputConfig : &mNoConfig);
}
AudioStreamBasicDescription OzzyHAL::GetStreamFormat(int device) const {
    AudioStreamBasicDescription f = mCurrentStreamFormat;
    f.mChannelsPerFrame = GetChannelCount(device);
    f.mBytesPerFrame = f.mBytesPerPacket = 4 * f.mChannelsPerFrame;
    return f;
}
UInt32 OzzyHAL::GetZeroTimestampPeriod() const { return mZeroTimestampPeriod; }

/* The CMS 600-3 labels its first pair of each scope as speakers and the rest
 * by position, and its mono devices as mono; the Ploytec devices leave every
 * channel unlabelled. */
void OzzyHAL::FillPreferredChannelLayout(int device, AudioObjectPropertyScope inScope, AudioChannelLayout* layout) const {
    const UInt32 channels = GetChannelCount(device);
    layout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
    layout->mChannelBitmap = 0;
    layout->mNumberChannelDescriptions = channels;
    if (kOzzyDevices[device].inputChannel) {
        layout->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Mono;
        layout->mChannelDescriptions[0].mChannelFlags = kAudioChannelFlags_AllOff;
        return;
    }
    for (UInt32 i = 0; i < channels; ++i) {
        AudioChannelLabel label = kAudioChannelLabel_Unknown;
        const OzzyChannelInfo* info = mIsDynacord ? OzzyDynacordChannel(IsOutputScope(inScope), i + 1) : nullptr;
        if (info) {
            switch (info->role) {
                case OzzyChannelRole::Left: label = kAudioChannelLabel_Left; break;
                case OzzyChannelRole::Right: label = kAudioChannelLabel_Right; break;
                case OzzyChannelRole::Discrete: label = kAudioChannelLabel_Discrete_0 + i; break;
            }
        }
        layout->mChannelDescriptions[i].mChannelLabel = label;
        layout->mChannelDescriptions[i].mChannelFlags = kAudioChannelFlags_AllOff;
    }
}

UInt32 OzzyHAL::GetPreferredChannelLayoutSize(int device) const {
    return offsetof(AudioChannelLayout, mChannelDescriptions) + (GetChannelCount(device) * sizeof(AudioChannelDescription));
}

/* A +1 name for a CMS 600-3 channel, or nullptr where the channel has none.
 * A mono device's only channel is input element 1, named after its source. */
CFStringRef OzzyHAL::CopyChannelName(int device, AudioObjectPropertyScope scope, AudioObjectPropertyElement element) const {
    if (!mIsDynacord || element < 1 || element > GetChannelCount(device)) return nullptr;
    if (scope != kAudioObjectPropertyScopeInput && scope != kAudioObjectPropertyScopeOutput) return nullptr;
    const uint32_t source = kOzzyDevices[device].inputChannel;
    if (source && scope != kAudioObjectPropertyScopeInput) return nullptr;
    const OzzyChannelInfo* info = OzzyDynacordChannel(scope == kAudioObjectPropertyScopeOutput, source ? source : element);
    return info ? CFStringCreateWithCString(NULL, info->name, kCFStringEncodingUTF8) : nullptr;
}

/* A mono device offers its one channel as both sides of the pair. */
OzzyStereoPair OzzyHAL::GetStereoPair(int device, AudioObjectPropertyScope scope) const {
    if (kOzzyDevices[device].inputChannel) return { 1, 1 };
    const std::atomic<uint32_t>& stored = IsOutputScope(scope) ? mStereoPairOut : mStereoPairIn;
    return OzzyStereoPairFor(OzzyStereoPairUnpack(stored.load()), mChannels);
}

OSStatus OzzyHAL::SetStereoPair(AudioObjectPropertyScope scope, OzzyStereoPair pair) {
    if (!OzzyStereoPairValid(pair, mChannels)) return kAudioHardwareIllegalOperationError;
    const bool output = IsOutputScope(scope);
    std::atomic<uint32_t>& stored = output ? mStereoPairOut : mStereoPairIn;
    if (stored.exchange(OzzyStereoPairPack(pair)) == OzzyStereoPairPack(pair)) return kAudioHardwareNoError;
    LogOzzyHAL("%{public}s stereo pair %{public}u,%{public}u", output ? "output" : "input", pair.left, pair.right);
    NotifyDeviceChanged(kOzzyDeviceID, kAudioDevicePropertyPreferredChannelsForStereo, output ? kAudioObjectPropertyScopeOutput : kAudioObjectPropertyScopeInput);
    if (mHost) WriteStereoPairToStorage(mHost, output ? kOutputStereoPairStorageKey : kInputStereoPairStorageKey, pair);
    return kAudioHardwareNoError;
}

extern "C" HRESULT OzzyInitialize(AudioServerPlugInDriverRef, AudioServerPlugInHostRef inHost) {
    OzzyHAL::Get().Init(inHost);
    return kAudioHardwareNoError;
}

extern "C" HRESULT OzzyCreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo*, AudioObjectID*) { return kAudioHardwareNoError; }
extern "C" HRESULT OzzyDestroyDevice(AudioServerPlugInDriverRef, AudioObjectID) { return kAudioHardwareNoError; }
extern "C" HRESULT OzzyAddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*) { return kAudioHardwareNoError; }
extern "C" HRESULT OzzyRemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*) { return kAudioHardwareNoError; }
extern "C" HRESULT OzzyPerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void*) {
    return OzzyHAL::Get().PerformDeviceConfigurationChange(inDeviceObjectID, inChangeAction);
}

/* The device an object belongs to, or -1: see OzzyHAL::DeviceIndex. */
static int DeviceOf(AudioObjectID inObjectID) { return OzzyHAL::Get().DeviceIndex(inObjectID); }
static bool IsDevice(AudioObjectID inObjectID) { return DeviceOf(inObjectID) >= 0 && !OzzyIsStream(inObjectID); }
static bool IsStream(AudioObjectID inObjectID) { return DeviceOf(inObjectID) >= 0 && OzzyIsStream(inObjectID); }
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
    if (IsDevice(inObjectID)) {
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
            case kAudioDevicePropertyZeroTimeStampPeriod: case kAudioObjectPropertyOwnedObjects:
            case kAudioObjectPropertyControlList: case kAudioDevicePropertyPreferredChannelsForStereo:
            case kAudioDevicePropertyClockDomain: case kAudioDevicePropertyRelatedDevices:
            case kAudioDevicePropertyDeviceCanBeDefaultDevice: case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: return true;
            case kAudioObjectPropertyElementName: {
                CFStringRef name = OzzyHAL::Get().CopyChannelName(DeviceOf(inObjectID), inAddress->mScope, inAddress->mElement);
                if (name) CFRelease(name);
                return name != nullptr;
            }
        }
        return false;
    }
    if (IsControl(inObjectID)) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner: case kAudioObjectPropertyOwnedObjects:
            case kAudioControlPropertyScope: case kAudioControlPropertyElement: return true;
        }
        if (inObjectID == kOzzyVolumeControlID) {
            switch (inAddress->mSelector) {
                case kAudioLevelControlPropertyScalarValue: case kAudioLevelControlPropertyDecibelValue:
                case kAudioLevelControlPropertyDecibelRange: case kAudioLevelControlPropertyConvertScalarToDecibels:
                case kAudioLevelControlPropertyConvertDecibelsToScalar: return true;
            }
            return false;
        }
        return inAddress->mSelector == kAudioBooleanControlPropertyValue;
    }
    if (IsStream(inObjectID)) {
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

extern "C" HRESULT OzzyIsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID inObjectID, pid_t, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable) {
    if (inObjectID == kOzzyDeviceID)
        *outIsSettable = (inAddress->mSelector == kAudioDevicePropertyNominalSampleRate && OzzyHAL::Get().IsRateSettable()) ||
                         inAddress->mSelector == kAudioDevicePropertyPreferredChannelsForStereo;
    else if (inObjectID == kOzzyVolumeControlID)
        *outIsSettable = inAddress->mSelector == kAudioLevelControlPropertyScalarValue || inAddress->mSelector == kAudioLevelControlPropertyDecibelValue;
    else if (inObjectID == kOzzyMuteControlID)
        *outIsSettable = inAddress->mSelector == kAudioBooleanControlPropertyValue;
    else
        *outIsSettable = false;
    return kAudioHardwareNoError;
}

extern "C" HRESULT OzzyGetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID inObjectID, pid_t, const AudioObjectPropertyAddress* inAddress, UInt32, const void*, UInt32* outDataSize) {
    if (!outDataSize) return kAudioHardwareIllegalOperationError;
    *outDataSize = 0;

    if (inObjectID == kAudioObjectPlugInObject) {
        switch (inAddress->mSelector) {
            case kAudioPlugInPropertyDeviceList: case kAudioObjectPropertyOwnedObjects:
                *outDataSize = OzzyHAL::Get().PublishedDeviceCount() * sizeof(AudioObjectID); return kAudioHardwareNoError;
            case kAudioObjectPropertyControlList: *outDataSize = 0; return kAudioHardwareNoError;
            case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer: *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
            case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
            default: return kAudioHardwareUnknownPropertyError;
        }
    }
    if (IsDevice(inObjectID)) {
        const int dev = DeviceOf(inObjectID);
        const bool main = inObjectID == kOzzyDeviceID;
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer:
            case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID:
                *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
            case kAudioObjectPropertyControlList:
                *outDataSize = main && IsOutputScope(inAddress->mScope) ? sizeof(kOzzyControls) : 0; return kAudioHardwareNoError;
            case kAudioObjectPropertyOwnedObjects: *outDataSize = main ? sizeof(kOzzyDeviceOwned) : sizeof(AudioObjectID); return kAudioHardwareNoError;
            case kAudioDevicePropertyRelatedDevices: *outDataSize = OzzyHAL::Get().PublishedDeviceCount() * sizeof(AudioObjectID); return kAudioHardwareNoError;
            case kAudioDevicePropertyClockDomain: case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
            case kAudioObjectPropertyOwner: *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
            case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
            case kAudioDevicePropertyTransportType: case kAudioDevicePropertyIsHidden:
            case kAudioDevicePropertyDeviceIsAlive: case kAudioDevicePropertyDeviceIsRunning:
            case kAudioDevicePropertyDeviceIsRunningSomewhere: case kAudioDevicePropertyNominalSampleRate:
                *outDataSize = sizeof(Float64); return kAudioHardwareNoError;
            case kAudioDevicePropertyAvailableNominalSampleRates: *outDataSize = OzzyHAL::Get().GetAvailableSampleRateCount() * sizeof(AudioValueRange); return kAudioHardwareNoError;
            case kAudioDevicePropertyStreams:
                if (inAddress->mScope == kAudioObjectPropertyScopeInput) *outDataSize = sizeof(AudioObjectID);
                else if (inAddress->mScope == kAudioObjectPropertyScopeOutput) *outDataSize = main ? sizeof(AudioObjectID) : 0;
                else if (inAddress->mScope == kAudioObjectPropertyScopeGlobal) *outDataSize = (main ? 2 : 1) * sizeof(AudioObjectID);
                else *outDataSize = 0;
                return kAudioHardwareNoError;
            case kAudioDevicePropertyStreamConfiguration:
                *outDataSize = GetAudioBufferListSize((IsOutputScope(inAddress->mScope) ? OzzyHAL::Get().GetOutputStreamConfiguration(dev) : OzzyHAL::Get().GetInputStreamConfiguration(dev))->mNumberBuffers);
                return kAudioHardwareNoError;
            case kAudioDevicePropertySafetyOffset: case kAudioDevicePropertyLatency: case kAudioDevicePropertyZeroTimeStampPeriod: *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
            case kAudioDevicePropertyPreferredChannelLayout: *outDataSize = OzzyHAL::Get().GetPreferredChannelLayoutSize(dev); return kAudioHardwareNoError;
            case kAudioDevicePropertyPreferredChannelsForStereo: *outDataSize = 2 * sizeof(UInt32); return kAudioHardwareNoError;
            case kAudioObjectPropertyElementName: {
                CFStringRef name = OzzyHAL::Get().CopyChannelName(dev, inAddress->mScope, inAddress->mElement);
                if (!name) return kAudioHardwareUnknownPropertyError;
                CFRelease(name);
                *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
            }
            default: return kAudioHardwareUnknownPropertyError;
        }
    }
    if (IsStream(inObjectID)) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
            case kAudioObjectPropertyName: *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
            case kAudioObjectPropertyOwner: *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
            case kAudioStreamPropertyDirection: case kAudioStreamPropertyTerminalType: case kAudioStreamPropertyStartingChannel: case kAudioStreamPropertyLatency:
                *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
            case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat: *outDataSize = sizeof(AudioStreamBasicDescription); return kAudioHardwareNoError;
            case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats: *outDataSize = OzzyHAL::Get().GetAvailableSampleRateCount() * sizeof(AudioStreamRangedDescription); return kAudioHardwareNoError;
            default: return kAudioHardwareUnknownPropertyError;
        }
    }
    if (IsControl(inObjectID)) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
            case kAudioObjectPropertyOwner: *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
            case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return kAudioHardwareNoError;
            case kAudioControlPropertyScope: *outDataSize = sizeof(AudioObjectPropertyScope); return kAudioHardwareNoError;
            case kAudioControlPropertyElement: *outDataSize = sizeof(AudioObjectPropertyElement); return kAudioHardwareNoError;
            case kAudioLevelControlPropertyScalarValue: case kAudioLevelControlPropertyDecibelValue:
            case kAudioLevelControlPropertyConvertScalarToDecibels: case kAudioLevelControlPropertyConvertDecibelsToScalar:
                if (inObjectID != kOzzyVolumeControlID) return kAudioHardwareUnknownPropertyError;
                *outDataSize = sizeof(Float32); return kAudioHardwareNoError;
            case kAudioLevelControlPropertyDecibelRange:
                if (inObjectID != kOzzyVolumeControlID) return kAudioHardwareUnknownPropertyError;
                *outDataSize = sizeof(AudioValueRange); return kAudioHardwareNoError;
            case kAudioBooleanControlPropertyValue:
                if (inObjectID != kOzzyMuteControlID) return kAudioHardwareUnknownPropertyError;
                *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
            default: return kAudioHardwareUnknownPropertyError;
        }
    }
    return kAudioHardwareBadObjectError;
}

/* The devices the attached hardware publishes, main device first. */
static OSStatus WriteDeviceList(UInt32 inMax, UInt32* outSize, void* outData) {
    AudioObjectID ids[kOzzyMaxDevices];
    const UInt32 n = OzzyHAL::Get().PublishedDeviceCount();
    for (UInt32 i = 0; i < n; i++) ids[i] = kOzzyDevices[i].device;
    return WriteObjectIDs(inMax, outSize, outData, ids, n);
}

extern "C" HRESULT OzzyGetPropertyData(AudioServerPlugInDriverRef, AudioObjectID inObjectID, pid_t, const AudioObjectPropertyAddress* inAddress, UInt32, const void*, UInt32 inMax, UInt32* outSize, void* outData) {
    if (inObjectID == kAudioObjectPlugInObject) {
        switch (inAddress->mSelector) {
            case kAudioPlugInPropertyDeviceList: case kAudioObjectPropertyOwnedObjects:
                return WriteDeviceList(inMax, outSize, outData);
            case kAudioObjectPropertyControlList: if (outSize) *outSize = 0; return kAudioHardwareNoError;
            case kAudioObjectPropertyName: return WriteCFString(inMax, outSize, outData, CFSTR("Ozzy HAL"));
            case kAudioObjectPropertyManufacturer: return WriteCFString(inMax, outSize, outData, CFSTR("Ozzy"));
            case kAudioObjectPropertyBaseClass: return WriteClassID(inMax, outSize, outData, kAudioObjectClassID);
            case kAudioObjectPropertyClass: return WriteClassID(inMax, outSize, outData, kAudioPlugInClassID);
            default: return kAudioHardwareUnknownPropertyError;
        }
    }
    if (IsDevice(inObjectID)) {
        const int dev = DeviceOf(inObjectID);
        const bool main = inObjectID == kOzzyDeviceID;
        const OzzyDeviceInfo& info = kOzzyDevices[dev];
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass: return WriteClassID(inMax, outSize, outData, kAudioObjectClassID);
            case kAudioObjectPropertyClass: return WriteClassID(inMax, outSize, outData, kAudioDeviceClassID);
            case kAudioObjectPropertyControlList:
                if (!main || !IsOutputScope(inAddress->mScope)) { if (outSize) *outSize = 0; return kAudioHardwareNoError; }
                return WriteObjectIDs(inMax, outSize, outData, kOzzyControls, 2);
            case kAudioObjectPropertyOwner: return WriteObjectID(inMax, outSize, outData, kAudioObjectPlugInObject);
            case kAudioObjectPropertyName: return WriteCFString(inMax, outSize, outData, OzzyHAL::Get().GetDeviceName(dev));
            case kAudioObjectPropertyManufacturer: return WriteCFString(inMax, outSize, outData, OzzyHAL::Get().GetManufacturer());
            case kAudioDevicePropertyDeviceUID: return WriteCFString(inMax, outSize, outData, OzzyHAL::Get().GetDeviceUID(dev));
            case kAudioDevicePropertyModelUID: return WriteCFString(inMax, outSize, outData, OzzyHAL::Get().GetModelUID(dev));
            case kAudioDevicePropertyTransportType: return WriteUInt32(inMax, outSize, outData, OzzyHAL::Get().GetTransportType());
            case kAudioDevicePropertyIsHidden: return WriteUInt32(inMax, outSize, outData, 0);
            case kAudioDevicePropertyDeviceIsAlive: return WriteUInt32(inMax, outSize, outData, OzzyHAL::Get().IsConnected() ? 1 : 0);
            case kAudioDevicePropertyDeviceIsRunning: case kAudioDevicePropertyDeviceIsRunningSomewhere: return WriteUInt32(inMax, outSize, outData, OzzyHAL::Get().IsRunning(dev) ? 1 : 0);
            // A mono device is only ever an input, and never the alert device
            case kAudioDevicePropertyDeviceCanBeDefaultDevice:
                return WriteUInt32(inMax, outSize, outData, main || inAddress->mScope == kAudioObjectPropertyScopeInput ? 1 : 0);
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: return WriteUInt32(inMax, outSize, outData, main ? 1 : 0);
            case kAudioDevicePropertyRelatedDevices: return WriteDeviceList(inMax, outSize, outData);
            case kAudioDevicePropertyNominalSampleRate: return WriteFloat64(inMax, outSize, outData, OzzyHAL::Get().GetNominalSampleRate());
            case kAudioDevicePropertyAvailableNominalSampleRates: {
                UInt32 n = std::min<UInt32>(inMax / sizeof(AudioValueRange), OzzyHAL::Get().GetAvailableSampleRateCount());
                if (outData) memcpy(outData, OzzyHAL::Get().GetAvailableSampleRateList(), n * sizeof(AudioValueRange));
                if (outSize) *outSize = n * sizeof(AudioValueRange);
                return kAudioHardwareNoError;
            }
            case kAudioDevicePropertyStreams: {
                const AudioObjectID streamList[] = { info.inputStream, info.outputStream };
                if (inAddress->mScope == kAudioObjectPropertyScopeInput) return WriteObjectID(inMax, outSize, outData, info.inputStream);
                else if (inAddress->mScope == kAudioObjectPropertyScopeOutput) return WriteObjectIDs(inMax, outSize, outData, &streamList[1], main ? 1 : 0);
                else return WriteObjectIDs(inMax, outSize, outData, streamList, main ? 2 : 1);
            }
            case kAudioObjectPropertyOwnedObjects:
                return main ? WriteObjectIDs(inMax, outSize, outData, kOzzyDeviceOwned, 4) : WriteObjectID(inMax, outSize, outData, info.inputStream);
            case kAudioDevicePropertyStreamConfiguration: {
                AudioBufferList* config = (inAddress->mScope == kAudioObjectPropertyScopeInput) ? OzzyHAL::Get().GetInputStreamConfiguration(dev) : OzzyHAL::Get().GetOutputStreamConfiguration(dev);
                UInt32 actualSize = GetAudioBufferListSize(config->mNumberBuffers); if (outSize) *outSize = actualSize;
                if (outData) { if (inMax < actualSize) return kAudioHardwareBadPropertySizeError; memcpy(outData, config, actualSize); } return kAudioHardwareNoError;
            }
            case kAudioDevicePropertyClockDomain: return WriteUInt32(inMax, outSize, outData, OzzyHAL::Get().GetClockDomain());
            case kAudioDevicePropertyLatency: return WriteUInt32(inMax, outSize, outData, OzzyHAL::Get().GetLatency());
            case kAudioDevicePropertySafetyOffset: return WriteUInt32(inMax, outSize, outData, OzzyHAL::Get().GetSafetyOffset(inAddress->mScope));
            case kAudioDevicePropertyZeroTimeStampPeriod: return WriteUInt32(inMax, outSize, outData, OzzyHAL::Get().GetZeroTimestampPeriod());
            case kAudioDevicePropertyPreferredChannelLayout: {
                UInt32 size = OzzyHAL::Get().GetPreferredChannelLayoutSize(dev);
                if (outSize) *outSize = size;
                if (outData) { if (inMax < size) return kAudioHardwareBadPropertySizeError; OzzyHAL::Get().FillPreferredChannelLayout(dev, inAddress->mScope, (AudioChannelLayout*)outData); }
                return kAudioHardwareNoError;
            }
            case kAudioDevicePropertyPreferredChannelsForStereo: {
                if (outData && inMax < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                const OzzyStereoPair pair = OzzyHAL::Get().GetStereoPair(dev, inAddress->mScope);
                if (outData) { ((UInt32*)outData)[0] = pair.left; ((UInt32*)outData)[1] = pair.right; }
                if (outSize) *outSize = 2 * sizeof(UInt32);
                return kAudioHardwareNoError;
            }
            case kAudioObjectPropertyElementName: {
                if (outData && inMax < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                CFStringRef name = OzzyHAL::Get().CopyChannelName(dev, inAddress->mScope, inAddress->mElement);
                if (!name) return kAudioHardwareUnknownPropertyError;
                if (outData) *(CFStringRef*)outData = name; else CFRelease(name);
                if (outSize) *outSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            }
            default: return kAudioHardwareUnknownPropertyError;
        }
    }
    if (IsStream(inObjectID)) {
        const int dev = DeviceOf(inObjectID);
        const bool mono = kOzzyDevices[dev].inputChannel != 0;
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass: return WriteClassID(inMax, outSize, outData, kAudioObjectClassID);
            case kAudioObjectPropertyClass: return WriteClassID(inMax, outSize, outData, kAudioStreamClassID);
            case kAudioObjectPropertyName: return WriteCFString(inMax, outSize, outData, (inObjectID == kOzzyOutputStreamID ? CFSTR("Ozzy PCM Output Stream") : CFSTR("Ozzy PCM Input Stream")));
            case kAudioObjectPropertyOwner: return WriteObjectID(inMax, outSize, outData, kOzzyDevices[dev].device);
            case kAudioStreamPropertyDirection: return WriteUInt32(inMax, outSize, outData, (inObjectID == kOzzyOutputStreamID ? 0 : 1));
            case kAudioStreamPropertyTerminalType: return WriteUInt32(inMax, outSize, outData, mono ? kAudioStreamTerminalTypeMicrophone : kAudioStreamTerminalTypeSpeaker);
            case kAudioStreamPropertyStartingChannel: return WriteUInt32(inMax, outSize, outData, 1);
            case kAudioStreamPropertyLatency: return WriteUInt32(inMax, outSize, outData, OzzyHAL::Get().GetLatency());
            case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat:
                if (outData && inMax < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
                if (outData) *(AudioStreamBasicDescription*)outData = OzzyHAL::Get().GetStreamFormat(dev);
                if (outSize) *outSize = sizeof(AudioStreamBasicDescription); return kAudioHardwareNoError;
            case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats: {
                UInt32 n = std::min<UInt32>(inMax / sizeof(AudioStreamRangedDescription), OzzyHAL::Get().GetAvailableSampleRateCount());
                const AudioValueRange* rates = OzzyHAL::Get().GetAvailableSampleRateList();
                AudioStreamRangedDescription* list = (AudioStreamRangedDescription*)outData;
                for (UInt32 i = 0; list && i < n; i++) {
                    list[i].mFormat = OzzyHAL::Get().GetStreamFormat(dev);
                    list[i].mFormat.mSampleRate = rates[i].mMinimum;
                    list[i].mSampleRateRange = rates[i];
                }
                if (outSize) *outSize = n * sizeof(AudioStreamRangedDescription);
                return kAudioHardwareNoError;
            }
            default: return kAudioHardwareUnknownPropertyError;
        }
    }
    if (IsControl(inObjectID)) {
        const bool isVolume = inObjectID == kOzzyVolumeControlID;
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass: return WriteClassID(inMax, outSize, outData, isVolume ? kAudioLevelControlClassID : kAudioBooleanControlClassID);
            case kAudioObjectPropertyClass: return WriteClassID(inMax, outSize, outData, isVolume ? kAudioVolumeControlClassID : kAudioMuteControlClassID);
            case kAudioObjectPropertyOwner: return WriteObjectID(inMax, outSize, outData, kOzzyDeviceID);
            case kAudioObjectPropertyOwnedObjects: if (outSize) *outSize = 0; return kAudioHardwareNoError;
            case kAudioControlPropertyScope: return WriteUInt32(inMax, outSize, outData, kAudioObjectPropertyScopeOutput);
            case kAudioControlPropertyElement: return WriteUInt32(inMax, outSize, outData, kAudioObjectPropertyElementMain);
            default: break;
        }
        if (isVolume) {
            switch (inAddress->mSelector) {
                case kAudioLevelControlPropertyScalarValue: return WriteFloat32(inMax, outSize, outData, OzzyHAL::Get().GetVolumeScalar());
                case kAudioLevelControlPropertyDecibelValue: return WriteFloat32(inMax, outSize, outData, OzzyVolumeScalarToDB(OzzyHAL::Get().GetVolumeScalar()));
                case kAudioLevelControlPropertyDecibelRange: {
                    if (outData && inMax < sizeof(AudioValueRange)) return kAudioHardwareBadPropertySizeError;
                    if (outData) *(AudioValueRange*)outData = { kOzzyVolumeMinDB, kOzzyVolumeMaxDB };
                    if (outSize) *outSize = sizeof(AudioValueRange);
                    return kAudioHardwareNoError;
                }
                // The value to convert arrives in outData and is converted in place.
                case kAudioLevelControlPropertyConvertScalarToDecibels:
                    if (!outData || inMax < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
                    return WriteFloat32(inMax, outSize, outData, OzzyVolumeScalarToDB(*(Float32*)outData));
                case kAudioLevelControlPropertyConvertDecibelsToScalar:
                    if (!outData || inMax < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
                    return WriteFloat32(inMax, outSize, outData, OzzyVolumeDBToScalar(*(Float32*)outData));
                default: return kAudioHardwareUnknownPropertyError;
            }
        }
        if (inAddress->mSelector == kAudioBooleanControlPropertyValue) return WriteUInt32(inMax, outSize, outData, OzzyHAL::Get().IsMuted() ? 1 : 0);
        return kAudioHardwareUnknownPropertyError;
    }
    return kAudioHardwareBadObjectError;
}

extern "C" HRESULT OzzySetPropertyData(AudioServerPlugInDriverRef, AudioObjectID inObjectID, pid_t, const AudioObjectPropertyAddress* inAddress, UInt32, const void*, UInt32 inDataSize, const void* inData) {
    if (inObjectID == kOzzyDeviceID && inAddress->mSelector == kAudioDevicePropertyNominalSampleRate) {
        if (inDataSize < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        return OzzyHAL::Get().RequestSampleRate(*(const Float64*)inData);
    }
    if (inObjectID == kOzzyDeviceID && inAddress->mSelector == kAudioDevicePropertyPreferredChannelsForStereo) {
        if (inDataSize < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
        const UInt32* ch = (const UInt32*)inData;
        return OzzyHAL::Get().SetStereoPair(inAddress->mScope, { ch[0], ch[1] });
    }
    if (inObjectID == kOzzyVolumeControlID) {
        if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
        Float32 v = *(const Float32*)inData;
        if (inAddress->mSelector == kAudioLevelControlPropertyScalarValue) { OzzyHAL::Get().SetVolumeScalar(v); return kAudioHardwareNoError; }
        if (inAddress->mSelector == kAudioLevelControlPropertyDecibelValue) { OzzyHAL::Get().SetVolumeScalar(OzzyVolumeDBToScalar(v)); return kAudioHardwareNoError; }
        return kAudioHardwareUnknownPropertyError;
    }
    if (inObjectID == kOzzyMuteControlID && inAddress->mSelector == kAudioBooleanControlPropertyValue) {
        if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
        OzzyHAL::Get().SetMuted(*(const UInt32*)inData != 0);
        return kAudioHardwareNoError;
    }
    return kAudioHardwareUnsupportedOperationError;
}
extern "C" HRESULT OzzyStartIO(AudioServerPlugInDriverRef, AudioObjectID inDeviceObjectID, UInt32) { return OzzyHAL::Get().StartIO(inDeviceObjectID); }
extern "C" HRESULT OzzyStopIO(AudioServerPlugInDriverRef, AudioObjectID inDeviceObjectID, UInt32) { return OzzyHAL::Get().StopIO(inDeviceObjectID); }
extern "C" HRESULT OzzyGetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed) { return OzzyHAL::Get().GetZeroTimeStamp(inDriver, inDeviceObjectID, inClientID, outSampleTime, outHostTime, outSeed); }
extern "C" OSStatus OzzyWillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID inDeviceObjectID, UInt32, UInt32 inOperationID, Boolean* outWillDo, Boolean* outWillDoInPlace) { bool willDo = false; bool willDoInPlace = true; switch (inOperationID) { case kAudioServerPlugInIOOperationWriteMix: willDo = inDeviceObjectID == kOzzyDeviceID; break; case kAudioServerPlugInIOOperationReadInput: willDo = true; break; default: willDo = false; break; } if (outWillDo) *outWillDo = willDo; if (outWillDoInPlace) *outWillDoInPlace = willDoInPlace; return kAudioHardwareNoError; }
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