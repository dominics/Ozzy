/* SPDX-License-Identifier: MIT */
#include "DynacordEngine.h"
#include <string.h>
#ifdef KERNEL
#include <kern/clock.h>
#else
#include <mach/mach_time.h>
#endif
#include "../../../common/devices/ploytec/dynacord_feedback.h"
#include "../../Shared/OzzyLog.h"
#include "../../../common/devices/ploytec/ploytec_protocol.h"

DynacordEngine::DynacordEngine(uint32_t outXfers)
	: mOutXfers(outXfers < 1 ? 1 : outXfers > kMaxOutXfers ? kMaxOutXfers : outXfers)
{
	mProfile.interfaceCount = 2;
	mProfile.interfaceIndices[0] = 0;
	mProfile.interfaceIndices[1] = 1;
	mProfile.altSettings[0] = 1;
	mProfile.altSettings[1] = 1;
	mProfile.isoOut = { DYNACORD_EP_PCM_OUT, 0 };
	mProfile.feedbackIn = { DYNACORD_EP_FEEDBACK, 1 };
	mProfile.pcmIn = { DYNACORD_EP_PCM_IN, 1 };
	mProfile.midiOut = { DYNACORD_EP_MIDI_OUT, 0 };
	mProfile.midiIn = { DYNACORD_EP_MIDI_IN, 0 };
}

bool DynacordEngine::Control(uint8_t type, uint8_t req, uint16_t val, uint16_t idx, uint8_t* data, uint16_t len,
			     const char* what)
{
	if (mBus->VendorRequest(type, req, val, idx, data, len))
		return true;
	LogDynacordError("%{public}s (%02x %02x %04x %04x) failed", what, type, req, val, idx);
	return false;
}

/* The Ploytec rate sequence as the Windows driver sends it to the CMS
 * (FINDINGS.md "Control sequence"; same as ploytec-play's init.c). */
bool DynacordEngine::RunInit()
{
	uint8_t buf[16] = {}, rbuf[3], status = 0;

	if (!Control(0xC0, PLOYTEC_CMD_FIRMWARE, 0, 0, buf, 15, "firmware"))
		return false;
	struct ploytec_firmware_version fw = ploytec_parse_firmware(buf);
	LogDynacord("firmware: chip %02x, version %u.%u", fw.chip_id, fw.major, fw.minor);

	if (!Control(0xC0, PLOYTEC_CMD_STATUS, 0, 0, &status, 1, "status"))
		return false;
	if (!Control(PLOYTEC_CMD_GET_RATE_TYPE, PLOYTEC_CMD_GET_RATE_REQ, 0x0100, 0, rbuf, 3, "get rate"))
		return false;
	LogDynacord("current rate: %u Hz", ploytec_decode_rate(rbuf));

	ploytec_encode_rate(mRate, rbuf);
	for (int i = 0; i < 5; i++) {
		uint16_t ep = i % 2 == 0 ? PLOYTEC_EP_RATE_IN : DYNACORD_EP_RATE_OUT;
		if (!Control(PLOYTEC_CMD_SET_RATE_TYPE, PLOYTEC_CMD_SET_RATE_REQ, 0x0100, ep, rbuf, 3, "set rate"))
			return false;
		mBus->Sleep(10);
	}

	if (!Control(PLOYTEC_CMD_GET_RATE_TYPE, PLOYTEC_CMD_GET_RATE_REQ, 0x0100, PLOYTEC_EP_RATE_IN, rbuf, 3,
		     "verify rate"))
		return false;
	if (ploytec_decode_rate(rbuf) != mRate) {
		LogDynacordError("device reports %u Hz after setting %u Hz", ploytec_decode_rate(rbuf), mRate);
		return false;
	}

	if (!Control(0xC0, PLOYTEC_CMD_STATUS, 0, 0, &status, 1, "status"))
		return false;
	return Control(0x40, PLOYTEC_CMD_STATUS, ploytec_confirm_wvalue(status), 0, nullptr, 0, "confirm");
}

/* Ring and timing description the HAL reads from a v2 segment. Output
 * safety: the OUT queue (3 ms per transfer) + one transfer + 13 ms for HAL
 * IO-thread jitter. Input: one 3 ms transfer + 4.5 ms worst completion gap,
 * with room for the not-yet-measured input lead: 25 ms. Both are starting
 * values; Task 10's probe measures the real leads. */
