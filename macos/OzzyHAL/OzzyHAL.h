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

// Codec function pointer types for device-agnostic encoding/decoding
typedef void (*PCMEncoderFunc)(uint8_t* dst, const float* src);
typedef void (*PCMDecoderFunc)(float* dst, const uint8_t* src);
typedef void (*WriteOutputFunc)(uint8_t* ringBuffer, const float* srcFrames, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame);
typedef void (*ReadInputFunc)(float* dstFrames, const uint8_t* ringBuffer, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame);
typedef void (*ClearOutputFunc)(uint8_t* outputBuffer, uint32_t bufferSize);

#define kOzzyDeviceID 100
#define kOzzyInputStreamID 150
#define kOzzyOutputStreamID 200
#define kOzzyVolumeControlID 300
#define kOzzyMuteControlID 301

class OzzyHAL {
public:
    static OzzyHAL& Get();
    
    void Init(AudioServerPlugInHostRef host);
    void Cleanup();

    bool IsConnected();
    bool IsRunning() const { return mIOStarted; }

    kern_return_t StartIO();
    kern_return_t StopIO();

    HRESULT GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed);
    
    Float64 GetNominalSampleRate() const;
    bool IsRateSettable() const { return mIsDynacord; }
    UInt32 GetAvailableSampleRateCount() const { return mNumSampleRates; }
    const AudioValueRange* GetAvailableSampleRateList() const { return mSampleRateList; }
    OSStatus RequestSampleRate(Float64 rate);
    OSStatus PerformSampleRateChange(UInt64 rate);
    AudioBufferList* GetInputStreamConfiguration() const;
    AudioBufferList* GetOutputStreamConfiguration() const;
    CFStringRef GetDeviceName() const;
    CFStringRef GetManufacturer() const;
    CFStringRef GetDeviceUID() const;
    CFStringRef GetModelUID() const;
    UInt32 GetTransportType() const;
    UInt32 GetClockDomain() const;
    AudioObjectID GetInputStreamID() const;
    AudioObjectID GetOutputStreamID() const;
    AudioStreamBasicDescription GetStreamFormat() const;
    
    UInt32 GetLatency() const { return 0; }
    UInt32 GetSafetyOffset(AudioObjectPropertyScope scope) const {
        return scope == kAudioObjectPropertyScopeInput ? mSafetyOffsetIn : mSafetyOffsetOut;
    }
    UInt32 GetZeroTimestampPeriod() const;
    
    AudioChannelLayout* GetPreferredChannelLayout(AudioObjectPropertyScope inScope);
    UInt32 GetPreferredChannelLayoutSize(AudioObjectPropertyScope inScope) const;
    
    // Output volume and mute, applied to the mix before it is encoded
    Float32 GetVolumeScalar() const { return mVolumeScalar.load(); }
    void SetVolumeScalar(Float32 scalar);
    bool IsMuted() const { return mMuted.load(); }
    void SetMuted(bool muted);

    OSStatus ioOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer);

private:
    OzzyHAL() = default;
    ~OzzyHAL() = default;

    CFStringRef mDeviceUID = nullptr;
    CFStringRef mModelUID = nullptr;
    CFStringRef mManufacturerUID = nullptr;
    
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
    std::atomic<uint32_t> mIOClients { 0 };   // StartIO calls minus StopIO calls (one per client), plus a running rate change
    std::atomic<bool> mRateChangeActive { false };   // PerformSampleRateChange is waiting for the engine

    // Daemon liveness (layout v2 only)
    uint32_t mLastHeartbeat = 0;
    uint64_t mHeartbeatSeenAt = 0;       // mach_absolute_time of the last change

    // Last anchor served by GetZeroTimeStamp()
    uint64_t mLastAnchorSample = 0, mLastAnchorHost = 0;
    
    // Codec function pointers (set based on device vendor/product ID)
    PCMEncoderFunc mEncode = nullptr;
    PCMDecoderFunc mDecode = nullptr;
    WriteOutputFunc mWriteOutput = nullptr;
    ReadInputFunc mReadInput = nullptr;
    ClearOutputFunc mClearOutput = nullptr;
    
    AudioStreamBasicDescription mCurrentStreamFormat;
    AudioBufferList mInputConfig;
    AudioBufferList mOutputConfig;
    AudioValueRange mSampleRateList[3] = {};
    UInt32 mNumSampleRates = 1;
    
    bool mIOStarted = false;

    std::atomic<Float32> mVolumeScalar { 1.0f };
    std::atomic<bool> mMuted { false };
    float mAppliedGain = 1.0f;   // IO thread only: the gain the last WriteMix ended on
    uint32_t mTimestampSeed = 1;

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
    alignas(16) uint8_t mChannelLayoutBuffer[0x50C];

    void MapSharedMemory();
    void UnmapSharedMemory();
    static void* MonitorEntry(void* arg);
    void MonitorLoop();
    void RebuildStreamConfigs();
    bool ApplyPacketLayout();
    bool ApplyFrameLayout();
    bool FrameLayoutUnchanged() const;
    void DropMapping();

    void ClearOutputBuffer();
    void LoadControlState();
    void SaveControlState();
    void NotifyControlChanged(AudioObjectID control, const AudioObjectPropertySelector* selectors, UInt32 count);
};

#endif