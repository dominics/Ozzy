/* SPDX-License-Identifier: MIT */
#pragma once
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <atomic>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include "SharedMemory.h"

/*
 * One attached mixer. Owns the shared memory segment for as long as the
 * device is present and runs the engine on one time-constraint thread,
 * restarting it (same segment, new sessionID) when the engine asks.
 */
class Session {
public:
	/* onFinished (may be null) runs on the main queue after the USB thread
	 * gives up; it must not assume this Session still exists. */
	Session(io_service_t device, bool verbose, void (*onFinished)() = nullptr);
	~Session();

	/* Create the segment and start the USB thread. */
	bool Start();

	/* Stop the USB thread and destroy the segment. Safe to call twice. Main
	 * thread only. Blocks until an in-progress engine Start() finishes its
	 * control requests (up to their 5 s timeouts each). */
	void Stop();

	/* Is s the device this session drives? */
	bool Is(io_service_t s) const { return IOObjectIsEqualTo(mDevice, s); }

	/* The USB thread gave up (device gone or repeated init failure). */
	bool Finished() const { return mFinished; }

private:
	static void* ThreadEntry(void* self);
	void ThreadMain();
	bool RunOnce(uint32_t rate, uint32_t* nextRate);
	static void FinishedTrampoline(void* ctx);
	static void OnTimer(CFRunLoopTimerRef timer, void* info);

	io_service_t mDevice;
	bool mVerbose;
	void (*mOnFinished)();
	bool mWedged = false; /* an abort leaked transfers: do not retry */
	SharedMemorySegment mShm;
	/* Ticks the heartbeat on the main queue, independent of the USB thread. */
	dispatch_source_t mHeartbeat = nullptr;
	pthread_t mThread {};
	bool mThreadStarted = false;
	std::atomic<bool> mStopRequested { false };
	std::atomic<bool> mFinished { false };
	std::atomic<CFRunLoopRef> mRunLoop { nullptr };
	class DynacordEngine* mEngine = nullptr;
	bool mRestart = false;
	uint32_t mTicks = 0;
};