void DynacordEngine::PublishConfig()
{
	volatile OzzyAudioBuffer& a = SHM()->audio;
	mLayout = DynacordRingLayoutFor(mRate);
	a.outputBytesPerFrame = kDynacordOutFrameBytes;
	a.inputBytesPerFrame = kDynacordInFrameBytes;
	a.framesPerPacket = mLayout.inFramesPerXfer;
	a.deviceFlags = 0;
	a.ringFramesOut = mLayout.outFrames;
	a.ringFramesIn = mLayout.inFrames;
	mPeriod = mRate / 10;
	a.zeroTimestampPeriod = mPeriod;
	a.safetyOffsetOut = mRate * (mOutXfers * 3 + 16) / 1000;
	a.safetyOffsetIn = mRate * kSafetyInMs / 1000;
	a.sampleRate = mRate;
	a.requestedRate = 0;
}

bool DynacordEngine::Start()
{
	if (!mBus || !mSharedMemory || !IsSupportedRate(mRate))
		return false;
	mStopping = false;
	if (!RunInit())
		return false;
	PublishConfig();
	SHM()->audio.hardwarePresent = true;
	if (!StartStreaming()) {
		LogDynacordError("could not queue streaming transfers");
		mFailed = true;
		return false;
	}
	return true;
}

void DynacordEngine::Stop()
{
	mStopping = true;
}

static uint64_t HostTicksPerMs()
{
#ifdef KERNEL
	uint64_t t;
	nanoseconds_to_absolutetime(1000000ull, &t);
	return t;
#else
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	return 1000000ull * tb.denom / tb.numer;
#endif
}

/* Feedback within 2 frames of nominal; the pacer applies the same rule. */
static bool ValidFeedback(int v, uint32_t rate)
{
	int d = v - (int)(rate / 1000);
	return v > 0 && d >= -2 && d <= 2;
}

/* Fill OUT transfer i: one pacer decision per microframe, frames taken from
 * the HAL's output ring (which zeroes them behind). */
void DynacordEngine::FillOut(uint32_t i)
{
	volatile OzzyAudioBuffer& a = SHM()->audio;
	uint32_t off = 0;
	for (uint32_t p = 0; p < kOutPktsPerXfer; p++) {
		uint32_t n = (uint32_t)pacer_next(&mPacer);
		DynacordTakeOutput((uint8_t*)a.outputBuffer, mLayout.outFrames, mSent, mOutBuf[i] + off, n);
		mSent += n;
		mOutFrames[i][p] = { (uint16_t)(n * DYNACORD_OUT_FRAME_SIZE), 0, 0 };
		off += n * DYNACORD_OUT_FRAME_SIZE;
	}
}

bool DynacordEngine::SubmitOut(uint32_t i)
{
	return mBus->SubmitIsochPacket(DYNACORD_EP_PCM_OUT, mOutBuf[i], mOutFrames[i], kOutPktsPerXfer,
				       (void*)(uintptr_t)i);
}

bool DynacordEngine::SubmitFeedback(uint32_t i)
{
	for (uint32_t p = 0; p < kFbPktsPerXfer; p++)
		mFbFrames[i][p] = { kFbPktSize, 0, 0 };
	return mBus->SubmitIsochPacket(DYNACORD_EP_FEEDBACK, mFbBuf[i], mFbFrames[i], kFbPktsPerXfer,
				       (void*)(uintptr_t)i);
}

/* Bulk IN transfer number seq reads straight into its fixed input ring slot. */
bool DynacordEngine::SubmitIn(uint64_t seq)
{
	uint8_t* slot = DynacordInputSlot((uint8_t*)SHM()->audio.inputBuffer, mLayout, seq);
	return mBus->SubmitUSBPacket(DYNACORD_EP_PCM_IN, slot, mLayout.inFramesPerXfer * kDynacordInFrameBytes,
				     (void*)(uintptr_t)seq);
}

/* Reset the counters and the published anchor, then queue every feedback,
 * bulk IN and OUT transfer. */
bool DynacordEngine::StartStreaming()
{
	volatile OzzyAudioBuffer& a = SHM()->audio;
	pacer_init(&mPacer, mRate);
	memset((void*)a.outputBuffer, 0, sizeof(a.outputBuffer));
	memset((void*)a.inputBuffer, 0, sizeof(a.inputBuffer));
	mSent = mConsumed = mReceived = mNextAnchor = mInShort = mIsoErrors = 0;
	mFailed = false;
	mTicksPerMs = HostTicksPerMs();
	mHaveRef = mBus->GetBusFrame(&mRefFrame, &mRefHost);
	mLastFbHost = mBus->GetTime();
	/* A new timeline: (0, 0) tells the HAL there is no anchor yet, so it keeps
	 * serving its last one until anchor 0 of this stream arrives. */
	WriteTimestamp(0, 0);

	for (uint32_t i = 0; i < kFbXfers; i++)
		if (!SubmitFeedback(i))
			return false;
	for (uint64_t s = 0; s < kInXfers; s++)
		if (!SubmitIn(s))
			return false;
	for (uint32_t i = 0; i < mOutXfers; i++) {
		FillOut(i);
		if (!SubmitOut(i))
			return false;
	}
	return true;
}

