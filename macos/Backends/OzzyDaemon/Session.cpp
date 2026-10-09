/* SPDX-License-Identifier: MIT */
#include "Session.h"
#include <stdio.h>
#include <unistd.h>
#include "../../Devices/Dynacord/DynacordEngine.h"
#include "../../OzzyCore/OzzyFactory.h"
#include "../../Shared/OzzyLog.h"
#include "RealTime.h"
#include "UserBus.h"

static void ReadString(io_service_t s, CFStringRef key, char* out, size_t n, const char* fallback)
{
	CFTypeRef v = IORegistryEntryCreateCFProperty(s, key, kCFAllocatorDefault, 0);
	if (!v || CFGetTypeID(v) != CFStringGetTypeID() || !CFStringGetCString((CFStringRef)v, out, (CFIndex)n, kCFStringEncodingUTF8))
		strlcpy(out, fallback, n);
	if (v)
		CFRelease(v);
}

Session::Session(io_service_t device, bool verbose, void (*onFinished)())
	: mDevice(device), mVerbose(verbose), mOnFinished(onFinished)
{
	IOObjectRetain(mDevice);
}

Session::~Session()
{
	Stop();
	IOObjectRelease(mDevice);
}

bool Session::Start()
{
	/* Create() clears the umask: it must stay on the main thread with no Session thread running. */
	switch (mShm.Create()) {
	case SharedMemorySegment::CreateResult::kCreated:
		break;
	case SharedMemorySegment::CreateResult::kReplacedStale:
		LogOzzyDaemon("replaced a stale segment left by a previous daemon");
		break;
	case SharedMemorySegment::CreateResult::kInUse:
		LogOzzyDaemonError("another OzzyDaemon is running; not starting");
		return false;
	case SharedMemorySegment::CreateResult::kError:
		LogOzzyDaemonError("could not create the shared memory segment");
		return false;
	}
	mShm.TickHeartbeat();
	mHeartbeat = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
	dispatch_source_set_timer(mHeartbeat, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC),
				  100 * NSEC_PER_MSEC, 10 * NSEC_PER_MSEC);
	dispatch_source_set_event_handler(mHeartbeat, ^{
		if (!mFinished)
			mShm.TickHeartbeat();
	});
	dispatch_resume(mHeartbeat);
	char manufacturer[64], product[64], serial[64];
	ReadString(mDevice, CFSTR("USB Vendor Name"), manufacturer, sizeof(manufacturer), "DYNACORD");
	ReadString(mDevice, CFSTR("USB Product Name"), product, sizeof(product), "CMS 600-3");
	ReadString(mDevice, CFSTR("USB Serial Number"), serial, sizeof(serial), "CMS6003");
	mShm.PublishIdentity(DYNACORD_VENDOR_ID, DYNACORD_PID_CMS600_3, manufacturer, product, serial);
	mThreadStarted = pthread_create(&mThread, nullptr, ThreadEntry, this) == 0;
	return mThreadStarted;
}

/* Only ever called on the main thread (main.cpp's notification and signal
 * handlers), so the heartbeat handler, which runs on the main queue, can
 * never run concurrently with Destroy(). Blocks until an in-progress engine
 * Start() finishes its control requests (up to their 5 s timeouts each). */
void Session::Stop()
{
	mStopRequested = true;
	if (CFRunLoopRef rl = mRunLoop.load())
		CFRunLoopStop(rl);
	if (mThreadStarted) {
		pthread_join(mThread, nullptr);
		mThreadStarted = false;
	}
	if (CFRunLoopRef rl = mRunLoop.exchange(nullptr))
		CFRelease(rl);
	if (mHeartbeat) {
		dispatch_source_cancel(mHeartbeat);
		dispatch_release(mHeartbeat);
		mHeartbeat = nullptr;
	}
	mShm.Destroy();
}

void* Session::ThreadEntry(void* self)
{
	((Session*)self)->ThreadMain();
	return nullptr;
}

