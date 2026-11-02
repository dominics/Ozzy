/* SPDX-License-Identifier: MIT */
/*
 * The isochronous ring path (Tascam US-144MKII: 4 in, 4 out, both routings): every frame
 * that goes out on the wire must be the next routed ring frame, in order; captured frames
 * must be the routed decoded ones.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "fake_bus.h"
#include "check.h"
#include "common/devices/ploytec/ploytec_protocol.h"
#include "common/devices/ploytec/ploytec_codec.h"
#include "engine/core/ozzy_isoc_output.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t seed = 7;
static uint8_t sent[200000 * 12];
static uint64_t sent_frames;

static void fill(struct fake_xfer *x, void *arg)
{
	uint32_t i, k;

	if (x->isoc) {
		for (k = 0; k < x->count; k++) {
			uint8_t *b = x->buf + x->packets[k].offset;

			b[0] = 0x00;
			b[1] = 0x00;
			b[2] = 0x0C;
		}
		return;
	}
	for (i = 0; i < x->size; i++)
		x->buf[i] = (uint8_t)fake_rand(&seed);
}

/* What an OUT transfer puts on the wire, in order (packets may wrap the ring) */
static void record_out(const struct fake_xfer *x)
{
	uint32_t k, j;

	for (k = 0; k < x->count; k++) {
		for (j = 0; j < x->packets[k].length; j++)
			sent[sent_frames * 12 + j] = x->buf[(x->packets[k].offset + j) % x->size];
		sent_frames += x->packets[k].length / 12;
	}
}

/* One float through the plain codec, back as a signed 24-bit sample */
static int32_t f32_to_s24(float sample)
{
	uint8_t b[3];

	ploytec_f32_to_s24(b, sample);
	return (int32_t)((uint32_t)(b[0] | (b[1] << 8) | (b[2] << 16)) << 8) >> 8;
}

/* What the HAL hands the plain codec: NaN is silence, the rest clamps to 24 bits */
static void check_f32_clamp(void)
{
	CHECK(f32_to_s24(NAN) == 0, "NaN encodes as %d", f32_to_s24(NAN));
	CHECK(f32_to_s24(-NAN) == 0, "-NaN encodes as %d", f32_to_s24(-NAN));
	CHECK(f32_to_s24(INFINITY) == 8388607, "+inf encodes as %d", f32_to_s24(INFINITY));
	CHECK(f32_to_s24(-INFINITY) == -8388608, "-inf encodes as %d", f32_to_s24(-INFINITY));
	CHECK(f32_to_s24(1.0f) == 8388607, "1.0 encodes as %d", f32_to_s24(1.0f));
	CHECK(f32_to_s24(-1.0f) == -8388608, "-1.0 encodes as %d", f32_to_s24(-1.0f));
	CHECK(f32_to_s24(0.5f) == 4194304, "0.5 encodes as %d", f32_to_s24(0.5f));
}

/*
 * Without a ring (macOS: the HAL reads the slots), input starts as silence, a short PCM IN
 * read leaves the frames it didn't bring as silence in its slot, and a failed one the whole slot
 */
static void check_short_in(void)
{
	const struct ozzy_device *model = ozzy_device_lookup(0x0562, 0x03EB);
	static struct ozzy_shared_memory shm;
	struct fake_bus fb;
	struct fake_xfer x;
	struct ozzy_engine *e;
	unsigned int i, ins = 0, stale;

	fake_bus_init(&fb, &shm, false, NULL);
	e = ozzy_engine_init(malloc(ozzy_engine_size(model)), model);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_OUT, 0x02, 1, 156, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_IN, 0x86, 2, 512, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_FEEDBACK, 0x81, 1, 3, 4);
	ozzy_engine_attach(e, &fb.bus, &shm);
	ozzy_engine_set_anchors(e, true);
	memset(shm.audio.input_buffer, 0x77, sizeof(shm.audio.input_buffer));   /* a previous run's input */
	CHECK(ozzy_engine_start(e), "CMS start");
	for (i = 0, stale = 0; i < sizeof(shm.audio.input_buffer); i++)
		stale += shm.audio.input_buffer[i] != 0;
	CHECK(!stale, "%u bytes of the previous run's input left after start", stale);
	for (i = 0; i < 100 && ins < 2 && fake_bus_pop(&fb, &x); i++) {
		uint32_t got, b;

		if (x.pipe != 0x86) {
			fake_bus_complete(&fb, e, &x, NULL, NULL);
			continue;
		}
		memset(x.buf, 0x55, x.size);
		got = ins ? 0 : 3 * PLOYTEC_IN_FRAME_SIZE;   /* 3 frames, then a failed read */
		ozzy_engine_complete(e, x.pipe, x.xfer, ins ? -1 : 0, got);
		for (b = got, stale = 0; b < x.size; b++)
			stale += x.buf[b] != 0;
		CHECK(!stale && (!got || x.buf[0] == 0x55), "short read %u: %u stale bytes after %u", ins, stale, got);
		ins++;
	}
	CHECK(ins == 2, "%u PCM IN reads", ins);
	free(e);
}

