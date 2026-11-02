/* SPDX-License-Identifier: MIT */
/*
 * OzzyHAL's device timelines (macos/OzzyHAL/OzzyTimeline.h): the anchor served while the engine
 * has none, a new seed when the clock goes back, a restart from another thread dropping the
 * anchor at the IO thread's next call, and two timelines on one clock (the main device and a
 * mono device) restarting independently.
 */

#include "macos/OzzyHAL/OzzyTimeline.h"
#include "check.h"

/* One GetZeroTimeStamp: the clock read @got / @s / @h; returns whether an anchor came out */
static bool anchor(struct ozzy_timeline *t, bool got, uint64_t s, uint64_t h, uint64_t *out_s, uint64_t *out_h,
		   uint64_t *seed)
{
	*out_s = s;
	*out_h = h;
	return ozzy_timeline_anchor(t, got, out_s, out_h, seed);
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
	return CHECK_DONE("timeline");
}
