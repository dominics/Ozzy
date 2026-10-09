/* SPDX-License-Identifier: MIT */
#include <mach/mach_time.h>
#include <vector>
#include "../../userspace/ploytec-play/tests/fixtures/counter_44100.h"
#include "../../userspace/ploytec-play/tests/fixtures/counter_48000.h"
#include "../../userspace/ploytec-play/tests/fixtures/counter_96000.h"
#include "engine_fixture.h"
#include "golden_compare.h"
#include "test.h"
#include "tests.h"

static uint64_t TicksPerMs()
{
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	return 1000000ull * tb.denom / tb.numer;
}

/* Complete the oldest feedback transfer with up to 5 readings for bus
 * frames startFrame.. and let the engine resubmit it. */
static void CompleteFeedback(EngineFixture& f, uint64_t startFrame, const int* values, int n)
{
	FakeBus::Iso s = f.bus.PopIso(DYNACORD_EP_FEEDBACK);
	for (uint32_t i = 0; i < s.frameCount; i++) {
		bool have = (int)i < n && values[i] >= 0;
		s.frames[i].actCount = have ? 3 : 0;
		s.frames[i].status = 0;
		if (have)
			s.buffer[i * DynacordEngine::kFbPktSize] = (uint8_t)values[i];
	}
	f.engine.OnIsochComplete(DYNACORD_EP_FEEDBACK, s.ctx, 0, startFrame, s.frames, s.frameCount);
}

/* Complete the oldest OUT transfer; returns the engine's refill. */
static FakeBus::Iso CompleteOut(EngineFixture& f)
{
	FakeBus::Iso s = f.bus.PopIso(DYNACORD_EP_PCM_OUT);
	for (uint32_t i = 0; i < s.frameCount; i++) {
		s.frames[i].actCount = s.frames[i].reqCount;
		s.frames[i].status = 0;
	}
	f.engine.OnIsochComplete(DYNACORD_EP_PCM_OUT, s.ctx, 0, 0, s.frames, s.frameCount);
	return f.bus.iso.back();
}

void test_engine_start_queues_transfers()
{
	EngineFixture f;
	CHECK(f.engine.Start());
	int out = 0, fb = 0;
	for (const auto& s : f.bus.iso) {
		out += s.pipe == DYNACORD_EP_PCM_OUT;
		fb += s.pipe == DYNACORD_EP_FEEDBACK;
		if (s.pipe == DYNACORD_EP_PCM_OUT)
			CHECK_EQ(s.frameCount, DynacordEngine::kOutPktsPerXfer);
		if (s.pipe == DYNACORD_EP_FEEDBACK)
			for (uint16_t r : s.reqCounts)
				CHECK_EQ(r, DynacordEngine::kFbPktSize);
	}
	CHECK_EQ(out, f.engine.OutXfers());
	CHECK_EQ(fb, DynacordEngine::kFbXfers);
	CHECK_EQ(f.bus.bulk.size(), DynacordEngine::kInXfers);
	for (const auto& b : f.bus.bulk)
		CHECK_EQ(b.size, 144 * kDynacordInFrameBytes);
}

void test_engine_queue_depth()
{
	EngineFixture f;
	DynacordEngine deep(32);
	deep.SetRate(48000);
	deep.Init(&f.bus, f.shm.get());
	f.bus.SetEngine(&deep);
	CHECK(deep.Start());
	int out = 0;
	for (const auto& s : f.bus.iso)
		out += s.pipe == DYNACORD_EP_PCM_OUT;
	CHECK_EQ(out, 32);
}

void test_engine_ready_waits_for_first_anchor()
{
	EngineFixture f;
	CHECK(f.engine.Start());
	CHECK(f.shm->audio.hardwarePresent);
	CHECK(!f.shm->audio.driverReady);
	int empty[5] = { -1, -1, -1, -1, -1 };
	CompleteFeedback(f, 10, empty, 5);
	CHECK(!f.shm->audio.driverReady);
	int v[5] = { 48, 48, 48, 48, 48 };
	CompleteFeedback(f, 15, v, 5);
	CHECK(f.shm->audio.driverReady);
	CHECK_EQ(f.shm->audio.timestamp.sampleTime, 0);
}