void Session::OnTimer(CFRunLoopTimerRef, void* info)
{
	Session* s = (Session*)info;
	if (s->mVerbose && s->mEngine && ++s->mTicks % 10 == 0) {
		/* How far output runs ahead of the mixer and input lags or leads it:
		 * the numbers the safety offsets must cover. */
		DynacordEngine* e = s->mEngine;
		fprintf(stderr, "lead: out %lld frames, in %lld frames, iso_err %llu, in_short %llu\n",
			(long long)(e->FramesSent() - e->FramesConsumed()),
			(long long)(e->FramesReceived() - e->FramesConsumed()),
			(unsigned long long)e->IsoErrors(), (unsigned long long)e->InShortReads());
	}
	if (s->mEngine && s->mEngine->Poll() == OzzyEngine::PollResult::kRestart) {
		s->mRestart = true;
		CFRunLoopStop(CFRunLoopGetCurrent());
	}
}

/* Run the engine once at rate until it asks for a restart or we are told to
 * stop. Returns false if it never got going (init or open failed). */
bool Session::RunOnce(uint32_t rate, uint32_t* nextRate)
{
	char err[256];
	DynacordEngine* engine = (DynacordEngine*)OzzyFactory::CreateEngine(DYNACORD_VENDOR_ID, DYNACORD_PID_CMS600_3);
	UserBus* bus = UserBus::Open(mDevice, engine->GetProfile(), mVerbose, err, sizeof(err));
	if (!bus) {
		LogOzzyDaemonError("open: %{public}s", err);
		delete engine;
		return false;
	}
	CFRunLoopRef rl = CFRunLoopGetCurrent();
	engine->SetRate(rate);
	engine->Init(bus, mShm.Get());
	bus->SetEngine(engine);
	bus->AttachToRunLoop(rl);
	mShm.NewSession();

	bool started = engine->Start();
	if (started) {
		LogOzzyDaemon("streaming at %u Hz", rate);
		mEngine = engine;
		mRestart = false;
		CFRunLoopTimerContext ctx = { 0, this, nullptr, nullptr, nullptr };
		CFRunLoopTimerRef timer = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 0.1, 0.1, 0, 0, OnTimer, &ctx);
		CFRunLoopAddTimer(rl, timer, kCFRunLoopDefaultMode);
		while (!mStopRequested && !mRestart)
			CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);
		CFRunLoopRemoveTimer(rl, timer, kCFRunLoopDefaultMode);
		CFRelease(timer);
		mEngine = nullptr;
		*nextRate = engine->NextRate();
	}
	engine->Stop();
	bus->AbortAll();
	/* Let aborted requests complete before their buffers go away. */
	for (int i = 0; i < 50 && bus->Outstanding() > 0; i++)
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
	bus->DetachFromRunLoop(rl);
	if (bus->Outstanding() > 0) {
		/* IOKit still owns transfers into engine memory: leak rather than free
		 * under it. The session ends (ThreadMain stops retrying and the
		 * daemon drops it); Stop() still Destroy()s the segment, which is safe
		 * because the kernel keeps the pages it writes to wired. */
		LogOzzyDaemonError("%zu transfers did not finish aborting", bus->Outstanding());
		mWedged = true;
	} else {
		delete bus;
		delete engine;
	}
	return started;
}

void Session::ThreadMain()
{
	char err[256];
	if (!SetRealTimePolicy(err, sizeof(err)))
		LogOzzyDaemonError("%{public}s; expect halts", err);
	/* Retained so Stop() can safely use it after this thread exits; released in Stop(). */
	mRunLoop = (CFRunLoopRef)CFRetain(CFRunLoopGetCurrent());
	uint32_t rate = 48000;
	int failures = 0;
	while (!mStopRequested && !mWedged && failures < 5) {
		uint32_t next = rate;
		if (RunOnce(rate, &next)) {
			failures = 0;
			rate = next;
		} else {
			failures++;
			usleep(500000);
		}
	}
	if (mWedged)
		LogOzzyDaemonError("USB transfers did not finish aborting; giving up until the mixer is replugged");
	else if (failures >= 5)
		LogOzzyDaemonError("giving up after 5 failed starts");
	mFinished = true;
	if (mOnFinished)
		dispatch_async_f(dispatch_get_main_queue(), (void*)mOnFinished, FinishedTrampoline);
}

void Session::FinishedTrampoline(void* ctx)
{
	if (ctx)
		((void (*)())ctx)();
}
