#ifndef OzzyHAL_h
#define OzzyHAL_h

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreAudioTypes/CoreAudioBaseTypes.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h> // Required for Kext Communication
#include <os/log.h>
#include <atomic>
#include <cstdint>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include "../Shared/OzzySharedData.h"
#include "OzzyChannels.h"
#include "OzzyDevices.h"

// Codec function pointer types for device-agnostic encoding/decoding
typedef void (*PCMEncoderFunc)(uint8_t* dst, const float* src);
typedef void (*PCMDecoderFunc)(float* dst, const uint8_t* src);
typedef void (*WriteOutputFunc)(uint8_t* ringBuffer, const float* srcFrames, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame);
typedef void (*ReadInputFunc)(float* dstFrames, const uint8_t* ringBuffer, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame);
typedef void (*ClearOutputFunc)(uint8_t* outputBuffer, uint32_t bufferSize);

class OzzyHAL {
public:
    static OzzyHAL& Get();
    
    void Init(AudioServerPlugInHostRef host);
    void Cleanup();

    bool IsConnected();
    bool IsRunning(int device) const { return mDevices[device].clients.load() > 0; }

    /* Index into kOzzyDevices of a device this plug-in answers for, or of one of
     * its streams; -1 for anything else. */
    int DeviceIndex(AudioObjectID object) const { return OzzyDeviceIndex(object, true, mIsDynacord); }
    UInt32 PublishedDeviceCount() { return OzzyDeviceCount(IsConnected(), mIsDynacord); }

    kern_return_t StartIO(AudioObjectID device);
    kern_return_t StopIO(AudioObjectID device);

    HRESULT GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed);
    
    Float64 GetNominalSampleRate() const;
    bool IsRateSettable() const { return mIsDynacord; }
    UInt32 GetAvailableSampleRateCount() const { return mNumSampleRates; }
    const AudioValueRange* GetAvailableSampleRateList() const { return mSampleRateList; }
    OSStatus RequestSampleRate(Float64 rate);
    OSStatus PerformDeviceConfigurationChange(AudioObjectID device, UInt64 rate);
    AudioBufferList* GetInputStreamConfiguration(int device) const;
    AudioBufferList* GetOutputStreamConfiguration(int device) const;
    CFStringRef GetDeviceName(int device) const;
    CFStringRef GetManufacturer() const;
    CFStringRef GetDeviceUID(int device) const;
    CFStringRef GetModelUID(int device) const;
    UInt32 GetTransportType() const;
    UInt32 GetClockDomain() const;
    UInt32 GetChannelCount(int device) const { return kOzzyDevices[device].inputChannel ? 1 : mChannels; }
    AudioStreamBasicDescription GetStreamFormat(int device) const;
    
    UInt32 GetLatency() const { return 0; }
    UInt32 GetSafetyOffset(AudioObjectPropertyScope scope) const {
        return scope == kAudioObjectPropertyScopeInput ? mSafetyOffsetIn : mSafetyOffsetOut;
    }
    UInt32 GetZeroTimestampPeriod() const;
    
    void FillPreferredChannelLayout(int device, AudioObjectPropertyScope inScope, AudioChannelLayout* layout) const;
    UInt32 GetPreferredChannelLayoutSize(int device) const;
    
    // Output volume and mute, applied to the mix before it is encoded
    Float32 GetVolumeScalar() const { return mVolumeScalar.load(); }
    void SetVolumeScalar(Float32 scalar);
    bool IsMuted() const { return mMuted.load(); }
    void SetMuted(bool muted);

    // Per-channel names, and the channels stereo clients use, for each scope
    CFStringRef CopyChannelName(int device, AudioObjectPropertyScope scope, AudioObjectPropertyElement element) const;
    OzzyStereoPair GetStereoPair(int device, AudioObjectPropertyScope scope) const;
    OSStatus SetStereoPair(AudioObjectPropertyScope scope, OzzyStereoPair pair);

    OSStatus ioOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer);

