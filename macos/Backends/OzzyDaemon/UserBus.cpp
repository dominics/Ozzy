/* SPDX-License-Identifier: MIT */
#include "UserBus.h"
#include <IOKit/IOCFPlugIn.h>
#include <mach/mach_time.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../../Shared/OzzyLog.h"
#include "../OzzyKext/KextStatus.h"

template <typename T>
static T** QueryPlugIn(io_service_t service, CFUUIDRef type, CFUUIDRef iid)
{
	IOCFPlugInInterface** plug = nullptr;
	SInt32 score = 0;
	if (IOCreatePlugInInterfaceForService(service, type, kIOCFPlugInInterfaceID, &plug, &score) != kIOReturnSuccess || !plug)
		return nullptr;
	T** out = nullptr;
	(*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(iid), (LPVOID*)&out);
	(*plug)->Release(plug);
	return out;
}

UserBus* UserBus::Open(io_service_t device, const OzzyDeviceProfile& profile, bool verbose, char* err, size_t errlen)
{
	UserBus* bus = new UserBus();
	bus->mVerbose = verbose;
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	bus->mStallTicks = 10ull * 1000 * 1000 * tb.denom / tb.numer;
	bus->mPool.resize(kPoolSize);
	for (Request& r : bus->mPool)
		bus->mFree.push_back(&r);

	bus->mDevice = QueryPlugIn<IOUSBDeviceInterface300>(device, kIOUSBDeviceUserClientTypeID, kIOUSBDeviceInterfaceID300);
	if (!bus->mDevice) {
		snprintf(err, errlen, "no device interface");
		delete bus;
		return nullptr;
	}
	IOReturn skr = (*bus->mDevice)->GetDeviceSpeed(bus->mDevice, &bus->mSpeed);
	if (skr != kIOReturnSuccess) {
		snprintf(err, errlen, "device speed: 0x%x", skr);
		delete bus;
		return nullptr;
	}

	/* With no kernel driver claiming it, macOS leaves a vendor-class device
	 * unconfigured, so it has no interfaces until we select configuration 1. */
	IOReturn dkr = (*bus->mDevice)->USBDeviceOpen(bus->mDevice);
	if (dkr != kIOReturnSuccess) {
		snprintf(err, errlen, "open device: 0x%x%s", dkr,
			 dkr == kIOReturnExclusiveAccess ? " (in use by another process or session)" : "");
		delete bus;
		return nullptr;
	}
	bus->mDeviceOpen = true;
	UInt8 config = 0;
	/* A failed GetConfiguration leaves config at 0, which degrades safely:
	 * configuration 1 is then set, and setting it again is idempotent. */
	(*bus->mDevice)->GetConfiguration(bus->mDevice, &config);
	if (config == 0) {
		IOUSBConfigurationDescriptorPtr cd = nullptr;
		dkr = (*bus->mDevice)->GetConfigurationDescriptorPtr(bus->mDevice, 0, &cd);
		if (dkr == kIOReturnSuccess)
			dkr = (*bus->mDevice)->SetConfiguration(bus->mDevice, cd->bConfigurationValue);
		if (dkr != kIOReturnSuccess) {
			snprintf(err, errlen, "set configuration: 0x%x", dkr);
			delete bus;
			return nullptr;
		}
	}

	IOUSBFindInterfaceRequest req = { kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare,
					  kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare };
	io_iterator_t it = IO_OBJECT_NULL;
	if ((*bus->mDevice)->CreateInterfaceIterator(bus->mDevice, &req, &it) != kIOReturnSuccess) {
		snprintf(err, errlen, "cannot list interfaces");
		delete bus;
		return nullptr;
	}
	io_service_t s;
	while ((s = IOIteratorNext(it)) != IO_OBJECT_NULL) {
		auto** intf = QueryPlugIn<IOUSBInterfaceInterface300>(s, kIOUSBInterfaceUserClientTypeID, kIOUSBInterfaceInterfaceID300);
		IOObjectRelease(s);
		if (!intf)
			continue;
		UInt8 num = 0;
		(*intf)->GetInterfaceNumber(intf, &num);
		int want = -1;
		for (int i = 0; i < profile.interfaceCount; i++)
			if (profile.interfaceIndices[i] == num)
				want = i;
		if (want < 0) {
			(*intf)->Release(intf);
			continue;
		}
		IOReturn kr = (*intf)->USBInterfaceOpen(intf);
		if (kr != kIOReturnSuccess) {
			snprintf(err, errlen, "open interface %u: 0x%x%s", num, kr,
				 kr == kIOReturnExclusiveAccess ? " (in use by another process or session)" : "");
			(*intf)->Release(intf);
			IOObjectRelease(it);
			delete bus;
			return nullptr;
		}
		kr = (*intf)->SetAlternateInterface(intf, profile.altSettings[want]);
		if (kr != kIOReturnSuccess) {
			snprintf(err, errlen, "interface %u alt %u: 0x%x", num, profile.altSettings[want], kr);
			(*intf)->USBInterfaceClose(intf);
			(*intf)->Release(intf);
			IOObjectRelease(it);
			delete bus;
			return nullptr;
		}
		Interface entry { intf, num, nullptr };
		kr = (*intf)->CreateInterfaceAsyncEventSource(intf, &entry.source);
		if (kr != kIOReturnSuccess) {
			snprintf(err, errlen, "interface %u event source: 0x%x", num, kr);
			(*intf)->USBInterfaceClose(intf);
			(*intf)->Release(intf);
			IOObjectRelease(it);
			delete bus;
			return nullptr;
		}
		bus->mInterfaces.push_back(entry);
		UInt8 n = 0;
		(*intf)->GetNumEndpoints(intf, &n);
		for (UInt8 ref = 1; ref <= n; ref++) {
			UInt8 dir, number, type, interval;
			UInt16 mps;
			if ((*intf)->GetPipeProperties(intf, ref, &dir, &number, &type, &mps, &interval) != kIOReturnSuccess)
				continue;
			Pipe p { (uint8_t)(number | (dir == kUSBIn ? 0x80 : 0)), bus->mInterfaces.size() - 1, ref, type,
				 interval, 0, 0, 0 };
			bus->mPipes.push_back(p);
			if (verbose)
				fprintf(stderr, "pipe %02x: intf %u ref %u type %u mps %u interval %u\n", p.address, num, ref, type, mps, interval);
		}
	}
	IOObjectRelease(it);
	if (bus->mInterfaces.size() != (size_t)profile.interfaceCount) {
		snprintf(err, errlen, "found %zu of %u interfaces", bus->mInterfaces.size(), profile.interfaceCount);
		delete bus;
		return nullptr;
	}
	return bus;
}

