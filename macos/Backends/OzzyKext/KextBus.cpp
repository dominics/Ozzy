/* SPDX-License-Identifier: MIT */
#include "KextBus.h"
#include "KextStatus.h"
#include "../../Shared/OzzyLog.h"
#include <stdarg.h>

// --- Bulk context (keeps a shm slice alive until completion) ---
struct TransferContext {
    IOMemoryDescriptor* slice;   /* a shm slice to complete/release, or null for a cached buffer */
    void* userContext;
    uint8_t pipeID;
    KextBus* busInstance;
};

static void KextBus_BulkCompletion(void* owner, void* parameter, IOReturn status, uint32_t bytesTransferred)
{
    TransferContext* ctx = (TransferContext*)parameter;
    if (!ctx) {
        LogOzzyKext("callback: NULL context");
        return;
    }
    ctx->busInstance->EnterCallback();
    if (status != kIOReturnSuccess && status != kIOReturnAborted && status != kIOReturnUnderrun)
        LogOzzyKext("pipe 0x%02x error: 0x%x", ctx->pipeID, status);
    if (ctx->slice) {
        ctx->slice->complete((ctx->pipeID & 0x80) ? kIODirectionIn : kIODirectionOut);
        ctx->slice->release();
    }
    KextBus::Completion c = { ctx->pipeID, false, ctx->userContext, KextTransferStatus(ctx->pipeID, status),
                              bytesTransferred, nullptr };
    KextBus* bus = ctx->busInstance;
    delete ctx;
    bus->Post(c);
}

static void KextBus_IsochCompletion(void* owner, void* parameter, IOReturn status,
                                    IOUSBHostIsochronousTransaction* txns)
{
    KextBus* bus = (KextBus*)owner;
    KextBus::IsochSlot* s = (KextBus::IsochSlot*)parameter;
    bus->EnterCallback();
    for (uint32_t i = 0; i < s->count; i++) {
        /* Never report more than was asked for; the engine sizes reads by it. */
        uint32_t got = txns[i].completeCount;
        s->frames[i].actCount = (uint16_t)(got < s->frames[i].reqCount ? got : s->frames[i].reqCount);
        s->frames[i].status = KextTransferStatus(s->pipe, txns[i].status);
    }
    KextBus::Completion c = { s->pipe, true, s->ctx, KextTransferStatus(s->pipe, status), 0, s };
    bus->Post(c);
}

KextBus::KextBus(IOUSBHostDevice* device, IOService* driver)
    : mDevice(device), mDriver(driver), mEngine(nullptr), mMainShmDesc(nullptr), mShmBase(0), mShmSize(0),
      mPipeCount(0), mBufferCount(0), mDepth(0), mQueueHead(0), mQueueLen(0), mOutstanding(0),
      mInCallbacks(0)
{
    bzero(mPipes, sizeof(mPipes));
    bzero(mBuffers, sizeof(mBuffers));
    bzero(mIsoch, sizeof(mIsoch));
    mLock = IORecursiveLockAlloc();
    mQueueLock = IOSimpleLockAlloc();
}

KextBus::~KextBus()
{
    ClearPipes();
    if (Busy()) {
        LogOzzyKext("transfers still outstanding: leaking the bus locks");
        return;
    }
    if (mQueueLock) IOSimpleLockFree(mQueueLock);
    if (mLock) IORecursiveLockFree(mLock);
}

void KextBus::SetEngine(OzzyEngine* engine) { mEngine = engine; }
OzzyEngine* KextBus::GetEngine() { return mEngine; }

void KextBus::SetSharedMemory(IOBufferMemoryDescriptor* mainDesc, void* virtBase)
{
    mMainShmDesc = mainDesc;
    mShmBase = (uintptr_t)virtBase;
    mShmSize = mainDesc ? mainDesc->getLength() : 0;
}

bool KextBus::AddPipe(uint8_t address, IOUSBHostPipe* pipe)
{
    if (!pipe || mPipeCount >= kMaxPipes) return false;
    const StandardUSB::EndpointDescriptor* d = pipe->getEndpointDescriptor();
    PipeEntry& e = mPipes[mPipeCount++];
    e = PipeEntry();
    e.address = address;
    e.pipe = pipe;
    e.maxPacket = d ? (d->wMaxPacketSize & 0x7FF) : 0;
    /* High-speed periodic interval: 2^(bInterval-1) microframes */
    e.intervalUframes = (d && d->bInterval >= 1 && d->bInterval <= 16) ? (1u << (d->bInterval - 1)) : 8;
    return true;
}

