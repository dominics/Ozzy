/* SPDX-License-Identifier: MIT */
/*
 * OzzyHAL's channel names and stereo pairs (macos/OzzyHAL/OzzyChannels.h): the CMS 600-3's
 * names and roles by 1-based element, nothing out of range or for unlisted models, and the
 * stereo pair's checks, fallback and packing, and the CMS 600-3's MON and AUX mono inputs.
 * Every channel map's key is a device-table model with that many channels.
 */

#include "macos/OzzyHAL/OzzyChannels.h"
#include "check.h"

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
	/* A pair stored for an 8-channel device falls back to 1-2 on a 4-channel one */
	p = ozzy_stereo_pair_for(f, 4);
	CHECK(p.left == 1 && p.right == 2, "fallback %u,%u", p.left, p.right);
	p = ozzy_stereo_pair_unpack(ozzy_stereo_pair_pack((struct ozzy_stereo_pair){ 4, 3 }));
	CHECK(p.left == 4 && p.right == 3, "pack %u,%u", p.left, p.right);
	return CHECK_DONE("channels");
}