void test_engine_restart_zeroes_anchor()
{
	EngineFixture f;
	f.bus.refFrame = 0;
	f.bus.refHost = 1000;
	CHECK(f.engine.Start());
	int v[5] = { 48, 48, 48, 48, 48 };
	for (uint64_t frame = 0; frame < 30; frame += 5)
		CompleteFeedback(f, frame, v, 5);
	CHECK(f.shm->audio.timestamp.hostTime != 0);
	f.engine.Stop();
	f.bus.iso.clear();
	f.bus.bulk.clear();
	CHECK(f.engine.Start());
	CHECK_EQ(f.shm->audio.timestamp.sampleTime, 0);
	CHECK_EQ(f.shm->audio.timestamp.hostTime, 0);
	CompleteFeedback(f, 40, v, 5);
	CHECK(f.shm->audio.timestamp.hostTime != 0);
}

void test_engine_anchors_from_bus_frames()
{
	EngineFixture f;
	f.bus.refFrame = 100;
	f.bus.refHost = 5000000;
	CHECK(f.engine.Start());
	const uint64_t tpm = TicksPerMs();
	for (uint64_t frame = 100; frame < 100 + 150; frame += 5) {
		int v[5] = { 48, 48, 48, 48, 48 };
		CompleteFeedback(f, frame, v, 5);
	}
	/* 150 ms at 48/ms = 7200 frames; anchors every 4800 frames = every 100 ms. */
	const auto& ts = f.shm->audio.timestamp;
	CHECK_EQ(ts.sampleTime % 4800, 0);
	CHECK_EQ(ts.sampleTime, 4800);
	CHECK_EQ(ts.hostTime, 5000000 + (ts.sampleTime / 48) * tpm);
	CHECK_EQ(f.engine.FramesConsumed(), 7200);
}

void test_engine_anchor_interpolates_within_ms()
{
	EngineFixture f(44100);
	f.bus.refFrame = 0;
	f.bus.refHost = 0;
	CHECK(f.engine.Start());
	const uint64_t tpm = TicksPerMs();
	/* 4410 is the first anchor after 0; it falls 10 frames into ms 100 (100 x 44 = 4400). */
	for (uint64_t frame = 0; frame < 105; frame += 5) {
		int v[5] = { 44, 44, 44, 44, 44 };
		CompleteFeedback(f, frame, v, 5);
	}
	CHECK_EQ(f.shm->audio.timestamp.sampleTime, 4410);
	CHECK_EQ(f.shm->audio.timestamp.hostTime, 100 * tpm + 10 * tpm / 44);
}

void test_engine_ignores_invalid_feedback()
{
	EngineFixture f;
	CHECK(f.engine.Start());
	int good[5] = { 48, 48, 48, 48, 48 };
	CompleteFeedback(f, 0, good, 5);
	uint64_t consumed = f.engine.FramesConsumed();
	uint64_t anchor = f.shm->audio.timestamp.sampleTime;
	int bad[5] = { 0, 200, 45, 51, 255 };   /* 45 and 51 are 3 away from 48 */
	CompleteFeedback(f, 5, bad, 5);
	CHECK_EQ(f.engine.FramesConsumed(), consumed);
	CHECK_EQ(f.shm->audio.timestamp.sampleTime, anchor);
}

void test_engine_output_takes_ring_and_zeroes()
{
	EngineFixture f;
	CHECK(f.engine.Start());
	uint8_t* ring = (uint8_t*)f.shm->audio.outputBuffer;
	uint64_t pos = f.engine.FramesSent();
	for (uint32_t i = 0; i < 400 * kDynacordOutFrameBytes; i++)
		ring[pos * kDynacordOutFrameBytes + i] = (uint8_t)(i % 251 + 1);
	FakeBus::Iso refill = CompleteOut(f);
	uint32_t bytes = 0;
	for (uint16_t r : refill.reqCounts)
		bytes += r;
	CHECK_EQ(bytes % kDynacordOutFrameBytes, 0);
	for (uint32_t i = 0; i < bytes; i++) {
		CHECK_EQ(refill.buffer[i], (uint8_t)(i % 251 + 1));
		CHECK_EQ(ring[pos * kDynacordOutFrameBytes + i], 0);
	}
}

void test_engine_bulk_in_slots()
{
	EngineFixture f;
	CHECK(f.engine.Start());
	DynacordRingLayout l = DynacordRingLayoutFor(48000);
	uint8_t* in = (uint8_t*)f.shm->audio.inputBuffer;
	for (uint64_t seq = 0; seq < DynacordEngine::kInXfers; seq++) {
		FakeBus::Bulk b = f.bus.PopBulk(DYNACORD_EP_PCM_IN);
		CHECK(b.buffer == DynacordInputSlot(in, l, seq));
		f.engine.OnPacketComplete(DYNACORD_EP_PCM_IN, b.ctx, 0, b.size);
		CHECK(f.bus.bulk.back().buffer == DynacordInputSlot(in, l, seq + DynacordEngine::kInXfers));
		CHECK_EQ(f.engine.FramesReceived(), (seq + 1) * l.inFramesPerXfer);
	}
}

