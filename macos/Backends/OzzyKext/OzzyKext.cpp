#include "OzzyKext.h"
#include "KextBus.h"
#include "../../OzzyCore/OzzyFactory.h"
#include "../../Shared/OzzySharedData.h"
#include "../../Shared/OzzyLog.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>

#include <IOKit/IOUserClient.h>

// --- OzzyUserClient Implementation ---

class OzzyUserClient : public IOUserClient {
    OSDeclareDefaultStructors(OzzyUserClient)

public:
    virtual bool initWithTask(task_t t, void* s, UInt32 type, OSDictionary* p) override {
        if (!IOUserClient::initWithTask(t, s, type, p)) return false;
        return true;
    }

    virtual bool start(IOService* provider) override {
        mOwner = OSDynamicCast(OzzyKext, provider);
        if (!mOwner) return false;
        return IOUserClient::start(provider);
    }

    // This is triggered by OzzyHAL calling IOConnectMapMemory
    virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits* options, 
                                         IOMemoryDescriptor** memory) override {
        // type 0 is what the HAL uses by default
        if (mOwner) {
            IOMemoryDescriptor* shm = mOwner->GetMemory();
            if (shm) {
                shm->retain(); // The framework will release it after the mapping is established
                *memory = shm;
                return kIOReturnSuccess;
            }
        }
        return kIOReturnNoMemory;
    }

    // The client closed its connection (or its task died): terminate so
    // IOKit detaches and frees this user client instead of leaking one per
    // HAL or MIDI reconnect.
    virtual IOReturn clientClose() override {
        terminate();
        return kIOReturnSuccess;
    }

private:
    OzzyKext* mOwner = nullptr;
};

OSDefineMetaClassAndStructors(OzzyUserClient, IOUserClient)

// --- OzzyKext Implementation of newUserClient ---

IOReturn OzzyKext::newUserClient(task_t owningTask, void* securityID, 
                                 UInt32 type, OSDictionary* properties, 
                                 IOUserClient** handler) {
    OzzyUserClient* client = new OzzyUserClient;
    
    if (!client || !client->init()) {
        if (client) client->release();
        return kIOReturnNoMemory;
    }

    if (!client->attach(this)) {
        client->release();
        return kIOReturnError;
    }

    if (!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnError;
    }

    *handler = client;
    return kIOReturnSuccess;
}

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static void GetRegistryString(IOService* service, const char* key, char* dest, size_t maxLen) {
    if (!service || !dest || maxLen == 0) return;
    dest[0] = '\0';
    
    OSObject* obj = service->copyProperty(key);
    if (!obj) return;
    
    OSString* str = OSDynamicCast(OSString, obj);
    if (str) {
        const char* cstr = str->getCStringNoCopy();
        if (cstr) {
            size_t len = 0;
            while (cstr[len] && len < maxLen - 1) {
                dest[len] = cstr[len];
                len++;
            }
            dest[len] = '\0';
        }
    }
    obj->release();
}

OSDefineMetaClassAndStructors(OzzyKext, IOService)

