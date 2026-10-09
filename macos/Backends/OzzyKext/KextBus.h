/* SPDX-License-Identifier: MIT */
#pragma once
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOService.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostPipe.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h> // REQUIRED for withSubRange
#include "../../Devices/Ploytec/PloytecBus.h"

/*
 * IOzzyBus on IOUSBHost in the kernel. Engine callbacks run one at a time
 * under a recursive lock; USB completions never wait for it. They queue a
 * record and the lock holder delivers it before releasing (see Unlock).
 */
class KextBus : public PloytecBus {
public:
    enum : uint32_t {
        kMaxPipes = 16,
        kMaxBuffers = 64,       /* engine-owned buffers with a cached descriptor */
        kMaxIsochSlots = 48,    /* isochronous transfers in flight */
        kMaxIsochTxns = 32,     /* service intervals per isochronous transfer */
        kIsochLeadFrames = 4,   /* ms from "now" to the first frame of a new run */
        kQueueLen = 256,        /* queued completions; > everything in flight */
    };

    KextBus(IOUSBHostDevice* device, IOService* driver);
    virtual ~KextBus();

    // IOzzyBus Implementation
    bool SubmitUSBPacket(uint8_t pipeID, void* buffer, uint32_t size, void* contextID) override;
    bool SubmitIsochPacket(uint8_t pipeID, void* buffer, OzzyIsochFrame* frames, uint32_t frameCount, void* contextID) override;
    bool GetBusFrame(uint64_t* frame, uint64_t* hostTime) override;
    bool VendorRequest(uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void* data, uint16_t len) override;
    void Sleep(uint32_t ms) override;
    uint64_t GetTime() override;
    void Log(const char* fmt, ...) override;
    void SetEngine(OzzyEngine* engine) override;

    // Receive the Master Shared Memory Descriptor
    void SetSharedMemory(IOBufferMemoryDescriptor* mainDesc, void* virtBase);
    OzzyEngine* GetEngine();

    /* Register an open pipe under its endpoint address; the caller keeps its reference. */
    bool AddPipe(uint8_t address, IOUSBHostPipe* pipe);
    /* Forget all pipes and release cached buffer descriptors. Only after
     * AbortAndDrain, and before the engine (whose buffers they wire) is freed. */
    void ClearPipes();
    /* Take the engine lock (recursive). Hold it around Start, Stop and Poll. */
    void Lock();
    /* Deliver queued completions (at depth 1), then release the engine lock. */
    void Unlock();
    /*
     * Abort every pipe synchronously, deliver the queued completions, and wait
     * up to timeoutMs for the rest. True means every submitted transfer has
     * been delivered and no USB completion callback is inside KextBus or will
     * enter it for them, so the bus may be freed once the caller's own threads
     * are done with it. False means some still may: leak the bus rather than
     * free it. The caller must hold neither the engine lock (the drain would
     * be nested and deliver nothing) nor any USB work loop gate, and must not
     * be a USB completion callback (the synchronous abort waits for those).
     */
    bool AbortAndDrain(uint32_t timeoutMs);

    struct IsochSlot;
    struct Completion {
        uint8_t pipe;
        bool isoch;
        void* ctx;
        int status;
        uint32_t bytes;
        IsochSlot* slot;
    };
    /* First call of every USB completion callback that reaches Post. */
    void EnterCallback();
    /* Called from USB completion context. Never blocks on the engine lock. Its
     * last access to the bus ends the EnterCallback that preceded it. */
    void Post(const Completion& c);

    struct IsochSlot {
        bool used;
        uint8_t pipe;
        void* ctx;
        OzzyIsochFrame* frames;
        uint32_t count;
        uint64_t startFrame;
        IOUSBHostIsochronousTransaction txn[kMaxIsochTxns];
    };

private:
    struct PipeEntry {
        uint8_t address;
        IOUSBHostPipe* pipe;
        uint32_t intervalUframes;   /* iso service interval in 125 us microframes */
        uint32_t maxPacket;
        uint64_t nextFrame;         /* next 1 ms bus frame of the current iso run */
        bool running;               /* an iso run is scheduled */
    };
    struct BufferEntry {
        const void* addr;
        uint32_t len;
        IOMemoryDescriptor* md;     /* prepared */
    };

    PipeEntry* FindPipe(uint8_t address);
    IOMemoryDescriptor* DescriptorFor(void* buffer, uint32_t len, uint8_t pipeID);
    void Dispatch(const Completion& c);
    bool PopQueued(Completion* c);
    bool Busy();

    IOUSBHostDevice* mDevice;
    IOService* mDriver;
    OzzyEngine* mEngine;

    // Master Memory References
    IOBufferMemoryDescriptor* mMainShmDesc;
    uintptr_t mShmBase;
    IOByteCount mShmSize;

    PipeEntry mPipes[kMaxPipes];
    uint32_t mPipeCount;
    BufferEntry mBuffers[kMaxBuffers];
    uint32_t mBufferCount;
    IsochSlot mIsoch[kMaxIsochSlots];

    IORecursiveLock* mLock;
    uint32_t mDepth;                /* lock depth, touched only while holding mLock */
    IOSimpleLock* mQueueLock;
    Completion mQueue[kQueueLen];
    uint32_t mQueueHead, mQueueLen;
    volatile SInt32 mOutstanding;   /* submitted, not yet delivered */
    volatile SInt32 mInCallbacks;   /* USB completion callbacks between EnterCallback and Post's end */
};
