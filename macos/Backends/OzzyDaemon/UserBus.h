/* SPDX-License-Identifier: MIT */
#pragma once
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <stddef.h>
#include <vector>
#include "../../OzzyCore/OzzyEngine.h"

/*
 * IOzzyBus over IOUSBLib for one device. Opens the interfaces a profile
 * names, selects their alternate settings and maps endpoint addresses to
 * pipe refs. Completions are delivered on whichever run loop the bus is
 * attached to; everything must run on that one thread.
 */
class UserBus : public IOzzyBus {
public:
	/* Open the profile's interfaces on device. NULL with a message on failure
	 * (kIOReturnExclusiveAccess means another process holds the mixer). */
	static UserBus* Open(io_service_t device, const OzzyDeviceProfile& profile, bool verbose, char* err,
			     size_t errlen);
	~UserBus() override;

	void AttachToRunLoop(CFRunLoopRef rl);
	void DetachFromRunLoop(CFRunLoopRef rl);

	/* Abort every pipe; outstanding requests complete with kIOReturnAborted. */
	void AbortAll();

	/* Requests still owned by IOKit. Free the bus only once this is 0. */
	size_t Outstanding() const { return mPool.size() - mFree.size(); }

	bool SubmitUSBPacket(uint8_t pipeID, void* buffer, uint32_t size, void* contextID) override;
	bool SubmitIsochPacket(uint8_t pipeID, void* buffer, OzzyIsochFrame* frames, uint32_t frameCount,
			       void* contextID) override;
	bool VendorRequest(uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void* data, uint16_t len) override;
	bool GetBusFrame(uint64_t* frame, uint64_t* hostTime) override;
	void Sleep(uint32_t ms) override;
	uint64_t GetTime() override;
	void Log(const char* fmt, ...) override;
	void SetEngine(OzzyEngine* engine) override { mEngine = engine; }

private:
	enum : uint32_t { kMaxIsochFrames = 32, kPoolSize = 64, kIsochLeadFrames = 2 };
	struct Interface {
		IOUSBInterfaceInterface300** intf;
		uint8_t number;
		CFRunLoopSourceRef source;
	};
	struct Pipe {
		uint8_t address;    /* endpoint address including direction bit */
		size_t intf;        /* index into mInterfaces */
		uint8_t ref;        /* IOUSBLib pipe ref */
		uint8_t type;       /* kUSBIsoc, kUSBBulk, ... */
		uint8_t interval;   /* bInterval as reported */
		uint64_t nextFrame; /* next bus frame for an iso submission, 0 = reseed */
		uint32_t logged;    /* completions logged with --verbose */
		uint64_t lastDone;  /* mach_absolute_time() of the last completion */
	};
	struct Request {
		UserBus* bus;
		Pipe* pipe;
		void* ctx;
		OzzyIsochFrame* frames;
		uint32_t frameCount;
		uint64_t startFrame;
		IOUSBIsocFrame io[kMaxIsochFrames];
	};

	UserBus() = default;
	Pipe* FindPipe(uint8_t address);
	Request* Acquire();
	void Release(Request* r);
	uint64_t FramesPerSubmission(const Pipe& p, uint32_t frameCount) const;
	static void IsochDone(void* refcon, IOReturn result, void* arg0);
	static void BulkDone(void* refcon, IOReturn result, void* arg0);

	IOUSBDeviceInterface300** mDevice = nullptr;
	UInt8 mSpeed = 0;
	bool mDeviceOpen = false;
	uint64_t mStallTicks = 0; /* 10 ms in mach_absolute_time() units */
	std::vector<Interface> mInterfaces;
	std::vector<Pipe> mPipes;
	std::vector<Request> mPool;
	std::vector<Request*> mFree;
	OzzyEngine* mEngine = nullptr;
	bool mVerbose = false;
};
