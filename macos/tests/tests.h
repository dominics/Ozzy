/* SPDX-License-Identifier: MIT */
#pragma once

void test_golden_pacer();
void test_ring_layout();
void test_ring_take_output_wraps_and_zeroes();
void test_ring_take_output_large_position();
void test_ring_input_slot();
void test_codec_encode_walking_bits();
void test_codec_encode_clamps();
void test_codec_read_input_matches_shared_decoder();
void test_codec_wrap_large_sample_time();
void test_bus_isoch_defaults_unsupported();
void test_bus_profile_defaults_zero();
void test_engine_base_defaults();
void test_engine_profile();
void test_engine_out_xfers_clamped();
void test_engine_factory();
void test_engine_init_sequence();
void test_engine_init_rate_mismatch_fails();
void test_engine_init_control_failure_fails();
void test_engine_publishes_config();
void test_engine_safety_follows_queue();