/* Bytes of @buf (@size bytes, from @at, wrapping) that aren't @value */
static uint32_t count_not(const uint8_t *buf, uint32_t size, uint32_t at, uint32_t len, uint8_t value)
{
	uint32_t j, n = 0;

	for (j = 0; j < len; j++)
		n += buf[(at + j) % size] != value;
	return n;
}

/*
 * Where the HAL writes the frame ring (no ring platform): once an OUT transfer completes, its
 * frames are silence, and the transfers still queued keep what the HAL wrote
 */
static void check_clear_sent_isoc(void)
{
	const struct ozzy_device *model = ozzy_device_lookup(0x0562, 0x03EB);
	static struct ozzy_shared_memory shm;
	struct fake_xfer queued[8];
	struct ozzy_isoc_packet sent_packets[OZZY_MAX_ISOC_PACKETS];
	struct fake_bus fb;
	struct fake_xfer x;
	struct ozzy_engine *e;
	unsigned int i, k, nq = 0, done = 0, left = 0, kept = 0;

	fake_bus_init(&fb, &shm, false, NULL);
	e = ozzy_engine_init(malloc(ozzy_engine_size(model)), model);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_OUT, 0x02, 1, 156, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_IN, 0x86, 2, 512, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_FEEDBACK, 0x81, 1, 3, 4);
	ozzy_engine_attach(e, &fb.bus, &shm);
	ozzy_engine_set_anchors(e, true);
	CHECK(ozzy_engine_start(e), "CMS start");
	memset(shm.audio.output_buffer, 0x33, sizeof(shm.audio.output_buffer));   /* the HAL's frames */
	for (i = 0; i < 400 && done < 40 && fake_bus_pop(&fb, &x); i++) {
		if (x.pipe != 0x02) {
			fake_bus_complete(&fb, e, &x, fill, NULL);
			continue;
		}
		/* The OUT transfers queue in order: complete the oldest, check it and the rest */
		queued[nq++] = x;
		if (nq < ISOC_OUT_TRANSFERS)
			continue;
		/* Its packets as sent: the completion submits it again with new ones */
		memcpy(sent_packets, queued[0].packets, queued[0].count * sizeof(sent_packets[0]));
		fake_bus_complete(&fb, e, &queued[0], NULL, NULL);
		for (k = 0; k < queued[0].count; k++)
			left += count_not(queued[0].buf, queued[0].size, sent_packets[k].offset, sent_packets[k].length, 0);
		for (k = 0; k < nq - 1; k++) {
			kept += count_not(queued[k + 1].buf, queued[k + 1].size, queued[k + 1].packets[0].offset,
					  queued[k + 1].packets[0].length, 0x33);
			queued[k] = queued[k + 1];
		}
		nq--;
		/* The HAL writes the next lap's frames into what just went out */
		memset(shm.audio.output_buffer, 0x33, sizeof(shm.audio.output_buffer));
		done++;
	}
	CHECK(done == 40 && !left, "CMS: %u OUT transfers, %u bytes left after they went out", done, left);
	CHECK(!kept, "CMS: %u bytes of queued transfers cleared", kept);
	free(e);
}

