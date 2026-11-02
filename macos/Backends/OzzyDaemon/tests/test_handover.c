/* SPDX-License-Identifier: MIT */
/* Which user's daemon drives a device: console changes, plugs, busy retries, failures */

#include "../handover.h"
#include "../../../../tests/check.h"

static const uid_t me = 501, other = 502;

/* In front with the device attached and started */
static void running(struct handover *h)
{
	handover_init(h, me);
	handover_console(h, me);
	handover_device(h, true);
	handover_started(h, HANDOVER_STARTED);
}

static void starts_only_when_front_and_present(void)
{
	struct handover h, g;

	handover_init(&h, me);
	CHECK(handover_device(&h, true) == HANDOVER_NONE, "not in front yet");
	CHECK(handover_console(&h, other) == HANDOVER_NONE, "someone else in front");
	CHECK(handover_console(&h, me) == HANDOVER_START, "our turn");
	CHECK(handover_started(&h, HANDOVER_STARTED) == HANDOVER_NONE && h.running, "running");

	handover_init(&g, me);
	CHECK(handover_console(&g, me) == HANDOVER_NONE, "in front, no device yet");
	CHECK(handover_device(&g, true) == HANDOVER_START, "the device appears");

	handover_init(&g, me);
	CHECK(handover_device(&g, true) == HANDOVER_NONE, "attached at the login window");
	CHECK(handover_console(&g, HANDOVER_NO_CONSOLE) == HANDOVER_NONE, "still the login window");
}

static void yields_on_switch_and_login_window(void)
{
	struct handover h;

	running(&h);
	CHECK(handover_console(&h, other) == HANDOVER_STOP && !h.running, "a switch releases it");
	CHECK(handover_console(&h, me) == HANDOVER_START, "back in front takes it");
	handover_started(&h, HANDOVER_STARTED);
	CHECK(handover_console(&h, HANDOVER_NO_CONSOLE) == HANDOVER_STOP, "the login window releases it");
	CHECK(handover_console(&h, HANDOVER_NO_CONSOLE) == HANDOVER_NONE, "a repeat does nothing");
}

static void unplug_and_replug(void)
{
	struct handover h;

	running(&h);
	CHECK(handover_device(&h, false) == HANDOVER_STOP, "unplug stops");
	CHECK(handover_device(&h, false) == HANDOVER_NONE, "a repeat does nothing");
	CHECK(handover_device(&h, true) == HANDOVER_START, "replug starts");
}

static void busy_falls_back_to_slow_retries(void)
{
	struct handover h;
	int i;

	handover_init(&h, me);
	handover_console(&h, me);
	handover_device(&h, true);
	for (i = 0; i < HANDOVER_FAST_RETRIES; i++) {
		CHECK(handover_started(&h, HANDOVER_BUSY) == HANDOVER_RETRY_SOON, "fast retry %d", i);
		CHECK(handover_retry(&h) == HANDOVER_START, "fast retry %d starts", i);
	}
	/* Whoever has it is slow to let go (a crashed daemon's respawn, a kext): keep trying slowly */
	for (i = 0; i < 3; i++) {
		CHECK(handover_started(&h, HANDOVER_BUSY) == HANDOVER_RETRY_SLOWLY, "slow retry %d", i);
		CHECK(handover_retry(&h) == HANDOVER_START, "slow retry %d starts", i);
	}
	CHECK(handover_started(&h, HANDOVER_STARTED) == HANDOVER_NONE && h.running, "it let go");
	CHECK(handover_device(&h, false) == HANDOVER_STOP, "unplug");
	CHECK(handover_device(&h, true) == HANDOVER_START, "replug");
	CHECK(handover_started(&h, HANDOVER_BUSY) == HANDOVER_RETRY_SOON, "a replug starts a fast round");
}

static void slow_retry_stops_when_front_lost(void)
{
	struct handover h;
	int i;

	handover_init(&h, me);
	handover_console(&h, me);
	handover_device(&h, true);
	for (i = 0; i < HANDOVER_FAST_RETRIES; i++) {
		handover_started(&h, HANDOVER_BUSY);
		handover_retry(&h);
	}
	CHECK(handover_started(&h, HANDOVER_BUSY) == HANDOVER_RETRY_SLOWLY, "slow");
	CHECK(handover_console(&h, other) == HANDOVER_NONE, "nothing running to stop");
	CHECK(handover_retry(&h) == HANDOVER_NONE, "the pending timer does nothing");
	CHECK(handover_console(&h, me) == HANDOVER_START, "back in front");
	CHECK(handover_started(&h, HANDOVER_BUSY) == HANDOVER_RETRY_SOON, "with a fast round");
}

static void retry_after_unplug_does_nothing(void)
{
	struct handover h;

	handover_init(&h, me);
	handover_console(&h, me);
	handover_device(&h, true);
	CHECK(handover_started(&h, HANDOVER_BUSY) == HANDOVER_RETRY_SOON, "busy");
	CHECK(handover_device(&h, false) == HANDOVER_NONE, "unplugged while waiting");
	CHECK(handover_retry(&h) == HANDOVER_NONE, "the pending timer does nothing");
	CHECK(handover_started(&h, HANDOVER_BUSY) == HANDOVER_NONE, "a late busy result does nothing");
}