private:
    OzzyHAL() = default;
    ~OzzyHAL() = default;

    CFStringRef mDeviceUID = nullptr;
    CFStringRef mModelUID = nullptr;
    CFStringRef mManufacturerUID = nullptr;

    /* State kept for each entry of kOzzyDevices. */
    struct DeviceState {
        std::atomic<uint32_t> clients { 0 };   // StartIO calls minus StopIO calls on this device
        std::atomic<uint64_t> seed { 1 };      // zero timestamp seed
        // Last anchor served by GetZeroTimeStamp(), from this device's IO thread
        uint64_t anchorSample = 0, anchorHost = 0;
        // UID and model UID derived from mDeviceUID / mModelUID; the main device's are those
        CFStringRef uid = nullptr, model = nullptr;
        CFStringRef name = nullptr;   // fixed name, or nullptr for the product name (mModelUID)
    };
    DeviceState mDevices[kOzzyMaxDevices];
    
    uint32_t mZeroTimestampPeriod = 640;  // Updated dynamically: 2 URBs * framesPerPacket * 4
    uint8_t* mInputBufferAddr = nullptr;
    uint8_t* mOutputBufferAddr = nullptr;
    
    // Cached frame format for real-time I/O (updated on connect)
    uint32_t mFramesPerPacket = 80;
    uint32_t mRingSize = 10240; // kOzzyNumPackets * mFramesPerPacket
    uint32_t mOutputBytesPerFrame = 48;
    uint32_t mInputBytesPerFrame = 64;

    // Device shape, set by ApplyPacketLayout() / ApplyFrameLayout() on attach
    uint32_t mChannels = 8;
    uint32_t mRingSizeIn = 10240;        // input ring, frames (kext: same as mRingSize)
    uint32_t mSafetyOffsetOut = 160;
    uint32_t mSafetyOffsetIn = 160;
    bool mIsDynacord = false;

    // Set when the mapping must no longer be touched by the IO path but IO is
    // still running; the monitor unmaps once StopIO has been seen.
    std::atomic<bool> mMappingDead { false };
    std::atomic<uint32_t> mIOClients { 0 };   // StartIO calls minus StopIO calls on every device, plus a running rate change
    std::atomic<bool> mRateChangeActive { false };   // PerformSampleRateChange is waiting for the engine

    // Daemon liveness (layout v2 only)
    uint32_t mLastHeartbeat = 0;
    uint64_t mHeartbeatSeenAt = 0;       // mach_absolute_time of the last change

    // Codec function pointers (set based on device vendor/product ID)
    PCMEncoderFunc mEncode = nullptr;
    PCMDecoderFunc mDecode = nullptr;
    WriteOutputFunc mWriteOutput = nullptr;
    ReadInputFunc mReadInput = nullptr;
    ClearOutputFunc mClearOutput = nullptr;
    
    AudioStreamBasicDescription mCurrentStreamFormat;
    AudioBufferList mInputConfig;
    AudioBufferList mOutputConfig;
    AudioBufferList mMonoInputConfig;
    AudioBufferList mNoConfig;   // an input-only device's output scope
    AudioValueRange mSampleRateList[3] = {};
    UInt32 mNumSampleRates = 1;
    
    std::atomic<Float32> mVolumeScalar { 1.0f };
    std::atomic<bool> mMuted { false };
    float mAppliedGain = 1.0f;   // IO thread only: the gain the last WriteMix ended on
    std::atomic<uint32_t> mStereoPairIn { OzzyStereoPairPack({}) };
    std::atomic<uint32_t> mStereoPairOut { OzzyStereoPairPack({}) };

    AudioServerPlugInHostRef mHost = nullptr;
    OzzySharedMemory* mSHM = nullptr;
    
    // --- Kext Support ---
    io_connect_t mKextConnect = IO_OBJECT_NULL;
    mach_vm_address_t mKextMapAddress = 0;
    mach_vm_size_t mKextMapSize = 0;
    bool mIsKextMapping = false;

    // --- Legacy Daemon Support ---
    int mSHMFd = -1;

    uint32_t mLastSessionID = 0;

    pthread_t mMonitorThread;
    std::atomic<bool> mMonitorRunning { false };
    bool mLastConnectedState = false;
    bool mLoggedWaitingForDriver = false;

    void MapSharedMemory();
    void UnmapSharedMemory();
    static void* MonitorEntry(void* arg);
    void MonitorLoop();
    void RebuildStreamConfigs();
    void SetIdentity(const char* product, const char* manufacturer, const char* serial);
    void RebuildDeviceStrings();
    bool ApplyPacketLayout();
    bool ApplyFrameLayout();
    bool FrameLayoutUnchanged() const;
    void DropMapping();

    void ClearOutputBuffer();
    void LoadControlState();
    void SaveControlState();
    void NotifyControlChanged(AudioObjectID control, const AudioObjectPropertySelector* selectors, UInt32 count);
    void NotifyDeviceChanged(AudioObjectID device, AudioObjectPropertySelector selector, AudioObjectPropertyScope scope);
    void NotifyAllDevicesChanged(AudioObjectPropertySelector selector);
};

#endif