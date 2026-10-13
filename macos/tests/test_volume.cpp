/* SPDX-License-Identifier: MIT */
#include <cmath>
#include "../OzzyHAL/OzzyVolume.h"
#include "test.h"
#include "tests.h"

static bool Near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

void test_volume_endpoints()
{
	CHECK(OzzyVolumeScalarToGain(0.0f) == 0.0f);
	CHECK(OzzyVolumeScalarToGain(1.0f) == 1.0f);
	CHECK(OzzyVolumeScalarToDB(0.0f) == kOzzyVolumeMinDB);
	CHECK(OzzyVolumeScalarToDB(1.0f) == kOzzyVolumeMaxDB);
	CHECK(OzzyVolumeDBToScalar(kOzzyVolumeMinDB) == 0.0f);
	CHECK(OzzyVolumeDBToScalar(kOzzyVolumeMaxDB) == 1.0f);
}

void test_volume_clamps()
{
	CHECK(OzzyVolumeClampScalar(-0.5f) == 0.0f);
	CHECK(OzzyVolumeClampScalar(2.0f) == 1.0f);
	CHECK(OzzyVolumeClampScalar(NAN) == 0.0f);
	CHECK(OzzyVolumeDBToScalar(-200.0f) == 0.0f);
	CHECK(OzzyVolumeDBToScalar(12.0f) == 1.0f);
	CHECK(OzzyVolumeScalarToGain(3.0f) == 1.0f);
}

/* The HAL's two conversion properties must be inverses. */
void test_volume_round_trip()
{
	for (int i = 0; i <= 100; i++) {
		const float s = (float)i / 100.0f;
		CHECK(Near(OzzyVolumeDBToScalar(OzzyVolumeScalarToDB(s)), s));
	}
}

/* Equal scalar steps are equal dB steps, and the gain matches the dB. */
void test_volume_curve_is_linear_in_db()
{
	float prevGain = 0.0f;
	for (int i = 1; i <= 16; i++) {
		const float s = (float)i / 16.0f;
		CHECK(Near(OzzyVolumeScalarToDB(s), kOzzyVolumeMinDB + s * (kOzzyVolumeMaxDB - kOzzyVolumeMinDB), 1e-4f));
		const float gain = OzzyVolumeScalarToGain(s);
		CHECK(Near(20.0f * std::log10(gain), OzzyVolumeScalarToDB(s), 1e-3f));
		CHECK(gain > prevGain);
		prevGain = gain;
	}
	CHECK(Near(OzzyVolumeScalarToGain(0.5f), std::pow(10.0f, -30.0f / 20.0f)));
}

void test_volume_ramp_scales_buffer()
{
	float buf[4 * 2];
	for (int i = 0; i < 8; i++) buf[i] = 0.5f;
	OzzyApplyGainRamp(buf, 4, 2, 0.25f, 0.25f);
	for (int i = 0; i < 8; i++) CHECK(Near(buf[i], 0.125f));

	for (int i = 0; i < 8; i++) buf[i] = 1.0f;
	OzzyApplyGainRamp(buf, 4, 2, 0.0f, 1.0f);
	const float want[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
	for (int f = 0; f < 4; f++) {
		CHECK(Near(buf[2 * f], want[f]));
		CHECK(Near(buf[2 * f + 1], want[f]));
	}

	/* Unity leaves samples bit-exact. */
	float exact[3] = { 0.1f, -0.7f, 0.33333f };
	OzzyApplyGainRamp(exact, 3, 1, 1.0f, 1.0f);
	CHECK(exact[0] == 0.1f && exact[1] == -0.7f && exact[2] == 0.33333f);
}
