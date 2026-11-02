/* SPDX-License-Identifier: MIT */
/*
 * What a device's channels carry, per device-table model (common/ozzy_devices.def key), for
 * OzzyHAL's channel names (kAudioObjectPropertyElementName), its preferred channel layout and
 * its preferred stereo pair, and the mono input devices it publishes next to a model's main
 * device. Models not listed here keep unnamed, unlabelled channels. C,
 * usable from C++ (OzzyHAL) and the host tests (tests/test_channels.c).
 */

#ifndef OZZY_CHANNELS_H
#define OZZY_CHANNELS_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "../../common/ozzy_devices.h"

/* Discrete channels are labelled by position rather than as a speaker */
enum ozzy_channel_role {
	OZZY_CHANNEL_LEFT,
	OZZY_CHANNEL_RIGHT,
	OZZY_CHANNEL_DISCRETE,
};

struct ozzy_channel_info {
	const char *name;
	enum ozzy_channel_role role;
};

struct ozzy_channel_map {
	const char *key;                          /* struct ozzy_device.key */
	uint8_t inputs, outputs;
	const struct ozzy_channel_info *in, *out;
};

/*
 * Dynacord CMS 600-3, from the mixer's manual: inputs 1-2 are the master bus (pre master
 * fader, pre GEQ), 3 the AUX send, 4 the pre-fader MON send. Outputs 1-2 return on stereo
 * strip 5-6, 3-4 on stereo strip 7-8.
 */
static const struct ozzy_channel_info ozzy_cms600_inputs[] = {
	{ "Master L", OZZY_CHANNEL_LEFT },
	{ "Master R", OZZY_CHANNEL_RIGHT },
	{ "AUX", OZZY_CHANNEL_DISCRETE },
	{ "MON", OZZY_CHANNEL_DISCRETE },
};

static const struct ozzy_channel_info ozzy_cms600_outputs[] = {
	{ "Ch 5-6 L", OZZY_CHANNEL_LEFT },
	{ "Ch 5-6 R", OZZY_CHANNEL_RIGHT },
	{ "Ch 7-8 L", OZZY_CHANNEL_DISCRETE },
	{ "Ch 7-8 R", OZZY_CHANNEL_DISCRETE },
};

static const struct ozzy_channel_map ozzy_channel_maps[] = {
	{ "DynacordCMS600", 4, 4, ozzy_cms600_inputs, ozzy_cms600_outputs },
};

/* The channel map for @model, or NULL */
static inline const struct ozzy_channel_map *ozzy_channel_map_for(const struct ozzy_device *model)
{
	unsigned int i;

	for (i = 0; model && i < sizeof(ozzy_channel_maps) / sizeof(ozzy_channel_maps[0]); i++)
		if (strcmp(ozzy_channel_maps[i].key, model->key) == 0)
			return &ozzy_channel_maps[i];
	return NULL;
}

/* @model's channel at 1-based @element of a direction, or NULL if it has none */
static inline const struct ozzy_channel_info *ozzy_channel(const struct ozzy_device *model, bool output,
							   uint32_t element)
{
	const struct ozzy_channel_map *m = ozzy_channel_map_for(model);

	if (!m || element < 1 || element > (output ? m->outputs : m->inputs))
		return NULL;
	return output ? &m->out[element - 1] : &m->in[element - 1];
}

/*
 * A one-channel input device OzzyHAL publishes next to a model's main device, for apps that only
 * record channels 1-2: the CMS 600-3's AUX and MON sends. It shares the main device's engine,
 * clock and rate; its UID is the main device's with @uid_suffix.
 */
struct ozzy_mono_input {
	const char *key;            /* struct ozzy_device.key */
	const char *name;           /* after the main device's name, e.g. "Dynacord CMS 600-3 MON" */
	const char *uid_suffix;
	uint8_t channel;            /* the main device's 1-based input channel */
};

#define OZZY_MAX_MONO_INPUTS 2   /* per model */

static const struct ozzy_mono_input ozzy_mono_inputs[] = {
	{ "DynacordCMS600", "MON", ":MON", 4 },
	{ "DynacordCMS600", "AUX", ":AUX", 3 },
};

/* @model's mono input devices into @out (OZZY_MAX_MONO_INPUTS); returns how many */
static inline unsigned int ozzy_mono_inputs_for(const struct ozzy_device *model,
						const struct ozzy_mono_input *out[OZZY_MAX_MONO_INPUTS])
{
	unsigned int i, n = 0;

	for (i = 0; model && i < sizeof(ozzy_mono_inputs) / sizeof(ozzy_mono_inputs[0]); i++)
		if (strcmp(ozzy_mono_inputs[i].key, model->key) == 0 && n < OZZY_MAX_MONO_INPUTS &&
		    ozzy_mono_inputs[i].channel >= 1 && ozzy_mono_inputs[i].channel <= model->in_channels)
			out[n++] = &ozzy_mono_inputs[i];
	return n;
}

/* The two 1-based channels a stereo client plays to or records from */
struct ozzy_stereo_pair {
	uint32_t left, right;
};

static inline bool ozzy_stereo_pair_valid(struct ozzy_stereo_pair p, uint32_t channels)
{
	return p.left >= 1 && p.left <= channels && p.right >= 1 && p.right <= channels;
}

/* @p if it fits @channels, else channels 1-2 */
static inline struct ozzy_stereo_pair ozzy_stereo_pair_for(struct ozzy_stereo_pair p, uint32_t channels)
{
	struct ozzy_stereo_pair first = { 1, 2 };

	return ozzy_stereo_pair_valid(p, channels) ? p : first;
}

/* A pair in one word, so it can be swapped atomically */
static inline uint32_t ozzy_stereo_pair_pack(struct ozzy_stereo_pair p)
{
	return (p.left & 0xFFFF) << 16 | (p.right & 0xFFFF);
}

static inline struct ozzy_stereo_pair ozzy_stereo_pair_unpack(uint32_t packed)
{
	struct ozzy_stereo_pair p = { packed >> 16, packed & 0xFFFF };

	return p;
}

#endif