KextBus::PipeEntry* KextBus::FindPipe(uint8_t address)
{
    for (uint32_t i = 0; i < mPipeCount; i++)
        if (mPipes[i].address == address) return &mPipes[i];
    return nullptr;
}

void KextBus::ClearPipes()
{
    /* In-flight transfers may still reference the descriptors: leak them. */
    if (Busy()) {
        LogOzzyKext("%d transfers outstanding: leaking %u buffer descriptors",
                    (int)__atomic_load_n(&mOutstanding, __ATOMIC_SEQ_CST), mBufferCount);
    } else {
        for (uint32_t i = 0; i < mBufferCount; i++) {
            mBuffers[i].md->complete();
            mBuffers[i].md->release();
        }
    }
    mBufferCount = 0;
    mPipeCount = 0;
}

/* A prepared descriptor over an engine-owned buffer, created once and reused.
 * Engine buffers live as long as the engine, which outlives the pipes. */
IOMemoryDescriptor* KextBus::DescriptorFor(void* buffer, uint32_t len, uint8_t pipeID)
{
    for (uint32_t i = 0; i < mBufferCount; i++)
        if (mBuffers[i].addr == buffer && mBuffers[i].len >= len) return mBuffers[i].md;
    if (mBufferCount >= kMaxBuffers) {
        LogOzzyKext("too many transfer buffers");
        return nullptr;
    }
    IODirection dir = (pipeID & 0x80) ? kIODirectionIn : kIODirectionOut;
    IOMemoryDescriptor* md = IOMemoryDescriptor::withAddress(buffer, len, dir);
    if (!md) return nullptr;
    if (md->prepare(dir) != kIOReturnSuccess) {
        md->release();
        return nullptr;
    }
    mBuffers[mBufferCount++] = { buffer, len, md };
    return md;
}

bool KextBus::SubmitUSBPacket(uint8_t pipeID, void* buffer, uint32_t size, void* contextID)
{
    PipeEntry* pe = FindPipe(pipeID);
    if (!pe) return false;
    IODirection dir = (pipeID & 0x80) ? kIODirectionIn : kIODirectionOut;
    uintptr_t ptr = (uintptr_t)buffer;
    IOMemoryDescriptor* md;
    IOMemoryDescriptor* slice = nullptr;

    if (mMainShmDesc && ptr >= mShmBase && ptr + size <= mShmBase + mShmSize) {
        // Zero copy: a sub-descriptor viewing the shared memory
        slice = IOSubMemoryDescriptor::withSubRange(mMainShmDesc, ptr - mShmBase, size, dir);
        if (!slice) return false;
        slice->prepare(dir);
        md = slice;
    } else {
        md = DescriptorFor(buffer, size, pipeID);
        if (!md) return false;
    }

    TransferContext* ctx = new TransferContext;
    if (!ctx) {
        if (slice) { slice->complete(dir); slice->release(); }
        return false;
    }
    ctx->slice = slice;
    ctx->userContext = contextID;
    ctx->pipeID = pipeID;
    ctx->busInstance = this;

    IOUSBHostCompletion completion;
    completion.owner = this;
    completion.action = &KextBus_BulkCompletion;
    completion.parameter = ctx;

    __atomic_add_fetch(&mOutstanding, 1, __ATOMIC_SEQ_CST);
    IOReturn err = pe->pipe->io(md, size, &completion, 0);
    if (err != kIOReturnSuccess) {
        __atomic_sub_fetch(&mOutstanding, 1, __ATOMIC_SEQ_CST);
        if (slice) { slice->complete(dir); slice->release(); }
        delete ctx;
        return false;
    }
    return true;
}