/* The same on the Xones' own transfers (Xone:DB4, interrupt layout): only the PCM frames */
static void check_clear_sent_xone(void)
{
	const struct ozzy_device *model = ozzy_device_lookup(0x0A4A, 0xFFDB);
	static struct ozzy_shared_memory shm;
	struct fake_bus fb;
	struct fake_xfer x;
	struct ozzy_engine *e;
	unsigned int i, f, outs = 0, left = 0, framing = 0;
	static uint8_t pcm[65536];   /* per byte of a transfer: a PCM frame's */

	fake_bus_init(&fb, &shm, false, NULL);
	e = ozzy_engine_init(malloc(ozzy_engine_size(model)), model);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_OUT, 0x05, 3, 964, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_IN, 0x86, 3, 1024, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_MIDI_IN, 0x83, 3, 64, 1);
	ozzy_engine_attach(e, &fb.bus, &shm);
	ozzy_engine_set_anchors(e, true);
	CHECK(ozzy_engine_start(e), "Xone start");
	memset(shm.audio.output_buffer, 0x33, sizeof(shm.audio.output_buffer));
	for (i = 0; i < 400 && outs < 20 && fake_bus_pop(&fb, &x); i++) {
		uint32_t frames = x.size / PLOYTEC_INT_OUT_SUBPKT_SIZE * PLOYTEC_FRAMES_PER_OUT_SUBPKT;

		fake_bus_complete(&fb, e, &x, fill, NULL);
		if (x.pipe != 0x05)
			continue;
		memset(pcm, 0, sizeof(pcm));
		for (f = 0; f < frames; f++) {
			uint32_t at = ploytec_out_frame_offset(PLOYTEC_OUT_LAYOUT_INT, f);

			left += count_not(x.buf + at, PLOYTEC_OUT_FRAME_SIZE, 0, PLOYTEC_OUT_FRAME_SIZE, 0);
			memset(pcm + at, 1, PLOYTEC_OUT_FRAME_SIZE);
		}
		for (f = 0; f < x.size; f++)   /* MIDI and sync bytes */
			framing += !pcm[f] && !x.buf[f];   /* 0x33, or MIDI the engine placed since */
		memset(x.buf, 0x33, x.size);
		outs++;
	}
	CHECK(outs == 20 && !left, "Xone: %u OUT transfers, %u PCM bytes left after they went out", outs, left);
	CHECK(!framing, "Xone: %u framing bytes cleared", framing);
	free(e);
}

/*
 * The OUT queue per device: 24 ms for the CMS 600-3 (OZZY_QUIRK_DEEP_OUT_QUEUE), 12 ms for
 * the rest of the family; the safety offset and the reported queue follow it
 */
static void check_out_queue(uint16_t vid, uint16_t pid, uint32_t transfers)
{
	const struct ozzy_device *model = ozzy_device_lookup(vid, pid);
	static struct ozzy_shared_memory shm;
	struct fake_bus fb;
	struct fake_xfer x;
	struct ozzy_engine *e;
	uint32_t outs = 0;

	memset(&shm, 0, sizeof(shm));
	fake_bus_init(&fb, &shm, false, NULL);
	e = ozzy_engine_init(malloc(ozzy_engine_size(model)), model);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_OUT, 0x02, 1, 156, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_IN, 0x86, 2, 512, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_FEEDBACK, 0x81, 1, 3, 4);
	ozzy_engine_attach(e, &fb.bus, &shm);
	ozzy_engine_set_anchors(e, true);
	CHECK(ozzy_engine_start(e), "%s start", model->name);
	while (fake_bus_pop(&fb, &x))
		outs += x.pipe == 0x02;
	CHECK(outs == transfers && shm.info.pipes[OZZY_PIPE_PCM_OUT].in_flight == transfers,
	      "%s: %u OUT transfers queued, %u reported, want %u", model->name, outs,
	      shm.info.pipes[OZZY_PIPE_PCM_OUT].in_flight, transfers);
	CHECK(shm.audio.safety_offset_frames == transfers * isoc_fb_max_frames(shm.audio.sample_rate, ISOC_TRANSFER_UFRAMES),
	      "%s: safety offset %u at %u Hz", model->name, shm.audio.safety_offset_frames, shm.audio.sample_rate);
	free(e);
}

