/* SPDX-License-Identifier: MIT */
/*
 * OzzyHAL's channel names and stereo pairs (macos/OzzyHAL/OzzyChannels.h): the CMS 600-3's
 * names and roles by 1-based element, nothing out of range or for unlisted models, and the
 * stereo pair's checks, passthrough, fallback and packing, the CMS 600-3's MON and AUX mono
 * inputs and their device names, and reading a mono input's channel.
 * Each channel map's key is a device-table model with that many channels.
 */

#include "macos/OzzyHAL/OzzyChannels.h"
#include "check.h"

/* A codec's reader for the test: sample (frame f of the ring, channel c) = f * 100 + c, from 1 */
static void fake_read(float *dst, const uint8_t *ring, uint64_t sample_time, uint32_t frame_count, uint32_t ring_frames)
{
	uint32_t k, c, channels = ring[0];

	for (k = 0; k < frame_count; k++)
		for (c = 0; c < channels; c++)
			dst[k * channels + c] = (float)((sample_time + k) % ring_frames * 100 + c + 1);
}

/* MON and AUX read their channel of the device's input across chunks and the ring's wrap, and
 * silence where there is no such channel or nothing to read */
static void check_mono_read(void)
{
	uint8_t ring[1] = { 4 };   /* fake_read's channel count */
	float out[10], scratch[3 * OZZY_MONO_MAX_CHANNELS];
	const uint32_t ring_frames = 64;
	uint32_t c, f, bad[] = { 0, 5 };
	bool ok;

	for (c = 3; c <= 4; c++) {
		ozzy_mono_read(fake_read, ring, ring_frames, 4, c, ring_frames - 4, 10, out, scratch, 3);
		for (ok = true, f = 0; f < 10; f++)
			ok &= out[f] == (float)((ring_frames - 4 + f) % ring_frames * 100 + c);
		CHECK(ok, "channel %u over the wrap, in chunks of 3", c);
	}
	for (c = 0; c < 2; c++) {
		for (f = 0; f < 10; f++) out[f] = 1.0f;
		ozzy_mono_read(fake_read, ring, ring_frames, 4, bad[c], 0, 10, out, scratch, 3);
		for (ok = true, f = 0; f < 10; f++) ok &= out[f] == 0.0f;
		CHECK(ok, "channel %u of 4 is silence", bad[c]);
	}
	for (f = 0; f < 10; f++) out[f] = 1.0f;
	ozzy_mono_read(NULL, ring, ring_frames, 4, 3, 0, 10, out, scratch, 3);
	ozzy_mono_read(fake_read, NULL, ring_frames, 4, 3, 0, 5, out + 5, scratch, 3);
	for (ok = true, f = 0; f < 10; f++) ok &= out[f] == 0.0f;
	CHECK(ok, "silence without a reader or a ring");
	ozzy_mono_read(fake_read, ring, ring_frames, OZZY_MONO_MAX_CHANNELS + 1, 3, 0, 10, out, scratch, 3);
	CHECK(out[0] == 0.0f, "silence past the decode buffer's width");
}