/* Called with the engine lock held (from Start or an engine callback). */
bool KextBus::SubmitIsochPacket(uint8_t pipeID, void* buffer, OzzyIsochFrame* frames, uint32_t frameCount, void* contextID)
{
    PipeEntry* pe = FindPipe(pipeID);
    if (!pe || frameCount == 0 || frameCount > kMaxIsochTxns || pe->maxPacket == 0) return false;
    uint32_t uframes = frameCount * pe->intervalUframes;
    if (uframes % 8) {
        LogOzzyKext("pipe 0x%02x: %u intervals is not a whole number of frames", pipeID, frameCount);
        return false;
    }
    IOMemoryDescriptor* md = DescriptorFor(buffer, frameCount * pe->maxPacket, pipeID);
    if (!md) return false;

    IsochSlot* s = nullptr;
    for (uint32_t i = 0; i < kMaxIsochSlots && !s; i++)
        if (!mIsoch[i].used) s = &mIsoch[i];
    if (!s) {
        LogOzzyKext("no free isoch slot");
        return false;
    }

    uint32_t off = 0;
    for (uint32_t i = 0; i < frameCount; i++) {
        /* Keeps every packed offset inside the frameCount * maxPacket descriptor */
        if (frames[i].reqCount > pe->maxPacket) {
            LogOzzyKext("pipe 0x%02x: interval of %u bytes exceeds %u", pipeID, frames[i].reqCount, pe->maxPacket);
            return false;
        }
        s->txn[i] = IOUSBHostIsochronousTransaction();
        s->txn[i].requestCount = frames[i].reqCount;
        s->txn[i].offset = off;
        off += frames[i].reqCount;
    }
    if (!pe->running) {
        pe->nextFrame = mDevice->getFrameNumber() + kIsochLeadFrames;
        pe->running = true;
    }
    s->used = true;
    s->pipe = pipeID;
    s->ctx = contextID;
    s->frames = frames;
    s->count = frameCount;
    s->startFrame = pe->nextFrame;

    IOUSBHostIsochronousTransactionCompletion completion;
    completion.owner = this;
    completion.action = &KextBus_IsochCompletion;
    completion.parameter = s;

    __atomic_add_fetch(&mOutstanding, 1, __ATOMIC_SEQ_CST);
    IOReturn err = pe->pipe->io(md, s->txn, frameCount, s->startFrame, kIsochronousTransferOptionsNone, &completion);
    if (err != kIOReturnSuccess) {
        __atomic_sub_fetch(&mOutstanding, 1, __ATOMIC_SEQ_CST);
        s->used = false;
        pe->running = false;   /* the next submit starts a fresh run */
        LogOzzyKext("pipe 0x%02x isoch submit at frame %llu failed: 0x%x", pipeID, s->startFrame, err);
        return false;
    }
    pe->nextFrame += uframes / 8;
    return true;
}

bool KextBus::GetBusFrame(uint64_t* frame, uint64_t* hostTime)
{
    AbsoluteTime t = 0;
    uint64_t f = mDevice->getFrameNumber(&t);
    if (f == 0 || t == 0) return false;
    *frame = f;
    *hostTime = t;
    return true;
}

void KextBus::Post(const Completion& c)
{
    bool dropped = false;
    IOSimpleLockLock(mQueueLock);
    if (mQueueLen < kQueueLen) {
        mQueue[(mQueueHead + mQueueLen) % kQueueLen] = c;
        mQueueLen++;
    } else {
        dropped = true;
    }
    IOSimpleLockUnlock(mQueueLock);
    if (dropped) {
        __atomic_sub_fetch(&mOutstanding, 1, __ATOMIC_SEQ_CST);
        LogOzzyKext("completion queue full; dropping pipe 0x%02x", c.pipe);
    } else if (IORecursiveLockTryLock(mLock)) {
        /* Deliver now unless another thread holds the engine lock; that
         * thread delivers it in Unlock(). */
        mDepth++;
        Unlock();
    }
    /* The callback's last access to the bus */
    __atomic_sub_fetch(&mInCallbacks, 1, __ATOMIC_SEQ_CST);
}

void KextBus::EnterCallback()
{
    __atomic_add_fetch(&mInCallbacks, 1, __ATOMIC_SEQ_CST);
}

/* A callback enters before it queues and leaves after its delivery, so once
 * mOutstanding reads 0 any callback still running shows in mInCallbacks. */