bool OzzyKext::start(IOService* provider) {
    if (!IOService::start(provider)) return false;
    LogOzzyKext("start");

    IOUSBHostDevice* device = OSDynamicCast(IOUSBHostDevice, provider);
    if (!device || !device->open(this)) {
        LogOzzyKext("open failed");
        return false;
    }
    mHostDevice = device;

    if (!MapSharedMemory()) { Teardown(); return false; }

    const StandardUSB::DeviceDescriptor* desc = mHostDevice->getDeviceDescriptor();
    if (!desc) { Teardown(); return false; }

    mEngine = OzzyFactory::CreateEngine(desc->idVendor, desc->idProduct);
    mBus = new KextBus(mHostDevice, this);
    if (!mEngine || !mBus) { Teardown(); return false; }

    // The HAL picks its decoder from the layout version, so it must be set
    // before the identity it waits for (vendorID) is published.
    mSHM->version = mEngine->LayoutVersion();

    // Extract device metadata early so HAL can see it immediately
    mSHM->vendorID = desc->idVendor;
    mSHM->productID = desc->idProduct;

    // Get device strings from IORegistry
    GetRegistryString(mHostDevice, "USB Vendor Name", mSHM->manufacturerName, 64);
    GetRegistryString(mHostDevice, "USB Product Name", mSHM->productName, 64);
    GetRegistryString(mHostDevice, "USB Serial Number", mSHM->serialNumber, 64);

    LogOzzyKext("found device: %s %s (S/N: %s)", mSHM->manufacturerName, mSHM->productName,
        mSHM->serialNumber[0] ? mSHM->serialNumber : "none");
    LogOzzyKext("  USB ID: %04x:%04x", desc->idVendor, desc->idProduct);
    LogOzzyKext("  speed: %s",
        mHostDevice->getSpeed() == kUSBHostConnectionSpeedHigh ? "high" :
        mHostDevice->getSpeed() == kUSBHostConnectionSpeedFull ? "full" : "unknown");

    mBus->SetEngine(mEngine);
    mBus->SetSharedMemory(mShmDescriptor, mSHM);

    LogOzzyKext("configuring hardware...");
    if (!OpenInterfaces() || !OpenPipes()) {
        LogOzzyKext("configuration failed");
        Teardown();
        return false;
    }

    LogOzzyKext("initializing engine...");
    mEngine->Init(mBus, mSHM);
    mBus->Lock();
    bool started = mEngine->Start();
    mBus->Unlock();
    // Stay attached if the first start fails: tearing down here leaves the
    // mixer needing a power-cycle, while the poll timer's restart (Poll()
    // reports the silent stream) retries it once a second.
    if (!started) LogOzzyKext("engine start failed; retrying from the poll timer");
    mLastRestart = mach_absolute_time();

    mWorkLoop = IOWorkLoop::workLoop();
    mPollTimer = IOTimerEventSource::timerEventSource(this,
        OSMemberFunctionCast(IOTimerEventSource::Action, this, &OzzyKext::PollTimerFired));
    if (mWorkLoop && mPollTimer && mWorkLoop->addEventSource(mPollTimer) != kIOReturnSuccess) {
        // Never added, so Teardown must not remove it
        mPollTimer->release();
        mPollTimer = nullptr;
    }
    if (!mWorkLoop || !mPollTimer) {
        LogOzzyKext("poll timer setup failed");
        Teardown();
        return false;
    }
    mPollTimer->setTimeoutMS(100);

    LogOzzyKext("driver ready%s", started ? "" : " (stream not running yet)");
    registerService();
    return true;
}

IOReturn OzzyKext::message(UInt32 type, IOService* provider, void* argument) {
    if (type == kIOMessageServiceIsTerminated) {
        LogOzzyKext("device disconnected");

        // No more polls or restarts; Teardown removes the timer for good
        mTerminating = true;
        if (mPollTimer) mPollTimer->cancelTimeout();

        // Immediately signal HAL that hardware is gone
        if (mSHM) {
            mSHM->audio.hardwarePresent = false;
            mSHM->audio.driverReady = false;
        }

        // No engine lock and no engine call here: this may run holding the
        // provider's gate, which a thread holding the engine lock can be
        // waiting on (resubmits, control requests). Teardown, on the thread
        // stop() starts, stops the engine under the lock, then aborts and
        // drains.
    }

    // MUST call super - failure to do so causes termination timeout and kernel panic
    return IOService::message(type, provider, argument);
}

/*
 * Stop polling, tell the HAL the hardware is gone, and close the interfaces
 * and the device. Runs holding the provider's gate, so it never waits: no
 * timer removal, no bus lock, no drain. Closing a provider re-enters the gate
 * this thread already holds. The interfaces stay retained and the members
 * stay set, because a poll handler in RestartStream may still use them;
 * Teardown releases them.
 */
void OzzyKext::CloseProviders() {
    mTerminating = true;
    if (mPollTimer) mPollTimer->cancelTimeout();

    if (mSHM) {
        mSHM->magic = 0;
        mSHM->audio.hardwarePresent = false;
        mSHM->audio.driverReady = false;
    }

    if (mProvidersClosed) return;
    for (int i = 0; i < kMaxInterfaces; i++)
        if (mInterfaces[i]) mInterfaces[i]->close(this);
    if (mHostDevice) mHostDevice->close(this);
    mProvidersClosed = true;
}