UserBus::~UserBus()
{
	for (Interface& i : mInterfaces) {
		if (i.source)
			CFRelease(i.source);
		(*i.intf)->USBInterfaceClose(i.intf);
		(*i.intf)->Release(i.intf);
	}
	if (mDeviceOpen)
		(*mDevice)->USBDeviceClose(mDevice);
	if (mDevice)
		(*mDevice)->Release(mDevice);
}

void UserBus::AttachToRunLoop(CFRunLoopRef rl)
{
	for (Interface& i : mInterfaces)
		if (i.source)
			CFRunLoopAddSource(rl, i.source, kCFRunLoopDefaultMode);
}

void UserBus::DetachFromRunLoop(CFRunLoopRef rl)
{
	for (Interface& i : mInterfaces)
		if (i.source)
			CFRunLoopRemoveSource(rl, i.source, kCFRunLoopDefaultMode);
}

void UserBus::AbortAll()
{
	for (Pipe& p : mPipes) {
		(*mInterfaces[p.intf].intf)->AbortPipe(mInterfaces[p.intf].intf, p.ref);
		p.nextFrame = 0;
	}
}

UserBus::Pipe* UserBus::FindPipe(uint8_t address)
{
	for (Pipe& p : mPipes)
		if (p.address == address)
			return &p;
	return nullptr;
}

