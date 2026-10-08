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
