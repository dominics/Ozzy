/* SPDX-License-Identifier: MIT */
/*
 * A start that the device doesn't take fails (Dynacord CMS 600-3 on the fake bus):
 *
 *   - the rate read back after SET_CUR is not the rate asked for;
 *   - a control request keeps failing (each is asked up to 10 times);
 *
 * and neither leaves transfers or the timer running. A control request that fails a few
 * times and then answers still starts.
 */

#include "fake_bus.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>

static struct ozzy_shared_memory shm;

/* An engine for the CMS 600-3 on its endpoints, to start at @rate */
static struct ozzy_engine *engine(struct fake_bus *fb, uint32_t rate)
{
	const struct ozzy_device *model = ozzy_device_lookup(0x0562, 0x03EB);
	struct ozzy_engine *e;

	memset(&shm, 0, sizeof(shm));
	fake_bus_init(fb, &shm, false, NULL);
	e = ozzy_engine_init(malloc(ozzy_engine_size(model)), model);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_OUT, 0x02, 1, 1024, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_IN, 0x86, 2, 512, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_FEEDBACK, 0x81, 1, 3, 4);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_MIDI_OUT, 0x04, 2, 512, 0);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_MIDI_IN, 0x83, 2, 512, 0);
	ozzy_engine_attach(e, &fb->bus, &shm);
	ozzy_engine_set_rate(e, rate);
	return e;
}

/* Nothing streams after a start that failed */
static void check_idle(struct fake_bus *fb, const char *what)
{
	CHECK(fb->head == fb->tail, "%s: %u transfers left queued", what, fb->tail - fb->head);
	CHECK(!fb->timer_on, "%s: timer left running", what);
}

static void stop(struct ozzy_engine *e)
{
	ozzy_engine_stop(e);
	free(e);
}

int main(void)
{
	struct fake_bus fb;
	struct ozzy_engine *e;

	/* The baseline: the fake device takes the rate */
	e = engine(&fb, 48000);
	CHECK(ozzy_engine_start(e), "start at 48000");
	stop(e);

	/* The device runs at another rate than the one set */
	e = engine(&fb, 48000);
	fb.rate_reads = 44100;
	CHECK(!ozzy_engine_start(e), "a start whose rate reads back 44100 instead of 48000 succeeded");
	check_idle(&fb, "rate read back");
	stop(e);

	/* The first control request never answers */
	e = engine(&fb, 48000);
	fb.control_fails = 1000;
	CHECK(!ozzy_engine_start(e), "a start whose control requests all failed succeeded");
	CHECK(fb.controls == 10, "a failed request was asked %u times, not 10", fb.controls);
	check_idle(&fb, "control failure");
	stop(e);

	/* One that fails a few times, then answers */
	e = engine(&fb, 48000);
	fb.control_fails = 3;
	CHECK(ozzy_engine_start(e), "a start whose first request failed 3 times failed");
	stop(e);

	return CHECK_DONE("start failures");
}