/* Feedback as the CMS sends it (48 frames a millisecond), or none once it has halted */
static void fill_feedback(struct fake_xfer *x, void *arg)
{
	bool halted = *(bool *)arg;
	uint32_t k;

	for (k = 0; x->isoc && k < x->count; k++) {
		x->buf[x->packets[k].offset] = 48;
		x->packets[k].actual = halted ? 0 : x->packets[k].length;
	}
}

/*
 * A device whose feedback goes silent (the CMS firmware halted): the bus hears of it once,
 * OZZY_FB_STALL_MS after the last valid feedback, not before, also when packet times run ahead
 * of the completions
 */
static void check_stall(uint64_t packet_lead)
{
	const struct ozzy_device *model = ozzy_device_lookup(0x0562, 0x03EB);
	static struct ozzy_shared_memory shm;
	struct fake_bus fb;
	struct fake_xfer x;
	struct ozzy_engine *e;
	uint64_t halt_at = 0, last_valid = 0, stall_at = 0;
	unsigned int i, early = 0;
	bool halted = false;

	memset(&shm, 0, sizeof(shm));
	fake_bus_init(&fb, &shm, true, NULL);
	fb.hz = 1000000;   /* microseconds */
	fb.packet_lead = packet_lead;
	e = ozzy_engine_init(malloc(ozzy_engine_size(model)), model);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_OUT, 0x02, 1, 156, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_IN, 0x86, 2, 512, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_FEEDBACK, 0x81, 1, 3, 4);
	ozzy_engine_attach(e, &fb.bus, &shm);
	CHECK(ozzy_engine_start(e), "CMS start");
	for (i = 0; i < 2000 && fake_bus_pop(&fb, &x); i++) {
		if (x.pipe == 0x81 || x.pipe == 0x02)
			fb.now += 2000;   /* 4 ms per transfer, two pipes */
		if (!halted && fb.now >= 200000) {
			halted = true;
			halt_at = fb.now;
		}
		if (x.pipe == 0x81 && !halted)
			last_valid = fb.now;
		fake_bus_complete(&fb, e, &x, x.pipe == 0x81 ? fill_feedback : NULL, &halted);
		early += fb.pcm_stalled && fb.now <= last_valid + OZZY_FB_STALL_MS * 1000;
		if (fb.pcm_stalled && !stall_at)
			stall_at = fb.now;
	}
	CHECK(halted && fb.now > halt_at + 200000, "ran %llu us", (unsigned long long)fb.now);
	CHECK(fb.pcm_stalled == 1 && !early, "stall reported %u times, %u too early", fb.pcm_stalled, early);
	CHECK(stall_at && stall_at <= last_valid + OZZY_FB_STALL_MS * 1000 + 8000, "stall reported %llu us after the last feedback",
	      (unsigned long long)(stall_at - last_valid));
	free(e);
}