/*
 * The device is going away. IOKit only calls stop() once its clients have
 * closed it, so close the interfaces and the device here, without waiting:
 * this runs holding the provider's gate.
 */
bool OzzyKext::willTerminate(IOService* provider, IOOptionBits options) {
    LogOzzyKext("will terminate");
    CloseProviders();
    return IOService::willTerminate(provider, options);
}

/*
 * Runs holding the provider's gate, so the blocking Teardown goes to a
 * thread call. Its retains on this instance and on the device keep both
 * alive until Teardown finishes; a live instance also keeps the kext loaded.
 */
void OzzyKext::stop(IOService* provider) {
    LogOzzyKext("stop");
    CloseProviders();

    retain();
    if (mHostDevice) mHostDevice->retain();
    mStopCall = thread_call_allocate_with_options(&OzzyKext::StopThread, this,
        THREAD_CALL_PRIORITY_KERNEL, 0);
    if (mStopCall) {
        thread_call_enter1(mStopCall, mHostDevice);
    } else {
        // Keep both retains: a poll handler may still be using the device,
        // and freeing this instance with the timer still on our work loop
        // would be a use after free.
        LogOzzyKext("stop: no thread for teardown; leaking the driver");
    }
    IOService::stop(provider);
}

/*
 * The thread call stop() starts: Teardown, then drop stop()'s retains. If
 * transfers leaked, keep them: the leaked bus still points at the device and
 * at this instance, and its completions are kext code, so the kext must not
 * unload.
 */
void OzzyKext::StopThread(thread_call_param_t self, thread_call_param_t device) {
    OzzyKext* me = (OzzyKext*)self;
    me->Teardown();
    thread_call_free(me->mStopCall);
    me->mStopCall = nullptr;
    if (me->mTransfersLeaked) return;
    if (device) ((IOService*)device)->release();
    me->release();
}

/*
 * Undo start(), on the thread call stop() starts or from any failed step of
 * start(); every member may be null, and a second call does nothing. Nothing
 * a transfer may still reference is freed unless the bus has drained. It
 * waits on our work loop's gate, the bus lock and the drain, so never call it
 * holding the provider's gate: a poll handler holding those can be waiting
 * on that gate.
 */
void OzzyKext::Teardown() {
    // 0. No new polls or restarts
    mTerminating = true;

    // 1. Remove the poll timer. removeEventSource takes our work loop's gate,
    //    so it waits for a handler (and any restart) already running. This
    //    thread holds no provider gate, so that handler's USB calls finish.
    if (mPollTimer) {
        mPollTimer->cancelTimeout();
        if (mWorkLoop) mWorkLoop->removeEventSource(mPollTimer);
        mPollTimer->release();
        mPollTimer = nullptr;
    }
    if (mWorkLoop) {
        mWorkLoop->release();
        mWorkLoop = nullptr;
    }

    // 2. Invalidate shared memory magic to signal HAL immediately
    if (mSHM) {
        mSHM->magic = 0;
    }

    // 3. Stop the engine resubmitting
    if (mEngine && mBus) {
        mBus->Lock();
        mEngine->Stop();
        mBus->Unlock();
    }

    // 4. Abort every transfer and wait until each has been delivered. Once a
    //    pass has leaked, a later pass has no bus but must still not free
    //    anything the leaked transfers reference.
    bool drained = mBus ? mBus->AbortAndDrain(1000) : !mTransfersLeaked;

    // 5. Release the pipes, then the engine and the bus. If the drain timed
    //    out, a late completion may still touch any of them: leak them all.
    if (drained) {
        if (mBus) {
            mBus->Lock();
            mBus->SetEngine(nullptr);
            mBus->Unlock();
            ClosePipes();
        }
        delete mEngine;
        delete mBus;
    } else if (!mTransfersLeaked) {
        LogOzzyKext("transfers did not drain: leaking engine, bus, pipes and shared memory");
        mTransfersLeaked = true;
    }
    mEngine = nullptr;
    mBus = nullptr;
    mPipeCount = 0;

    // 6. Close and release interfaces (CloseProviders may have closed them)
    for (int i = 0; i < kMaxInterfaces; i++) {
        if (mInterfaces[i]) {
            if (!mProvidersClosed) mInterfaces[i]->close(this);
            mInterfaces[i]->release();
            mInterfaces[i] = nullptr;
        }
    }

    // 7. Unmap shared memory (the capture ring lives there, so only once drained)
    if (drained) UnmapSharedMemory();

    // 8. Close USB device (don't release - it's the provider, managed by IOKit)
    if (mHostDevice) {
        if (!mProvidersClosed) mHostDevice->close(this);
        mHostDevice = nullptr;
    }
}