/* A control request timing out at plug-in: retried quickly a few times, then slowly */
static void failed_start_is_retried(void)
{
	struct handover h;
	int i;

	handover_init(&h, me);
	handover_console(&h, me);
	handover_device(&h, true);
	for (i = 0; i < HANDOVER_FAILED_RETRIES; i++) {
		CHECK(handover_started(&h, HANDOVER_FAILED) == HANDOVER_RETRY_FAILED && !h.running, "failed retry %d", i);
		CHECK(handover_retry(&h) == HANDOVER_START, "failed retry %d starts", i);
	}
	for (i = 0; i < 3; i++) {
		CHECK(handover_started(&h, HANDOVER_FAILED) == HANDOVER_RETRY_SLOWLY, "still failing: slowly %d", i);
		CHECK(handover_retry(&h) == HANDOVER_START, "slow retry %d starts", i);
	}
	CHECK(handover_started(&h, HANDOVER_STARTED) == HANDOVER_NONE && h.running, "it started");
	CHECK(handover_device(&h, false) == HANDOVER_STOP, "unplug");
	CHECK(handover_device(&h, true) == HANDOVER_START, "replug");
	CHECK(handover_started(&h, HANDOVER_FAILED) == HANDOVER_RETRY_FAILED, "a replug starts a fast round");

	/* Busy and failed results count separately */
	handover_retry(&h);
	CHECK(handover_started(&h, HANDOVER_BUSY) == HANDOVER_RETRY_SOON, "busy after a failure: fast");
	handover_retry(&h);
	CHECK(handover_started(&h, HANDOVER_FAILED) == HANDOVER_RETRY_FAILED, "failed again: still fast");

	/* Away from front or unplugged while waiting: nothing more */
	CHECK(handover_console(&h, other) == HANDOVER_NONE, "nothing running to stop");
	CHECK(handover_retry(&h) == HANDOVER_NONE, "the pending timer does nothing");
	CHECK(handover_started(&h, HANDOVER_FAILED) == HANDOVER_NONE, "a late failure does nothing");
	CHECK(handover_console(&h, me) == HANDOVER_START, "back in front tries again");
	CHECK(handover_started(&h, HANDOVER_FAILED) == HANDOVER_RETRY_FAILED, "with a fast round");
}

static void unsupported_gives_up(void)
{
	struct handover h;

	handover_init(&h, me);
	handover_console(&h, me);
	handover_device(&h, true);
	CHECK(handover_started(&h, HANDOVER_UNSUPPORTED) == HANDOVER_GIVE_UP && !h.running, "give up");
	CHECK(handover_retry(&h) == HANDOVER_NONE, "no retry after giving up");
	CHECK(handover_console(&h, other) == HANDOVER_NONE, "nothing to stop");
	CHECK(handover_console(&h, me) == HANDOVER_START, "back in front tries again");
}

static void lost_is_retried_slowly(void)
{
	struct handover h;

	running(&h);
	CHECK(handover_lost(&h) == HANDOVER_RETRY_SLOWLY && !h.running, "a failed recovery: retry slowly");
	CHECK(handover_retry(&h) == HANDOVER_START, "the retry starts it");
	CHECK(handover_started(&h, HANDOVER_FAILED) == HANDOVER_RETRY_SLOWLY, "a failed start after a loss: slowly again");
	CHECK(handover_retry(&h) == HANDOVER_START, "and again");
	CHECK(handover_started(&h, HANDOVER_BUSY) == HANDOVER_RETRY_SLOWLY, "busy after a loss: slowly too");
	CHECK(handover_retry(&h) == HANDOVER_START, "once more");
	CHECK(handover_started(&h, HANDOVER_STARTED) == HANDOVER_NONE && h.running, "back");
	CHECK(handover_lost(&h) == HANDOVER_RETRY_SLOWLY, "lost again");
	CHECK(handover_device(&h, false) == HANDOVER_NONE, "unplugged while waiting: nothing to stop");
	CHECK(handover_retry(&h) == HANDOVER_NONE, "and no retry");
	CHECK(handover_device(&h, true) == HANDOVER_START, "replug starts a new round");
	CHECK(handover_started(&h, HANDOVER_FAILED) == HANDOVER_RETRY_FAILED, "a new round retries a failure quickly");

	running(&h);
	handover_console(&h, other);
	CHECK(handover_lost(&h) == HANDOVER_NONE, "lost while not in front: no retry");
}

int main(void)
{
	starts_only_when_front_and_present();
	yields_on_switch_and_login_window();
	unplug_and_replug();
	busy_falls_back_to_slow_retries();
	slow_retry_stops_when_front_lost();
	retry_after_unplug_does_nothing();
	failed_start_is_retried();
	unsupported_gives_up();
	lost_is_retried_slowly();
	return CHECK_DONE("handover");
}
