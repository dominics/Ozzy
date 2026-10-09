/* SPDX-License-Identifier: MIT */
#pragma once
#include "../../OzzyCore/OzzyEngine.h"
#include "../../Shared/OzzySharedData.h"
#include "../../../common/devices/ploytec/dynacord_pacer.h"
#include "../../../common/devices/ploytec/ploytec_defs.h"
#include "DynacordRing.h"

/*
 * Dynacord CMS 600-3 (old Ploytec card, 0562:03eb): iso S24_3LE out on 0x02
 * paced by explicit feedback on 0x81, bulk 64-byte Ploytec frames in on
 * 0x86. Built into the kext (KextBus) and the userspace daemon (UserBus).
 */
class DynacordEngine : public OzzyEngine {
public:
	enum : uint32_t { kMaxOutXfers = 32 };
	enum : uint32_t {
		kOutPktsPerXfer = 24,   /* one packet per 125 us microframe */
		kOutXferBytes = kOutPktsPerXfer * DYNACORD_OUT_MAX_FRAMES_PER_PKT * DYNACORD_OUT_FRAME_SIZE,
		kFbXfers = 8,
		kFbPktsPerXfer = 5,     /* one 3-byte reading per ms */
		kFbPktSize = 64,        /* wMaxPacketSize of 0x81 */
		kInXfers = 8,
	};

	/* outXfers: iso OUT transfers of 3 ms kept queued, clamped to 1..kMaxOutXfers. */
	explicit DynacordEngine(uint32_t outXfers);
	uint32_t OutXfers() const { return mOutXfers; }

	/* Rate for the next Start(): 44100, 48000 or 96000. */
	void SetRate(uint32_t rate) override
	{
		mRate = rate;
		mNextRate = rate;
	}
	uint32_t Rate() const { return mRate; }
	static bool IsSupportedRate(uint32_t rate) { return rate == 44100 || rate == 48000 || rate == 96000; }

	uint32_t LayoutVersion() const override { return kOzzyLayoutFrames; }
	const OzzyDeviceProfile& GetProfile() const override { return mProfile; }
	void SetBulk(bool) override {}

	/* Run the rate init sequence, publish the ring and timing config, then
	 * start streaming. False if the device didn't accept the rate. */
	bool Start() override;

	/* Stop resubmitting; completions after this are ignored. */
	void Stop() override;

	/*
	 * Called every 100 ms by the host with the engine lock held. Offers
	 * the bus frame reference a new sample and decides whether the stream
	 * must be restarted: after a transfer failure, 1 s without valid
	 * feedback (the firmware halts for good when its OUT buffer runs dry;
	 * re-running init recovers it), or a supported rate requested through
	 * the segment.
	 */
	PollResult Poll() override;

	/* Rate to Start() at after a kRestart. */
	uint32_t NextRate() const override { return mNextRate; }

	void OnPacketComplete(uint8_t pipeID, void* ctx, int status, uint32_t bytes) override;
	void OnIsochComplete(uint8_t pipeID, void* ctx, int status, uint64_t startFrame, OzzyIsochFrame* frames,
			     uint32_t frameCount) override;

	uint64_t FramesSent() const { return mSent; }
	uint64_t FramesConsumed() const { return mConsumed; }
	uint64_t FramesReceived() const { return mReceived; }
	uint64_t InShortReads() const { return mInShort; }
	uint64_t IsoErrors() const { return mIsoErrors; }
	bool Failed() const { return mFailed; }

private:
	bool RunInit();
	bool Control(uint8_t type, uint8_t req, uint16_t val, uint16_t idx, uint8_t* data, uint16_t len, const char* what);
	void PublishConfig();
	bool StartStreaming();
	void FillOut(uint32_t i);
	bool SubmitOut(uint32_t i);
	bool SubmitFeedback(uint32_t i);
	bool SubmitIn(uint64_t seq);
	void Consume(int frames, uint64_t busFrame);
	void OfferBusReference(uint64_t frame, uint64_t host);
	uint64_t HostTimeOfBusFrame(uint64_t frame);
	void WriteTimestamp(uint64_t sampleTime, uint64_t hostTime);
	void PublishAnchor(uint64_t sampleTime, uint64_t hostTime);
	volatile OzzySharedMemory* SHM() { return (volatile OzzySharedMemory*)mSharedMemory; }

	enum : uint32_t { kSafetyInMs = 25 };

	OzzyDeviceProfile mProfile;
	uint32_t mOutXfers;
	uint32_t mRate = 48000;
	uint32_t mNextRate = 48000;
	DynacordRingLayout mLayout {};
	bool mStopping = false;

	struct pacer mPacer;
	uint8_t mOutBuf[kMaxOutXfers][kOutXferBytes];
	OzzyIsochFrame mOutFrames[kMaxOutXfers][kOutPktsPerXfer];
	uint8_t mFbBuf[kFbXfers][kFbPktsPerXfer * kFbPktSize];
	OzzyIsochFrame mFbFrames[kFbXfers][kFbPktsPerXfer];

	uint64_t mSent = 0;          /* frames handed to iso OUT */
	uint64_t mConsumed = 0;      /* frames the mixer reported consuming */
	uint64_t mReceived = 0;      /* input frames delivered (whole transfers) */
	uint64_t mNextAnchor = 0;    /* next sample time to publish */
	uint32_t mPeriod = 0;        /* frames between anchors, as published */
	uint64_t mInShort = 0, mIsoErrors = 0;
	uint64_t mTicksPerMs = 0;
	uint64_t mRefFrame = 0, mRefHost = 0;
	bool mHaveRef = false;
	uint64_t mLastFbHost = 0;    /* GetTime() of the last valid feedback */
	bool mFailed = false;
};