void OzzyKext::free() {
    LogOzzyKext("free");
    IOService::free();
}

bool OzzyKext::MapSharedMemory() {
    // FIX: Store the descriptor in member variable, don't leak it!
    mShmDescriptor = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIOMemoryPhysicallyContiguous, sizeof(OzzySharedMemory), 0);
    
    if (!mShmDescriptor) return false;
    
    mShmDescriptor->prepare();
    mShmMap = mShmDescriptor->map();
    mSHM = (OzzySharedMemory*)mShmMap->getVirtualAddress();
    memset(mSHM, 0, sizeof(OzzySharedMemory));
    mSHM->magic = kOzzyMagic;
    mSHM->version = 1;
    
    // Generate random session ID using current time and address
    uint32_t random = (uint32_t)((uintptr_t)mSHM ^ mach_absolute_time());
    mSHM->sessionID = random;
    
    return true;
}

void OzzyKext::UnmapSharedMemory() { 
    if (mShmMap) { mShmMap->release(); mShmMap = nullptr; } 
    if (mShmDescriptor) { mShmDescriptor->release(); mShmDescriptor = nullptr; } // Release Master
    mSHM = nullptr; 
}

/*
 * Claim the profile's interfaces at its alternate settings. On a restart the
 * interfaces are already held: only reselect the alternate settings.
 */
bool OzzyKext::OpenInterfaces() {
    const OzzyDeviceProfile& profile = mEngine->GetProfile();

    int held = 0;
    for (int i = 0; i < profile.interfaceCount; i++)
        if (mInterfaces[i]) held++;
    if (held && held == profile.interfaceCount) {
        for (int i = 0; i < profile.interfaceCount; i++) {
            if (mInterfaces[i]->selectAlternateSetting(profile.altSettings[i]) != kIOReturnSuccess) return false;
            IOSleep(50);
        }
        return true;
    }

    mHostDevice->setConfiguration(1);
    IOSleep(500);

    const IORegistryPlane* plane = getPlane("IOService");
    int foundCount = 0;

    OSIterator* it = mHostDevice->getChildIterator(plane);
    if (it) {
        OSObject* obj = nullptr;
        while ((obj = it->getNextObject())) {
            IOUSBHostInterface* iface = OSDynamicCast(IOUSBHostInterface, obj);
            if (!iface) continue;
            const StandardUSB::InterfaceDescriptor* d = iface->getInterfaceDescriptor();
            if (!d) continue;

            for(int i=0; i < profile.interfaceCount; i++) {
                if (d->bInterfaceNumber == profile.interfaceIndices[i] && !mInterfaces[i]) {
                    if (iface->open(this)) {
                        iface->retain();
                        mInterfaces[i] = iface;
                        foundCount++;
                        iface->selectAlternateSetting(profile.altSettings[i]);
                        IOSleep(50);
                    }
                }
            }
        }
        it->release();
    }

    return foundCount == profile.interfaceCount;
}