/* Host time at which bus frame `frame` started, from the latest reference
 * pair; falls back to now when the bus can't report frames. */
uint64_t DynacordEngine::HostTimeOfBusFrame(uint64_t frame)
{
	if (!mHaveRef)
		return mBus->GetTime();
	int64_t df = (int64_t)(frame - mRefFrame);
	return (uint64_t)((int64_t)mRefHost + df * (int64_t)mTicksPerMs);
}

/* Seqlock write of the published (sampleTime, hostTime) pair. The HAL reads
 * it from another address space (acquire on the sequence, then an acquire
 * fence before re-reading it); the release fences keep the odd sequence
 * ahead of the data and the data ahead of the even sequence on arm64,
 * where the kernel's plain volatile stores are otherwise unordered. */
void DynacordEngine::WriteTimestamp(uint64_t sampleTime, uint64_t hostTime)
{
	volatile auto& ts = SHM()->audio.timestamp;
	uint32_t seq = ts.sequence;
	ts.sequence = seq + 1;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	ts.sampleTime = sampleTime;
	ts.hostTime = hostTime;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	ts.sequence = seq + 2;
}

/* Publish one anchor; anchor 0 makes the driver ready. */
void DynacordEngine::PublishAnchor(uint64_t sampleTime, uint64_t hostTime)
{
	WriteTimestamp(sampleTime, hostTime);
	if (sampleTime == 0)
		SHM()->audio.driverReady = true;
}

/* The mixer consumed `frames` during bus frame busFrame. Publish an anchor
 * for every multiple of the period crossed, interpolated within the ms. */
void DynacordEngine::Consume(int frames, uint64_t busFrame)
{
	uint64_t before = mConsumed;
	mConsumed += (uint64_t)frames;
	while (mNextAnchor <= mConsumed) {
		uint64_t into = mNextAnchor - before;
		PublishAnchor(mNextAnchor, HostTimeOfBusFrame(busFrame) + into * mTicksPerMs / (uint64_t)frames);
		mNextAnchor += mPeriod;
	}
}

/* Feedback completions drive the pacer and the clock; OUT completions are
 * refilled and resubmitted. Any transfer error fails the stream. */
void DynacordEngine::OnIsochComplete(uint8_t pipeID, void* ctx, int status, uint64_t startFrame,
				     OzzyIsochFrame* frames, uint32_t frameCount)
{
	if (mStopping || mFailed)
		return;
	uint32_t i = (uint32_t)(uintptr_t)ctx;
	if (status != 0) {
		LogDynacordError("iso pipe %02x failed: 0x%x", pipeID, status);
		mFailed = true;
		return;
	}
	if (pipeID == DYNACORD_EP_FEEDBACK) {
		for (uint32_t k = 0; k < frameCount; k++) {
			if (frames[k].status != 0 || frames[k].actCount < 1)
				continue;
			int v = feedback_parse(mFbBuf[i] + k * kFbPktSize, frames[k].actCount);
			pacer_feedback(&mPacer, v);
			if (!ValidFeedback(v, mRate))
				continue;
			Consume(v, startFrame + k);
			mLastFbHost = mBus->GetTime();
		}
		if (!SubmitFeedback(i))
			mFailed = true;
	} else if (pipeID == DYNACORD_EP_PCM_OUT) {
		for (uint32_t k = 0; k < frameCount; k++)
			mIsoErrors += frames[k].status != 0;
		FillOut(i);
		if (!SubmitOut(i))
			mFailed = true;
	}
}

/* A bulk IN read landed in its ring slot. A short read zero-fills the rest
 * of the slot so slot positions stay fixed, then the slot kInXfers ahead is
 * queued. */
void DynacordEngine::OnPacketComplete(uint8_t pipeID, void* ctx, int status, uint32_t bytes)
{
	if (mStopping || mFailed || pipeID != DYNACORD_EP_PCM_IN)
		return;
	if (status != 0) {
		LogDynacordError("bulk in failed: 0x%x", status);
		mFailed = true;
		return;
	}
	uint64_t seq = (uint64_t)(uintptr_t)ctx;
	uint32_t want = mLayout.inFramesPerXfer * kDynacordInFrameBytes;
	if (bytes < want) {
		memset(DynacordInputSlot((uint8_t*)SHM()->audio.inputBuffer, mLayout, seq) + bytes, 0, want - bytes);
		mInShort++;
	}
	mReceived += mLayout.inFramesPerXfer;
	if (!SubmitIn(seq + kInXfers))
		mFailed = true;
}