void test_engine_short_bulk_read_keeps_slots()
{
	EngineFixture f;
	CHECK(f.engine.Start());
	DynacordRingLayout l = DynacordRingLayoutFor(48000);
	uint8_t* in = (uint8_t*)f.shm->audio.inputBuffer;
	FakeBus::Bulk b = f.bus.PopBulk(DYNACORD_EP_PCM_IN);
	std::memset(b.buffer, 0x5A, b.size);
	f.engine.OnPacketComplete(DYNACORD_EP_PCM_IN, b.ctx, 0, b.size / 2);
	for (uint32_t i = b.size / 2; i < b.size; i++)
		CHECK_EQ(b.buffer[i], 0);
	CHECK_EQ(f.engine.InShortReads(), 1);
	CHECK(f.bus.bulk.back().buffer == DynacordInputSlot(in, l, DynacordEngine::kInXfers));
}

void test_engine_transfer_error_fails()
{
	EngineFixture f;
	CHECK(f.engine.Start());
	FakeBus::Iso s = f.bus.PopIso(DYNACORD_EP_PCM_OUT);
	size_t before = f.bus.iso.size();
	f.engine.OnIsochComplete(DYNACORD_EP_PCM_OUT, s.ctx, (int)0xE00002C0 /* kIOReturnNoDevice */, 0, s.frames, s.frameCount);
	CHECK(f.engine.Failed());
	CHECK_EQ(f.bus.iso.size(), before);   /* not resubmitted */
}

void test_engine_stop_stops_resubmitting()
{
	EngineFixture f;
	CHECK(f.engine.Start());
	f.engine.Stop();
	size_t iso = f.bus.iso.size(), bulk = f.bus.bulk.size();
	FakeBus::Iso s = f.bus.PopIso(DYNACORD_EP_PCM_OUT);
	f.engine.OnIsochComplete(DYNACORD_EP_PCM_OUT, s.ctx, 0, 0, s.frames, s.frameCount);
	FakeBus::Bulk b = f.bus.PopBulk(DYNACORD_EP_PCM_IN);
	f.engine.OnPacketComplete(DYNACORD_EP_PCM_IN, b.ctx, 0, b.size);
	CHECK_EQ(f.bus.iso.size(), iso - 1);
	CHECK_EQ(f.bus.bulk.size(), bulk - 1);
}

/* The Windows captures again, but through the whole engine: feedback
 * transfers of 5 readings every 5 ms and an OUT transfer refilled every
 * 3 ms, comparing every packet the engine submits with what Windows sent. */
static void GoldenThroughEngine(uint32_t rate, const unsigned char* fb, size_t nfb, const unsigned char* win, size_t nwin)
{
	EngineFixture f(rate);
	CHECK(f.engine.Start());
	std::vector<int> sizes;
	for (const auto& s : f.bus.iso)
		if (s.pipe == DYNACORD_EP_PCM_OUT)
			for (uint16_t r : s.reqCounts)
				sizes.push_back(r / kDynacordOutFrameBytes);
	for (size_t ms = 0; ms + 5 <= nfb && sizes.size() < nwin; ms++) {
		if (ms % 5 == 0) {
			int v[5];
			for (int i = 0; i < 5; i++)
				v[i] = fb[ms + i];
			CompleteFeedback(f, ms, v, 5);
		}
		if (ms % 3 == 0) {
			FakeBus::Iso refill = CompleteOut(f);
			for (uint16_t r : refill.reqCounts)
				sizes.push_back(r / kDynacordOutFrameBytes);
		}
	}
	GoldenTally tally;
	for (size_t i = 0; i < sizes.size() && i < nwin; i++)
		tally.add(sizes[i], win[i]);
	tally.check(rate, 3);
}

void test_engine_golden()
{
	GoldenThroughEngine(44100, fb_44100, sizeof(fb_44100), out_44100, sizeof(out_44100));
	GoldenThroughEngine(48000, fb_48000, sizeof(fb_48000), out_48000, sizeof(out_48000));
	GoldenThroughEngine(96000, fb_96000, sizeof(fb_96000), out_96000, sizeof(out_96000));
}