/* Open every endpoint the profile configures and register it with the bus. */
bool OzzyKext::OpenPipes()
{
    const OzzyDeviceProfile& p = mEngine->GetProfile();
    const OzzyPipeConfig* cfgs[] = { &p.pcmOut, &p.pcmIn, &p.midiIn, &p.midiOut, &p.isoOut, &p.feedbackIn };
    const uint32_t maxPipes = sizeof(mPipes) / sizeof(mPipes[0]);

    for (const OzzyPipeConfig* cfg : cfgs) {
        if (cfg->address == 0) continue;
        IOUSBHostInterface* intf = nullptr;
        for (int i = 0; i < p.interfaceCount; i++)
            if (p.interfaceIndices[i] == cfg->interfaceIndex) intf = mInterfaces[i];
        if (!intf) {
            LogOzzyKext("  no interface found for pipe 0x%02x", cfg->address);
            return false;
        }
        IOUSBHostPipe* pipe = intf->copyPipe(cfg->address);
        if (!pipe || mPipeCount >= maxPipes || !mBus->AddPipe(cfg->address, pipe)) {
            LogOzzyKext("  failed to open pipe 0x%02x", cfg->address);
            if (pipe) pipe->release();
            return false;
        }
        mPipes[mPipeCount++] = pipe;
        LogOzzyKext("  pipe 0x%02x opened", cfg->address);

        // Detect bulk vs interrupt from the output endpoint descriptor
        if (cfg == &p.pcmOut) {
            const StandardUSB::EndpointDescriptor* epDesc = pipe->getEndpointDescriptor();
            if (epDesc) mEngine->SetBulk((epDesc->bmAttributes & 0x03) == 0x02);
        }
    }
    return true;
}

/* Forget the pipes on the bus and drop our references. Only once drained. */
void OzzyKext::ClosePipes()
{
    mBus->ClearPipes();
    for (uint32_t i = 0; i < mPipeCount; i++) {
        mPipes[i]->release();
        mPipes[i] = nullptr;
    }
    mPipeCount = 0;
}

/* A new nonzero session ID, different from the current one. */
void OzzyKext::NewSession()
{
    uint32_t old = mSHM->sessionID, s;
    do {
        s = (uint32_t)(mach_absolute_time() ^ ((uintptr_t)this >> 4));
    } while (s == old || s == 0);
    mSHM->sessionID = s;
}

/* Every 100 ms on our own work loop: liveness for the HAL, then let the
 * engine decide whether its stream needs restarting. */
void OzzyKext::PollTimerFired(IOTimerEventSource* sender)
{
    if (mTerminating || !mEngine) return;
    mSHM->heartbeat = mSHM->heartbeat + 1;
    mBus->Lock();
    if (mTerminating) {
        mBus->Unlock();
        return;
    }
    OzzyEngine::PollResult r = mEngine->Poll();
    mBus->Unlock();
    if (r == OzzyEngine::PollResult::kRestart && !mTerminating) {
        uint64_t oneSecond;
        nanoseconds_to_absolutetime(1000000000ull, &oneSecond);
        if (mach_absolute_time() - mLastRestart >= oneSecond) RestartStream();
    }
    if (!mTerminating) sender->setTimeoutMS(100);
}

/*
 * Stop the engine, abort and drain every transfer, reselect the alternate
 * settings (output stays silent after a rate change otherwise, as found on
 * Linux), then start again at the engine's next rate in a new session. The
 * HAL's rate change waits for that session change.
 */
void OzzyKext::RestartStream()
{
    mLastRestart = mach_absolute_time();
    LogOzzyKext("restarting stream at %u Hz", mEngine->NextRate());
    mBus->Lock();
    mEngine->Stop();
    mBus->Unlock();
    if (!mBus->AbortAndDrain(1000)) return;   // try again on a later poll
    if (mTerminating) return;                 // Teardown finishes the job
    ClosePipes();
    if (!OpenInterfaces() || !OpenPipes()) {
        LogOzzyKext("restart: reconfiguration failed");
        return;
    }
    mBus->Lock();
    if (mTerminating) {
        mBus->Unlock();
        return;
    }
    NewSession();
    mEngine->SetRate(mEngine->NextRate());
    bool ok = mEngine->Start();
    mBus->Unlock();
    if (!ok) LogOzzyKext("restart: engine start failed; retrying");
}

extern "C" {
    kern_return_t OzzyKext_start(kmod_info_t * ki, void *d) { return KERN_SUCCESS; }
    kern_return_t OzzyKext_stop(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
    __attribute__((visibility("default"))) kmod_start_func_t *_realmain = OzzyKext_start;
    __attribute__((visibility("default"))) kmod_stop_func_t  *_antimain = OzzyKext_stop;
    __attribute__((visibility("default"))) kmod_info_t kmod_info = {
        0, KMOD_INFO_VERSION, 0, "com.ozzy.kext.OzzyKext", "1.0.0", -1, 0, 0, 0, 0, OzzyKext_start, OzzyKext_stop
    };
}