UserBus::Request* UserBus::Acquire()
{
	if (mFree.empty())
		return nullptr;
	Request* r = mFree.back();
	mFree.pop_back();
	return r;
}

void UserBus::Release(Request* r) { mFree.push_back(r); }

/* Bus frames (1 ms) one submission spans. Each frame list entry is one
 * service interval; at high speed bInterval n means 2^(n-1) microframes.
 * On the CMS 600-3 (2026-10-09): 0x02 has bInterval 1, so 24 entries span
 * 3 ms; 0x81 has bInterval 4, so 5 entries span 5 ms. */
uint64_t UserBus::FramesPerSubmission(const Pipe& p, uint32_t frameCount) const
{
	uint32_t interval = p.interval ? p.interval : 1;
	if (mSpeed == kUSBDeviceSpeedHigh)
		return ((uint64_t)frameCount * (1u << (interval - 1)) + 7) / 8;
	return (uint64_t)frameCount * interval;
}

bool UserBus::SubmitIsochPacket(uint8_t pipeID, void* buffer, OzzyIsochFrame* frames, uint32_t frameCount, void* ctx)
{
	Pipe* p = FindPipe(pipeID);
	if (!p) {
		Log("iso submit: no pipe %02x", pipeID);
		return false;
	}
	if (frameCount > kMaxIsochFrames) {
		Log("iso submit on %02x: %u frames exceeds %u", pipeID, frameCount, (unsigned)kMaxIsochFrames);
		return false;
	}
	Request* r = Acquire();
	if (!r) {
		Log("iso submit on %02x: request pool exhausted", pipeID);
		return false;
	}
	auto** intf = mInterfaces[p->intf].intf;
	if (p->nextFrame == 0) {
		UInt64 cur = 0;
		AbsoluteTime at;
		if ((*intf)->GetBusFrameNumber(intf, &cur, &at) != kIOReturnSuccess) {
			Release(r);
			return false;
		}
		p->nextFrame = cur + kIsochLeadFrames;
	}
	*r = Request { this, p, ctx, frames, frameCount, p->nextFrame, {} };
	for (uint32_t i = 0; i < frameCount; i++)
		r->io[i] = IOUSBIsocFrame { kIOReturnSuccess, frames[i].reqCount, 0 };
	IOReturn kr = (p->address & 0x80)
		? (*intf)->ReadIsochPipeAsync(intf, p->ref, buffer, r->startFrame, frameCount, r->io, IsochDone, r)
		: (*intf)->WriteIsochPipeAsync(intf, p->ref, buffer, r->startFrame, frameCount, r->io, IsochDone, r);
	if (kr != kIOReturnSuccess) {
		uint64_t start = r->startFrame;
		Release(r);
		p->nextFrame = 0;
		UInt64 cur = 0;
		AbsoluteTime at;
		(*intf)->GetBusFrameNumber(intf, &cur, &at);
		Log("iso submit on %02x failed: 0x%x (start frame %llu, bus frame %llu)", pipeID, kr,
		    (unsigned long long)start, (unsigned long long)cur);
		return false;
	}
	p->nextFrame += FramesPerSubmission(*p, frameCount);
	return true;
}

void UserBus::IsochDone(void* refcon, IOReturn result, void*)
{
	Request* r = (Request*)refcon;
	UserBus* bus = r->bus;
	for (uint32_t i = 0; i < r->frameCount; i++)
		r->frames[i] = OzzyIsochFrame { r->io[i].frReqCount, r->io[i].frActCount,
						KextTransferStatus(r->pipe->address, r->io[i].frStatus) };
	Pipe* p = r->pipe;
	int status = KextTransferStatus(p->address, result);
	void* ctx = r->ctx;
	OzzyIsochFrame* frames = r->frames;
	uint32_t n = r->frameCount;
	uint64_t start = r->startFrame;
	bus->Release(r);
	uint64_t now = mach_absolute_time();
	if (bus->mVerbose && p->logged < 40) {
		p->logged++;
		fprintf(stderr, "iso %02x done: start %llu frames %u result 0x%x\n", p->address, (unsigned long long)start, n, status);
	}
	if (status != 0)
		p->nextFrame = 0;
	if (bus->mEngine)
		bus->mEngine->OnIsochComplete(p->address, ctx, status, start, frames, n);
	/* A completion arriving long after the previous one often means this
	 * thread was not scheduled (system sleep has also caused it), which is
	 * one way isochronous requests land too late. Checked after the engine
	 * callback so the late completion is resubmitted first, and skipped for
	 * aborted completions, which are the deliberate stop/restart drain. */
	if (status != (int)kIOReturnAborted && p->lastDone && now - p->lastDone > bus->mStallTicks)
		bus->Log("iso %02x: %llu us since the previous completion", p->address,
			 (unsigned long long)((now - p->lastDone) * 1000 / bus->mStallTicks * 10));
	p->lastDone = now;
}