int main(void)
{
	const struct ozzy_device *model = ozzy_device_lookup(0x0644, 0x8020);
	static struct ozzy_shared_memory shm;
	static uint8_t out_ring[1000 * 12], in_ring[1000 * 12];
	struct ozzy_pcm_ring ro = { out_ring, 1000, 4, 0 }, ri = { in_ring, 1000, 4, 0 };
	struct fake_bus fb;
	struct fake_xfer x;
	struct ozzy_engine *e;
	unsigned int i, ins = 0, wrong = 0, wrong_in = 0;

	check_f32_clamp();
	check_short_in();
	check_clear_sent_isoc();
	check_clear_sent_xone();
	check_out_queue(0x0562, 0x03EB, ISOC_OUT_TRANSFERS_DEEP);   /* CMS 600-3 */
	check_out_queue(0x09E8, 0x0010, ISOC_OUT_TRANSFERS);        /* EIE Pro */
	check_stall(0);
	check_stall(3000);   /* packet times a bus clock puts after the completion */
	CHECK(model && model->family == OZZY_FAMILY_PLOYTEC_ISOC, "US-144MKII in the device list");
	for (i = 0; i < sizeof(out_ring); i++)
		out_ring[i] = (uint8_t)(i * 7 + 3);
	fake_bus_init(&fb, &shm, true, NULL);
	e = ozzy_engine_init(malloc(ozzy_engine_size(model)), model);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_OUT, 0x02, 1, 1024, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_IN, 0x86, 2, 512, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_FEEDBACK, 0x81, 1, 3, 4);
	ozzy_engine_attach(e, &fb.bus, &shm);
	ozzy_engine_set_anchors(e, true);   /* as the macOS kext */
	ozzy_engine_set_setting(e, OZZY_SETTING_OUT_ROUTE_1, 1);   /* outputs 1-2 from playback 3-4 */
	ozzy_engine_set_setting(e, OZZY_SETTING_IN_ROUTE_1, 1);    /* capture 1-2 from inputs 3-4 */
	ozzy_engine_set_pcm_ring(e, OZZY_PCM_OUT, &ro);
	ozzy_engine_set_pcm_ring(e, OZZY_PCM_IN, &ri);
	CHECK(ozzy_engine_start(e), "start");
	/* The family's own defaults: 80-frame in transfers, 2 in flight, input safety what they queue */
	CHECK(ozzy_engine_has_setting(e, OZZY_SETTING_PCM_IN_QUEUE) && !ozzy_engine_has_setting(e, OZZY_SETTING_PCM_OUT_QUEUE) &&
	      !ozzy_engine_has_setting(e, OZZY_SETTING_PCM_OUT_FRAMES), "isoc: the wrong transfer settings");
	CHECK(ozzy_pcm_queue(e->settings.value[OZZY_SETTING_PCM_IN_QUEUE]) == 2 &&
	      shm.info.pipes[OZZY_PIPE_PCM_IN].in_flight == 2, "isoc: in queue %u by default, %u in flight",
	      ozzy_pcm_queue(e->settings.value[OZZY_SETTING_PCM_IN_QUEUE]), shm.info.pipes[OZZY_PIPE_PCM_IN].in_flight);
	CHECK(shm.audio.input_safety_offset_frames == 2 * 80, "isoc: input safety %u", shm.audio.input_safety_offset_frames);

	for (i = 0; i < 300 && fake_bus_pop(&fb, &x); i++) {
		if (x.isoc && !(x.pipe & 0x80))
			record_out(&x);
		ins += x.pipe == 0x86;
		fake_bus_complete(&fb, e, &x, fill, NULL);
	}
	for (i = 0; i < sent_frames; i++) {
		const uint8_t *src = out_ring + (i % 1000) * 12;

		wrong += memcmp(sent + i * 12, src + 6, 6) || memcmp(sent + i * 12 + 6, src + 6, 6);
	}
	/* The last IN transfer's frames (80, end to end in the ring) */
	for (i = 0; i < 80 && ins; i++) {
		unsigned int j = ins - 1;
		const uint8_t *pkt = (const uint8_t *)shm.audio.input_buffer +
				     (j % (shm.audio.input_ring_frames / 80)) * 80 * PLOYTEC_IN_FRAME_SIZE;
		uint32_t r = (j * 80 + i) % 1000;
		uint8_t dec[12];

		ploytec_decode_frame_4ch(dec, pkt + i * PLOYTEC_IN_FRAME_SIZE);
		wrong_in += memcmp(in_ring + r * 12, dec + 6, 6) || memcmp(in_ring + r * 12 + 6, dec + 6, 6);
	}
	CHECK(sent_frames > 10000, "%llu frames sent", (unsigned long long)sent_frames);
	CHECK(!wrong, "%u of %llu frames sent aren't the routed ring frames", wrong, (unsigned long long)sent_frames);
	CHECK(fb.progress[OZZY_PCM_OUT] >= sent_frames, "out progress %llu", (unsigned long long)fb.progress[OZZY_PCM_OUT]);
	CHECK(ins > 10 && !wrong_in, "%u IN packets, %u captured frames wrong", ins, wrong_in);
	CHECK(fb.progress[OZZY_PCM_IN] == (uint64_t)ins * 80, "in progress %llu", (unsigned long long)fb.progress[OZZY_PCM_IN]);
	free(e);
	return CHECK_DONE("isochronous rings");
}
