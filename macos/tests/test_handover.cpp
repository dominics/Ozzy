/* SPDX-License-Identifier: MIT */
#include "../Backends/OzzyDaemon/ConsoleHandover.h"
#include "test.h"
#include "tests.h"

using A = ConsoleHandover::Action;
using R = ConsoleHandover::StartResult;
static const uid_t kMe = 501, kOther = 502;

void test_handover_starts_only_when_front_and_present()
{
	ConsoleHandover h(kMe);
	CHECK(h.OnDevice(true) == A::kNone);          /* not front yet */
	CHECK(h.OnConsole(kOther) == A::kNone);
	CHECK(h.OnConsole(kMe) == A::kStart);
	CHECK(h.OnStartResult(R::kStarted) == A::kNone);
	CHECK(h.Running());

	ConsoleHandover g(kMe);
	CHECK(g.OnConsole(kMe) == A::kNone);           /* front, no mixer yet */
	CHECK(g.OnDevice(true) == A::kStart);
}

void test_handover_yields_on_switch_and_login_window()
{
	ConsoleHandover h(kMe);
	h.OnConsole(kMe);
	h.OnDevice(true);
	h.OnStartResult(R::kStarted);
	CHECK(h.OnConsole(kOther) == A::kStop);
	CHECK(!h.Running());
	CHECK(h.OnConsole(kMe) == A::kStart);
	h.OnStartResult(R::kStarted);
	CHECK(h.OnConsole(ConsoleHandover::kNoConsole) == A::kStop);
	CHECK(h.OnConsole(ConsoleHandover::kNoConsole) == A::kNone); /* repeat is a no-op */
}

void test_handover_unplug_and_replug()
{
	ConsoleHandover h(kMe);
	h.OnConsole(kMe);
	h.OnDevice(true);
	h.OnStartResult(R::kStarted);
	CHECK(h.OnDevice(false) == A::kStop);
	CHECK(h.OnDevice(false) == A::kNone);
	CHECK(h.OnDevice(true) == A::kStart);
}

void test_handover_busy_falls_back_to_slow_retries()
{
	ConsoleHandover h(kMe);
	h.OnConsole(kMe);
	h.OnDevice(true);
	for (int i = 0; i < ConsoleHandover::kMaxRetries; i++) {
		CHECK(h.OnStartResult(R::kBusy) == A::kRetryLater);
		CHECK(h.OnRetryTimer() == A::kStart);
	}
	/* The other daemon is slow to let go (crash respawn, a USB start in
	 * progress): keep trying, slowly, for as long as this user is in front. */
	for (int i = 0; i < 3; i++) {
		CHECK(h.OnStartResult(R::kBusy) == A::kRetrySlowly);
		CHECK(h.OnRetryTimer() == A::kStart);
	}
	CHECK(h.OnStartResult(R::kStarted) == A::kNone);
	CHECK(h.Running());
	CHECK(h.OnDevice(false) == A::kStop);
	CHECK(h.OnDevice(true) == A::kStart);          /* a replug starts a fresh fast round */
	CHECK(h.OnStartResult(R::kBusy) == A::kRetryLater);
}

void test_handover_slow_retry_stops_when_front_lost()
{
	ConsoleHandover h(kMe);
	h.OnConsole(kMe);
	h.OnDevice(true);
	for (int i = 0; i < ConsoleHandover::kMaxRetries; i++) {
		h.OnStartResult(R::kBusy);
		h.OnRetryTimer();
	}
	CHECK(h.OnStartResult(R::kBusy) == A::kRetrySlowly);
	CHECK(h.OnConsole(kOther) == A::kNone);
	CHECK(h.OnRetryTimer() == A::kNone);           /* the pending slow timer does nothing */
}

void test_handover_retry_timer_after_losing_front_does_nothing()
{
	ConsoleHandover h(kMe);
	h.OnConsole(kMe);
	h.OnDevice(true);
	CHECK(h.OnStartResult(R::kBusy) == A::kRetryLater);
	CHECK(h.OnConsole(kOther) == A::kNone);        /* nothing running to stop */
	CHECK(h.OnRetryTimer() == A::kNone);
	CHECK(h.OnConsole(kMe) == A::kStart);
}

void test_handover_failed_start_and_session_gone()
{
	ConsoleHandover h(kMe);
	h.OnConsole(kMe);
	h.OnDevice(true);
	CHECK(h.OnStartResult(R::kFailed) == A::kGiveUp);
	CHECK(!h.Running());
	CHECK(h.OnConsole(kOther) == A::kNone);
	CHECK(h.OnConsole(kMe) == A::kStart);          /* coming back to the front tries again */
	h.OnStartResult(R::kStarted);
	h.OnSessionGone();                             /* USB thread gave up */
	CHECK(!h.Running());
	CHECK(h.OnConsole(kOther) == A::kNone);
}