bool UserBus::SubmitUSBPacket(uint8_t pipeID, void* buffer, uint32_t size, void* ctx)
{
	Pipe* p = FindPipe(pipeID);
	if (!p) {
		Log("submit: no pipe %02x", pipeID);
		return false;
	}
	Request* r = Acquire();
	if (!r) {
		Log("submit on %02x: request pool exhausted", pipeID);
		return false;
	}
	*r = Request { this, p, ctx, nullptr, 0, 0, {} };
	auto** intf = mInterfaces[p->intf].intf;
	IOReturn kr = (p->address & 0x80) ? (*intf)->ReadPipeAsync(intf, p->ref, buffer, size, BulkDone, r)
					  : (*intf)->WritePipeAsync(intf, p->ref, buffer, size, BulkDone, r);
	if (kr != kIOReturnSuccess) {
		Release(r);
		return false;
	}
	return true;
}

void UserBus::BulkDone(void* refcon, IOReturn result, void* arg0)
{
	Request* r = (Request*)refcon;
	UserBus* bus = r->bus;
	Pipe* p = r->pipe;
	uint8_t address = p->address;
	void* ctx = r->ctx;
	bus->Release(r);
	int status = KextTransferStatus(address, result);
	if (bus->mVerbose && p->logged < 40) {
		p->logged++;
		fprintf(stderr, "bulk %02x done: bytes %u result 0x%x\n", address, (uint32_t)(uintptr_t)arg0, status);
	}
	if (bus->mEngine)
		bus->mEngine->OnPacketComplete(address, ctx, status, (uint32_t)(uintptr_t)arg0);
}

bool UserBus::VendorRequest(uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void* data, uint16_t len)
{
	if (mInterfaces.empty())
		return false;
	auto** intf = mInterfaces[0].intf;
	IOUSBDevRequestTO r = {};
	r.bmRequestType = type;
	r.bRequest = req;
	r.wValue = val;
	r.wIndex = idx;
	r.wLength = len;
	r.pData = data;
	r.noDataTimeout = 1000;
	r.completionTimeout = 5000;
	IOReturn kr = (*intf)->ControlRequestTO(intf, 0, &r);
	if (kr != kIOReturnSuccess)
		Log("control %02x %02x %04x %04x failed: 0x%x", type, req, val, idx, kr);
	return kr == kIOReturnSuccess;
}

bool UserBus::GetBusFrame(uint64_t* frame, uint64_t* hostTime)
{
	if (mInterfaces.empty())
		return false;
	auto** intf = mInterfaces[0].intf;
	UInt64 f = 0;
	AbsoluteTime at;
	if ((*intf)->GetBusFrameNumber(intf, &f, &at) != kIOReturnSuccess)
		return false;
	uint64_t t;
	memcpy(&t, &at, sizeof(t));
	*frame = f;
	*hostTime = t;
	return true;
}

void UserBus::Sleep(uint32_t ms) { usleep(ms * 1000); }
uint64_t UserBus::GetTime() { return mach_absolute_time(); }

void UserBus::Log(const char* fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	LogOzzyDaemon("%{public}s", buf);
	if (mVerbose)
		fprintf(stderr, "%s\n", buf);
}
