/* SPDX-License-Identifier: MIT */
#include <mach/mach_time.h>
#include "engine_fixture.h"
#include "test.h"
#include "tests.h"

static uint64_t Ms(uint64_t ms)
{
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	return ms * 1000000ull * tb.denom / tb.numer;
}

void test_poll_running_while_feedback_flows()
{
	EngineFixture f;
	CHECK(f.engine.Start());
	f.bus.now += Ms(900);
	CHECK(f.engine.Poll() == DynacordEngine::PollResult::kRunning);
}

void test_poll_restarts_after_feedback_silence()
{
	EngineFixture f;
	CHECK(f.engine.Start());
	f.bus.now += Ms(1001);
	CHECK(f.engine.Poll() == DynacordEngine::PollResult::kRestart);
	CHECK_EQ(f.engine.NextRate(), 48000);
}

void test_poll_restarts_after_failure()
{
	EngineFixture f;
	CHECK(f.engine.Start());
	FakeBus::Iso s = f.bus.PopIso(DYNACORD_EP_FEEDBACK);
	f.engine.OnIsochComplete(DYNACORD_EP_FEEDBACK, s.ctx, (int)0xE00002ED /* kIOReturnNotResponding */, 0, s.frames, s.frameCount);
	CHECK(f.engine.Poll() == DynacordEngine::PollResult::kRestart);
}

void test_poll_rate_request()
{
	EngineFixture f;
	CHECK(f.engine.Start());
	f.shm->audio.requestedRate = 96000;
	CHECK(f.engine.Poll() == DynacordEngine::PollResult::kRestart);
	CHECK_EQ(f.engine.NextRate(), 96000);
}

void test_poll_same_or_unsupported_rate_ignored()
{
	EngineFixture f;
	CHECK(f.engine.Start());
	f.shm->audio.requestedRate = 48000;
	CHECK(f.engine.Poll() == DynacordEngine::PollResult::kRunning);
	CHECK_EQ(f.shm->audio.requestedRate, 0);
	f.shm->audio.requestedRate = 88200;
	CHECK(f.engine.Poll() == DynacordEngine::PollResult::kRunning);
	CHECK_EQ(f.shm->audio.requestedRate, 0);
	CHECK_EQ(f.engine.NextRate(), 48000);
}

void test_poll_refreshes_bus_frame_reference()
{
	EngineFixture f;
	f.bus.refFrame = 0;
	f.bus.refHost = 0;
	CHECK(f.engine.Start());
	f.bus.refFrame = 1000;
	f.bus.refHost = Ms(1000) + 77;      /* the host clock drifted 77 ticks */
	f.engine.Poll();
	FakeBus::Iso s = f.bus.PopIso(DYNACORD_EP_FEEDBACK);
	for (uint32_t i = 0; i < s.frameCount; i++) {
		s.frames[i].actCount = 3;
		s.frames[i].status = 0;
		s.buffer[i * DynacordEngine::kFbPktSize] = 48;
	}
	f.engine.OnIsochComplete(DYNACORD_EP_FEEDBACK, s.ctx, 0, 1000, s.frames, s.frameCount);
	CHECK_EQ(f.shm->audio.timestamp.hostTime, Ms(1000) + 77);   /* anchor 0 at frame 1000 */
}

/* Deliver one feedback transfer of nominal 48 kHz readings starting at bus
 * frame startFrame, so anchor 0 is published for that frame. */
static void FeedbackAt(EngineFixture& f, uint64_t startFrame)
{
	FakeBus::Iso s = f.bus.PopIso(DYNACORD_EP_FEEDBACK);
	for (uint32_t i = 0; i < s.frameCount; i++) {
		s.frames[i].actCount = 3;
		s.frames[i].status = 0;
		s.buffer[i * DynacordEngine::kFbPktSize] = 48;
	}
	f.engine.OnIsochComplete(DYNACORD_EP_FEEDBACK, s.ctx, 0, startFrame, s.frames, s.frameCount);
}

/* GetBusFrame's time is read 0-1 ms into the frame: the reference keeps the
 * sample nearest its frame start and ignores later-reading ones. */
void test_poll_keeps_earliest_bus_frame_reference()
{
	EngineFixture f;
	f.bus.refFrame = 0;
	f.bus.refHost = Ms(1) / 2;           /* read 0.5 ms into frame 0 */
	CHECK(f.engine.Start());
	f.bus.refFrame = 1000;
	f.bus.refHost = Ms(1000) + 77;       /* read 77 ticks into frame 1000: earlier */
	f.engine.Poll();
	f.bus.refFrame = 2000;
	f.bus.refHost = Ms(2000) + Ms(1) * 9 / 10;   /* 0.9 ms into frame 2000: later */
	f.engine.Poll();
	FeedbackAt(f, 3000);
	CHECK_EQ(f.shm->audio.timestamp.hostTime, Ms(3000) + 77);
}

/* A later-reading sample still wins once the 500 ppm drift allowance since
 * the reference exceeds its lateness, so the reference can't get stuck. */
void test_poll_bus_frame_reference_follows_drift()
{
	EngineFixture f;
	f.bus.refFrame = 0;
	f.bus.refHost = 0;
	CHECK(f.engine.Start());
	f.bus.refFrame = 1000;
	f.bus.refHost = Ms(1000) + Ms(1) * 9 / 10;   /* 0.9 ms late, allowance 0.5 ms */
	f.engine.Poll();
	f.bus.refFrame = 2000;
	f.bus.refHost = Ms(2000) + Ms(1) * 9 / 10;   /* 0.9 ms late, allowance 1 ms */
	f.engine.Poll();
	FeedbackAt(f, 2000);
	CHECK_EQ(f.shm->audio.timestamp.hostTime, Ms(2000) + Ms(1) * 9 / 10);
}