bool KextBus::Busy()
{
    if (__atomic_load_n(&mOutstanding, __ATOMIC_SEQ_CST) != 0) return true;
    return __atomic_load_n(&mInCallbacks, __ATOMIC_SEQ_CST) != 0;
}

bool KextBus::PopQueued(Completion* c)
{
    bool got = false;
    IOSimpleLockLock(mQueueLock);
    if (mQueueLen) {
        *c = mQueue[mQueueHead];
        mQueueHead = (mQueueHead + 1) % kQueueLen;
        mQueueLen--;
        got = true;
    }
    IOSimpleLockUnlock(mQueueLock);
    return got;
}

void KextBus::Dispatch(const Completion& c)
{
    if (c.isoch) {
        IsochSlot* s = c.slot;
        uint8_t pipe = s->pipe;
        void* ctx = s->ctx;
        uint64_t start = s->startFrame;
        OzzyIsochFrame* frames = s->frames;
        uint32_t count = s->count;
        s->used = false;   /* free before the engine resubmits */
        if (mEngine) mEngine->OnIsochComplete(pipe, ctx, c.status, start, frames, count);
    } else if (mEngine) {
        mEngine->OnPacketComplete(c.pipe, c.ctx, c.status, c.bytes);
    }
    __atomic_sub_fetch(&mOutstanding, 1, __ATOMIC_SEQ_CST);
}

void KextBus::Lock()
{
    IORecursiveLockLock(mLock);
    mDepth++;
}

/* At depth 1, deliver everything queued before releasing, then re-check:
 * a completion may have queued after our last pop and failed its try-lock
 * while we still held the lock. Nested unlocks never deliver and return at
 * once (the outer holder's drain picks up anything they queued), so the
 * engine is never re-entered from inside one of its own calls. */
void KextBus::Unlock()
{
    for (;;) {
        bool outermost = mDepth == 1;
        if (outermost) {
            Completion c;
            while (PopQueued(&c)) Dispatch(c);
        }
        mDepth--;
        IORecursiveLockUnlock(mLock);
        if (!outermost) return;
        IOSimpleLockLock(mQueueLock);
        bool more = mQueueLen > 0;
        IOSimpleLockUnlock(mQueueLock);
        if (!more || !IORecursiveLockTryLock(mLock)) return;
        mDepth++;
    }
}

bool KextBus::AbortAndDrain(uint32_t timeoutMs)
{
    /* Returns once each aborted transfer's callback has run */
    for (uint32_t i = 0; i < mPipeCount; i++) {
        IOReturn err = mPipes[i].pipe->abort(IOUSBHostIOSource::kAbortSynchronous, kIOReturnAborted, nullptr);
        if (err != kIOReturnSuccess)
            LogOzzyKext("pipe 0x%02x abort failed: 0x%x", mPipes[i].address, err);
    }
    Lock();
    Unlock();   /* deliver anything queued while nobody held the lock */
    for (uint32_t t = 0; t < timeoutMs && Busy(); t++) {
        IOSleep(1);
        Lock();
        Unlock();
    }
    for (uint32_t i = 0; i < mPipeCount; i++)
        mPipes[i].running = false;
    if (Busy()) {
        LogOzzyKext("%d transfers, %d callbacks still outstanding after %u ms",
                    (int)__atomic_load_n(&mOutstanding, __ATOMIC_SEQ_CST),
                    (int)__atomic_load_n(&mInCallbacks, __ATOMIC_SEQ_CST), timeoutMs);
        return false;
    }
    return true;
}

bool KextBus::VendorRequest(uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void* data, uint16_t len)
{
    if (!mDevice || !mDriver) return false;
    StandardUSB::DeviceRequest dr;
    dr.bmRequestType = type;
    dr.bRequest = req;
    dr.wValue = val;
    dr.wIndex = idx;
    dr.wLength = len;
    uint32_t bytesTransferred = 0;
    // Use Raw Pointer (Fixed 0 Samplerate issue)
    IOReturn err = mDevice->deviceRequest(mDriver, dr, data, bytesTransferred, 5000);
    return (err == kIOReturnSuccess);
}

void KextBus::Sleep(uint32_t ms) { IOSleep(ms); }
uint64_t KextBus::GetTime() { return mach_absolute_time(); }

void KextBus::Log(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    IOLogv(fmt, args);
    va_end(args);
}
