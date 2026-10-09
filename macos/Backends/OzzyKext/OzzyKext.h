#ifndef OzzyKext_h
#define OzzyKext_h

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/IOUSBHostPipe.h>
#include <IOKit/IOBufferMemoryDescriptor.h> // Added include
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <kern/thread_call.h>

class KextBus;
class OzzyEngine;
struct OzzySharedMemory;

#define kMaxInterfaces 4

class OzzyKext : public IOService {
    OSDeclareDefaultStructors(OzzyKext)

public:
    virtual bool start(IOService* provider) override;
    virtual void stop(IOService* provider) override;
    virtual bool willTerminate(IOService* provider, IOOptionBits options) override;
   	virtual IOReturn newUserClient(task_t t, void* sec, UInt32 type, OSDictionary* p, IOUserClient** h) override;
    virtual void free() override; // Added free
    virtual IOReturn message(UInt32 type, IOService* provider, void* argument) override;

    IOMemoryDescriptor* GetMemory() { return mShmDescriptor; }
    OzzySharedMemory* GetSHM() { return mSHM; }

private:
    bool MapSharedMemory();
    void UnmapSharedMemory();
    void Teardown();
    void CloseProviders();
    static void StopThread(thread_call_param_t self, thread_call_param_t device);
    bool OpenInterfaces();
    bool OpenPipes();
    void ClosePipes();
    void PollTimerFired(IOTimerEventSource* sender);
    void RestartStream();
    void NewSession();

    IOUSBHostDevice* mHostDevice = nullptr;
    KextBus* mBus = nullptr;
    OzzyEngine* mEngine = nullptr;

    IOWorkLoop* mWorkLoop = nullptr;            // ours, not the provider's
    IOTimerEventSource* mPollTimer = nullptr;
    uint64_t mLastRestart = 0;                  // mach_absolute_time of the last restart
    volatile bool mTerminating = false;
    bool mTransfersLeaked = false;              // a Teardown found transfers that never drained
    bool mProvidersClosed = false;              // the interfaces and the device are closed, not yet released
    thread_call_t mStopCall = nullptr;          // runs Teardown after stop()
    
    // MASTER MEMORY OBJECTS
    IOBufferMemoryDescriptor* mShmDescriptor = nullptr; // The Owner
    IOMemoryMap* mShmMap = nullptr;                     // The Mapping
    OzzySharedMemory* mSHM = nullptr;                   // The Pointer

    IOUSBHostInterface* mInterfaces[kMaxInterfaces] = { nullptr };
    IOUSBHostPipe* mPipes[16]                  = { nullptr };
    uint32_t mPipeCount = 0;
};

#endif