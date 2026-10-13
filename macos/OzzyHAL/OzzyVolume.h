/* SPDX-License-Identifier: MIT */
#pragma once
#include <cmath>
#include <cstdint>

/* Output volume curve for the HAL's volume control. The scalar the menu bar
 * slider and volume keys move is linear in decibels across
 * [kOzzyVolumeMinDB, kOzzyVolumeMaxDB], so each step changes the level by the
 * same amount. Scalar 0 is silence rather than kOzzyVolumeMinDB. */
constexpr float kOzzyVolumeMinDB = -60.0f;
constexpr float kOzzyVolumeMaxDB = 0.0f;

/* Clamps to [0, 1]; NaN reads as 0. */
inline float OzzyVolumeClampScalar(float scalar)
{
	if (!(scalar > 0.0f)) return 0.0f;
	return scalar < 1.0f ? scalar : 1.0f;
}

inline float OzzyVolumeScalarToDB(float scalar)
{
	return kOzzyVolumeMinDB + OzzyVolumeClampScalar(scalar) * (kOzzyVolumeMaxDB - kOzzyVolumeMinDB);
}

inline float OzzyVolumeDBToScalar(float db)
{
	return OzzyVolumeClampScalar((db - kOzzyVolumeMinDB) / (kOzzyVolumeMaxDB - kOzzyVolumeMinDB));
}

/* Linear amplitude for a scalar. */
inline float OzzyVolumeScalarToGain(float scalar)
{
	scalar = OzzyVolumeClampScalar(scalar);
	if (scalar == 0.0f) return 0.0f;
	return std::pow(10.0f, OzzyVolumeScalarToDB(scalar) / 20.0f);
}

/* Scales interleaved samples in place, ramping linearly from gain `from` to
 * gain `to` across the buffer so a volume change does not click. The last
 * frame gets exactly `to`. */
inline void OzzyApplyGainRamp(float* samples, uint32_t frames, uint32_t channels, float from, float to)
{
	if (from == 1.0f && to == 1.0f) return;
	for (uint32_t f = 0; f < frames; f++) {
		const float g = from + (to - from) * (float)(f + 1) / (float)frames;
		for (uint32_t c = 0; c < channels; c++)
			samples[f * channels + c] *= g;
	}
}