int main(void)
{
	static const char *const in[] = { "Master L", "Master R", "AUX", "MON" };
	static const char *const out[] = { "Ch 5-6 L", "Ch 5-6 R", "Ch 7-8 L", "Ch 7-8 R" };
	const struct ozzy_device *cms = ozzy_device_lookup(0x0562, 0x03EB);
	const struct ozzy_device *db4 = ozzy_device_lookup(0x0A4A, 0xFFDB);
	struct ozzy_stereo_pair p, a = { 1, 2 }, b = { 4, 4 }, c = { 3, 1 }, d = { 0, 2 }, e = { 1, 5 }, f = { 7, 8 };
	unsigned int i;
	uint32_t el;
	int dir;

	CHECK(cms && db4, "CMS 600-3 and Xone:DB4 in the device list");
	for (el = 1; el <= 4; el++) {
		const struct ozzy_channel_info *ci = ozzy_channel(cms, false, el), *co = ozzy_channel(cms, true, el);

		CHECK(ci && strcmp(ci->name, in[el - 1]) == 0, "input %u", el);
		CHECK(co && strcmp(co->name, out[el - 1]) == 0, "output %u", el);
	}
	for (dir = 0; dir < 2; dir++) {
		CHECK(!ozzy_channel(cms, dir, 0) && !ozzy_channel(cms, dir, 5) && !ozzy_channel(cms, dir, 0xFFFFFFFF),
		      "out of range, %s", dir ? "output" : "input");
		/* Only the first pair of each direction is a speaker pair */
		CHECK(ozzy_channel(cms, dir, 1)->role == OZZY_CHANNEL_LEFT && ozzy_channel(cms, dir, 2)->role == OZZY_CHANNEL_RIGHT &&
		      ozzy_channel(cms, dir, 3)->role == OZZY_CHANNEL_DISCRETE &&
		      ozzy_channel(cms, dir, 4)->role == OZZY_CHANNEL_DISCRETE, "roles, %s", dir ? "output" : "input");
	}
	CHECK(!ozzy_channel(db4, false, 1) && !ozzy_channel(NULL, true, 1), "names for an unlisted model");

	for (i = 0; i < sizeof(ozzy_channel_maps) / sizeof(ozzy_channel_maps[0]); i++) {
		const struct ozzy_channel_map *m = &ozzy_channel_maps[i];
		const struct ozzy_device *model = NULL;
		unsigned int k;

		for (k = 0; k < OZZY_NUM_DEVICES; k++)
			if (strcmp(ozzy_devices[k].key, m->key) == 0)
				model = &ozzy_devices[k];
		CHECK(model && model->in_channels == m->inputs && model->out_channels == m->outputs,
		      "channel map %s: not a device-table model with %u in / %u out", m->key, m->inputs, m->outputs);
	}

	{
		const struct ozzy_mono_input *mono[OZZY_MAX_MONO_INPUTS];

		CHECK(ozzy_mono_inputs_for(cms, mono) == 2 && strcmp(mono[0]->name, "MON") == 0 && mono[0]->channel == 4 &&
		      strcmp(mono[1]->name, "AUX") == 0 && mono[1]->channel == 3, "CMS 600-3: MON on 4, AUX on 3");
		CHECK(strcmp(mono[0]->uid_suffix, mono[1]->uid_suffix) != 0, "MON and AUX share a UID");
		CHECK(strcmp(mono[0]->device_name, "CMS 600-3 MON") == 0 && strcmp(mono[1]->device_name, "CMS 600-3 AUX") == 0,
		      "device names %s, %s", mono[0]->device_name, mono[1]->device_name);
		CHECK(ozzy_mono_inputs_for(db4, mono) == 0 && ozzy_mono_inputs_for(NULL, mono) == 0, "mono inputs elsewhere");
	}
	for (i = 0; i < sizeof(ozzy_mono_inputs) / sizeof(ozzy_mono_inputs[0]); i++) {
		const struct ozzy_mono_input *m = &ozzy_mono_inputs[i];
		unsigned int k, found = 0;

		for (k = 0; k < OZZY_NUM_DEVICES; k++)
			found |= strcmp(ozzy_devices[k].key, m->key) == 0 && m->channel >= 1 && m->channel <= ozzy_devices[k].in_channels;
		CHECK(found, "mono input %s %s: not a channel of a device-table model", m->key, m->name);
	}

	CHECK(ozzy_stereo_pair_valid(a, 4) && ozzy_stereo_pair_valid(b, 4) && ozzy_stereo_pair_valid(c, 4), "valid pairs");
	CHECK(!ozzy_stereo_pair_valid(d, 4) && !ozzy_stereo_pair_valid(e, 4), "invalid pairs");
	CHECK(ozzy_stereo_pair_valid(f, 8), "7-8 of 8");
	/* A valid pair stays as it is */
	p = ozzy_stereo_pair_for((struct ozzy_stereo_pair){ 3, 4 }, 4);
	CHECK(p.left == 3 && p.right == 4, "valid pair %u,%u", p.left, p.right);
	/* A pair stored for an 8-channel device falls back to 1-2 on a 4-channel one */
	p = ozzy_stereo_pair_for(f, 4);
	CHECK(p.left == 1 && p.right == 2, "fallback %u,%u", p.left, p.right);
	p = ozzy_stereo_pair_unpack(ozzy_stereo_pair_pack((struct ozzy_stereo_pair){ 4, 3 }));
	CHECK(p.left == 4 && p.right == 3, "pack %u,%u", p.left, p.right);
	check_mono_read();
	return CHECK_DONE("channels");
}
