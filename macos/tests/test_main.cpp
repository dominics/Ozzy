/* SPDX-License-Identifier: MIT */
#include <cstdio>
#include "test.h"
#include "tests.h"

int test_failures;

int main()
{
	test_golden_pacer();
	test_ring_layout();
	test_ring_take_output_wraps_and_zeroes();
	test_ring_take_output_large_position();
	test_ring_input_slot();
	test_codec_encode_walking_bits();
	test_codec_encode_clamps();
	test_codec_read_input_matches_shared_decoder();
	test_codec_wrap_large_sample_time();
	test_bus_isoch_defaults_unsupported();
	test_bus_profile_defaults_zero();
	test_engine_base_defaults();
	test_engine_profile();
	test_engine_out_xfers_clamped();
	test_engine_factory();
	test_engine_init_sequence();
	test_engine_init_rate_mismatch_fails();
	test_engine_init_control_failure_fails();
	test_engine_publishes_config();
	test_engine_safety_follows_queue();
	if (test_failures) {
		std::fprintf(stderr, "%d failure(s)\n", test_failures);
		return 1;
	}
	std::printf("macos tests passed\n");
	return 0;
}
