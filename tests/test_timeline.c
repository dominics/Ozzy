/* SPDX-License-Identifier: MIT */
/*
 * OzzyHAL's device timelines (macos/OzzyHAL/OzzyTimeline.h): the anchor served while the engine
 * has none, a new seed when the clock goes back, a restart from another thread dropping the
 * anchor at the IO thread's next call, and two timelines on one clock (the main device and a
 * mono device) restarting independently. Then the clock without a driver: the last anchor moved
 * on by whole periods at the nominal rate, and a driver coming back as a new clock (a rebase).
 */

#include "macos/OzzyHAL/OzzyTimeline.h"
#include "check.h"

/* One GetZeroTimeStamp: the clock read @got / @s / @h; returns whether an anchor came out */
static bool anchor(struct ozzy_timeline *t, bool got, uint64_t s, uint64_t h, uint64_t *out_s, uint64_t *out_h,
		   uint64_t *seed)
{
	*out_s = s;
	*out_h = h;
	return ozzy_timeline_anchor(t, got, out_s, out_h, seed, 0, 0, 0);
}

/* The same at host time @now, with anchors @period frames and @period_host ticks apart */
static bool anchor_at(struct ozzy_timeline *t, bool got, uint64_t s, uint64_t h, uint64_t now, uint64_t *out_s,
		      uint64_t *out_h, uint64_t *seed)
{
	*out_s = s;
	*out_h = h;
	return ozzy_timeline_anchor(t, got, out_s, out_h, seed, now, 800, 400000);
}

int main(void)
{
	struct ozzy_timeline main_dev, mono;
	uint64_t s, h, seed, seed0, mono_seed;

	ozzy_timeline_init(&main_dev);
	ozzy_timeline_init(&mono);

	/* Nothing to hand out before the first real anchor: never (0, 0) */
	CHECK(!anchor(&main_dev, true, 0, 0, &s, &h, &seed), "no anchor yet, host 0");
	CHECK(!anchor(&main_dev, false, 5, 6, &s, &h, &seed), "no anchor yet, failed read");

	CHECK(anchor(&main_dev, true, 1024, 5000, &s, &h, &seed0) && s == 1024 && h == 5000, "first anchor");
	/* The engine restarting (host 0) or a failed read: the last anchor, same seed */
	CHECK(anchor(&main_dev, true, 0, 0, &s, &h, &seed) && s == 1024 && h == 5000 && seed == seed0, "kept on host 0");
	CHECK(anchor(&main_dev, false, 9, 9, &s, &h, &seed) && s == 1024 && h == 5000 && seed == seed0, "kept on a failed read");
	CHECK(anchor(&main_dev, true, 2048, 6000, &s, &h, &seed) && s == 2048 && seed == seed0, "moving on");
	/* The clock went back: the engine restarted under running IO */
	CHECK(anchor(&main_dev, true, 512, 7000, &s, &h, &seed) && s == 512 && seed != seed0, "new seed when the clock goes back");
	seed0 = seed;

	/* A mono device on the same clock has its own seed: the main device restarting leaves it */
	CHECK(anchor(&mono, true, 1024, 8000, &s, &h, &mono_seed), "mono anchor");
	ozzy_timeline_restart(&main_dev);
	CHECK(anchor(&mono, true, 1536, 8500, &s, &h, &seed) && seed == mono_seed, "main restart leaves the mono seed");
	/* The restart drops the main device's anchor at its next call, with a new seed */
	CHECK(!anchor(&main_dev, true, 0, 0, &s, &h, &seed), "anchor dropped by a restart");
	CHECK(anchor(&main_dev, true, 256, 9000, &s, &h, &seed) && s == 256 && seed != seed0, "after a restart");
	/* Going forward keeps the seed */
	seed0 = seed;
	CHECK(anchor(&main_dev, true, 300, 9100, &s, &h, &seed) && seed == seed0, "no bump going forward");

	ozzy_timeline_restart(&mono);
	CHECK(ozzy_timeline_seed(&mono) != mono_seed, "mono restart: new seed at once");

	/* No driver: anchors 800 frames and 400000 ticks apart (48 kHz on a 24 MHz host clock) */
	struct ozzy_timeline t;
	ozzy_timeline_init(&t);
	CHECK(!anchor_at(&t, false, 0, 0, 1000000, &s, &h, &seed), "nothing to move on before the first anchor");
	CHECK(anchor_at(&t, true, 8000, 1000000, 1000000, &s, &h, &seed0), "real anchor");
	CHECK(anchor_at(&t, false, 0, 0, 1399999, &s, &h, &seed) && s == 8000 && h == 1000000 && seed == seed0,
	      "within the first period: the anchor itself");
	CHECK(anchor_at(&t, true, 0, 0, 1399999, &s, &h, &seed) && s == 8000 && h == 1000000,
	      "host 0 within the period: the anchor itself");
	CHECK(anchor_at(&t, false, 0, 0, 1400000, &s, &h, &seed) && s == 8800 && h == 1400000 && seed == seed0,
	      "one period on: moved on by one");
	CHECK(anchor_at(&t, false, 0, 0, 2399999, &s, &h, &seed) && s == 10400 && h == 2200000 && h <= 2399999,
	      "3.5 periods on: three whole ones, never past now");
	CHECK(anchor_at(&t, false, 0, 0, 1400000, &s, &h, &seed) && s == 8800,
	      "moved on from the real anchor, not from the last one served");
	CHECK(anchor_at(&t, false, 0, 0, 900000, &s, &h, &seed) && s == 8000 && h == 1000000,
	      "a now before the anchor: the anchor");
	CHECK(anchor_at(&t, false, 0, 0, 1000000 + 7500 * 400000ULL, &s, &h, &seed) && s == 8000 + 7500 * 800ULL &&
	      seed == seed0, "3 s of grace: 7500 periods, same seed");

	/* The driver comes back as a new clock from 0: a real anchor behind the one served, a new seed */
	CHECK(anchor_at(&t, true, 800, 3100000000ULL, 3100000000ULL, &s, &h, &seed) && s == 800 && seed != seed0,
	      "a new engine's clock from 0: new seed");
	seed0 = seed;
	CHECK(anchor_at(&t, true, 1600, 3100400000ULL, 3100400000ULL, &s, &h, &seed) && seed == seed0, "then onwards");

	/* A rebase: the next real anchor starts a new seed even when its sample time is no earlier */
	ozzy_timeline_rebase(&t);
	CHECK(anchor_at(&t, false, 0, 0, 3100400000ULL, &s, &h, &seed) && s == 1600 && seed == seed0,
	      "a rebase keeps the anchor, and the seed until a real anchor");
	CHECK(anchor_at(&t, true, 1600, 3100400000ULL, 3100400000ULL, &s, &h, &seed) && s == 1600 && seed != seed0,
	      "rebase: new seed on an equal sample time");
	seed0 = seed;
	CHECK(anchor_at(&t, true, 2400, 3100800000ULL, 3100800000ULL, &s, &h, &seed) && seed == seed0, "rebase applied once");

	/* A restart still drops the anchor: nothing is moved on from before it */
	ozzy_timeline_rebase(&t);
	ozzy_timeline_restart(&t);
	seed0 = ozzy_timeline_seed(&t);
	CHECK(!anchor_at(&t, false, 0, 0, 3200000000ULL, &s, &h, &seed), "restart: no anchor to move on");
	CHECK(anchor_at(&t, true, 100, 3200000000ULL, 3200000000ULL, &s, &h, &seed) && seed == seed0,
	      "restart's seed covers a rebase before it");
	return CHECK_DONE("timeline");
}
