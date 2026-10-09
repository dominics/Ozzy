/* SPDX-License-Identifier: MIT */
#pragma once
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <vector>
#include "../OzzyCore/OzzyEngine.h"

/*
 * An IOzzyBus that records everything the engine asks for. Control requests
 * go to an optional responder; iso and bulk submissions queue up until a test
 * completes them through the engine callbacks.
 */
class FakeBus : public IOzzyBus {
public:
	struct Control {
		uint8_t type, req;
		uint16_t val, idx, len;
		std::vector<uint8_t> out; /* data sent (host to device) */
	};
	struct Iso {
		uint8_t pipe;
		uint8_t* buffer;
		OzzyIsochFrame* frames;
		uint32_t frameCount;
		void* ctx;
		std::vector<uint16_t> reqCounts; /* snapshot at submit time */
	};
	struct Bulk {
		uint8_t pipe;
		uint8_t* buffer;
		uint32_t size;
		void* ctx;
	};

	/* Fills `data` for IN requests; return false to fail the request. */
	std::function<bool(const Control&, uint8_t* data)> responder;
	std::vector<Control> controls;
	std::deque<Iso> iso;
	std::deque<Bulk> bulk;
	uint64_t now = 1000000;          /* GetTime(), host ticks */
	bool busFrameSupported = true;
	uint64_t refFrame = 0, refHost = 1000000;
	bool failSubmits = false;
	OzzyEngine* engine = nullptr;

	bool SubmitUSBPacket(uint8_t pipeID, void* buffer, uint32_t size, void* contextID) override
	{
		if (failSubmits)
			return false;
		bulk.push_back({ pipeID, (uint8_t*)buffer, size, contextID });
		return true;
	}
	bool SubmitIsochPacket(uint8_t pipeID, void* buffer, OzzyIsochFrame* frames, uint32_t frameCount,
			       void* contextID) override
	{
		if (failSubmits)
			return false;
		Iso s { pipeID, (uint8_t*)buffer, frames, frameCount, contextID, {} };
		for (uint32_t i = 0; i < frameCount; i++)
			s.reqCounts.push_back(frames[i].reqCount);
		iso.push_back(s);
		return true;
	}
	bool VendorRequest(uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void* data, uint16_t len) override
	{
		Control c { type, req, val, idx, len, {} };
		if (!(type & 0x80) && data && len)
			c.out.assign((uint8_t*)data, (uint8_t*)data + len);
		controls.push_back(c);
		return responder ? responder(c, (uint8_t*)data) : true;
	}
	bool GetBusFrame(uint64_t* frame, uint64_t* hostTime) override
	{
		if (!busFrameSupported)
			return false;
		*frame = refFrame;
		*hostTime = refHost;
		return true;
	}
	void Sleep(uint32_t) override {}
	uint64_t GetTime() override { return now; }
	void Log(const char* fmt, ...) override
	{
		va_list ap;
		va_start(ap, fmt);
		std::vfprintf(stderr, fmt, ap);
		va_end(ap);
		std::fputc('\n', stderr);
	}
	void SetEngine(OzzyEngine* e) override { engine = e; }

	/* Remove and return the oldest pending iso submission on pipe. */
	Iso PopIso(uint8_t pipe)
	{
		for (auto it = iso.begin(); it != iso.end(); ++it) {
			if (it->pipe == pipe) {
				Iso s = *it;
				iso.erase(it);
				return s;
			}
		}
		std::fprintf(stderr, "FakeBus: no pending iso on pipe %02x\n", pipe);
		std::abort();
	}
	Bulk PopBulk(uint8_t pipe)
	{
		for (auto it = bulk.begin(); it != bulk.end(); ++it) {
			if (it->pipe == pipe) {
				Bulk b = *it;
				bulk.erase(it);
				return b;
			}
		}
		std::fprintf(stderr, "FakeBus: no pending bulk on pipe %02x\n", pipe);
		std::abort();
	}
};

/* A bus that only implements the original pure virtuals. */
class MinimalBus : public IOzzyBus {
public:
	bool SubmitUSBPacket(uint8_t, void*, uint32_t, void*) override { return true; }
	bool VendorRequest(uint8_t, uint8_t, uint16_t, uint16_t, void*, uint16_t) override { return true; }
	void Sleep(uint32_t) override {}
	uint64_t GetTime() override { return 0; }
	void Log(const char*, ...) override {}
	void SetEngine(OzzyEngine*) override {}
};
