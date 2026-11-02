/* SPDX-License-Identifier: MIT */
/*
 * The output volume curve of OzzyHAL's volume control. The scalar that the menu bar slider and
 * the volume keys move is linear in decibels across [OZZY_VOLUME_MIN_DB, OZZY_VOLUME_MAX_DB],
 * so each step changes the level by the same amount; scalar 0 is silence rather than
 * OZZY_VOLUME_MIN_DB. C, usable from C++ (OzzyHAL) and the host tests (tests/test_volume.c).
 */

#ifndef OZZY_VOLUME_H
#define OZZY_VOLUME_H

#include <math.h>
#include <stdint.h>

static const float ozzy_volume_min_db = -60.0f;
static const float ozzy_volume_max_db = 0.0f;

/* @scalar clamped to [0, 1]; NaN reads as 0 */
static inline float ozzy_volume_clamp(float scalar)
{
	if (!(scalar > 0.0f))
		return 0.0f;
	return scalar < 1.0f ? scalar : 1.0f;
}

static inline float ozzy_volume_scalar_to_db(float scalar)
{
	return ozzy_volume_min_db + ozzy_volume_clamp(scalar) * (ozzy_volume_max_db - ozzy_volume_min_db);
}

static inline float ozzy_volume_db_to_scalar(float db)
{
	return ozzy_volume_clamp((db - ozzy_volume_min_db) / (ozzy_volume_max_db - ozzy_volume_min_db));
}

/* The linear amplitude for @scalar (0 for silence) */
static inline float ozzy_volume_gain(float scalar)
{
	scalar = ozzy_volume_clamp(scalar);
	if (scalar == 0.0f)
		return 0.0f;
	return powf(10.0f, ozzy_volume_scalar_to_db(scalar) / 20.0f);
}

/*
 * ozzy_volume_ramp - Scale @frames interleaved frames of @channels samples in place, ramping
 * linearly from gain @from to gain @to across them so a change doesn't click. The last frame
 * gets exactly @to. Nothing to do at unity.
 */
static inline void ozzy_volume_ramp(float *samples, uint32_t frames, uint32_t channels, float from, float to)
{
	uint32_t f, c;

	if (from == 1.0f && to == 1.0f)
		return;
	for (f = 0; f < frames; f++) {
		float g = f + 1 == frames ? to : from + (to - from) * (float)(f + 1) / (float)frames;

		for (c = 0; c < channels; c++)
			samples[f * channels + c] *= g;
	}
}

#endif
