/* SPDX-License-Identifier: MIT */
/*
 * OzzyHAL's volume curve, output and input, (macos/OzzyHAL/OzzyVolume.h): the end points, clamping, the
 * scalar <-> dB conversions as inverses, equal scalar steps as equal dB steps, and the gain
 * ramp over a buffer.
 */

#include "macos/OzzyHAL/OzzyVolume.h"
#include "check.h"
#include <math.h>

static int near(float a, float b, float eps)
{
	return fabsf(a - b) <= eps;
}

int main(void)
{
	float buf[8], prev = 0.0f;
	static const float want[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
	float exact[3] = { 0.1f, -0.7f, 0.33333f };
	int i;

	CHECK(ozzy_volume_gain(0.0f) == 0.0f && ozzy_volume_gain(1.0f) == 1.0f, "gain at the ends");
	CHECK(ozzy_volume_scalar_to_db(0.0f) == ozzy_volume_min_db && ozzy_volume_scalar_to_db(1.0f) == ozzy_volume_max_db,
	      "dB at the ends");
	CHECK(ozzy_volume_db_to_scalar(ozzy_volume_min_db) == 0.0f && ozzy_volume_db_to_scalar(ozzy_volume_max_db) == 1.0f,
	      "scalar at the ends");

	CHECK(ozzy_volume_clamp(-0.5f) == 0.0f && ozzy_volume_clamp(2.0f) == 1.0f && ozzy_volume_clamp(NAN) == 0.0f,
	      "clamp");
	CHECK(ozzy_volume_db_to_scalar(-200.0f) == 0.0f && ozzy_volume_db_to_scalar(12.0f) == 1.0f, "dB out of range");
	CHECK(ozzy_volume_gain(3.0f) == 1.0f, "gain above 1");

	/* The HAL's two conversion properties are inverses */
	for (i = 0; i <= 100; i++) {
		float s = (float)i / 100.0f;

		CHECK(near(ozzy_volume_db_to_scalar(ozzy_volume_scalar_to_db(s)), s, 1e-5f), "round trip at %.2f", s);
	}

	/* Equal scalar steps are equal dB steps, and the gain matches the dB */
	for (i = 1; i <= 16; i++) {
		float s = (float)i / 16.0f, gain = ozzy_volume_gain(s);

		CHECK(near(ozzy_volume_scalar_to_db(s), ozzy_volume_min_db + s * (ozzy_volume_max_db - ozzy_volume_min_db),
			   1e-4f), "dB at %.4f", s);
		CHECK(near(20.0f * log10f(gain), ozzy_volume_scalar_to_db(s), 1e-3f), "gain at %.4f", s);
		CHECK(gain > prev, "gain rises at %.4f", s);
		prev = gain;
	}
	CHECK(near(ozzy_volume_gain(0.5f), powf(10.0f, -30.0f / 20.0f), 1e-5f), "half is -30 dB");

	/* A constant gain scales the buffer; a ramp ends exactly on its target */
	for (i = 0; i < 8; i++)
		buf[i] = 0.5f;
	ozzy_volume_ramp(buf, 4, 2, 0.25f, 0.25f);
	for (i = 0; i < 8; i++)
		CHECK(near(buf[i], 0.125f, 1e-6f), "constant gain, sample %d", i);
	for (i = 0; i < 8; i++)
		buf[i] = 1.0f;
	ozzy_volume_ramp(buf, 4, 2, 0.0f, 1.0f);
	for (i = 0; i < 4; i++)
		CHECK(near(buf[2 * i], want[i], 1e-6f) && near(buf[2 * i + 1], want[i], 1e-6f), "ramp, frame %d", i);
	/* Unity leaves samples bit-exact */
	ozzy_volume_ramp(exact, 3, 1, 1.0f, 1.0f);
	CHECK(exact[0] == 0.1f && exact[1] == -0.7f && exact[2] == 0.33333f, "unity changed samples");
	return CHECK_DONE("volume");
}
