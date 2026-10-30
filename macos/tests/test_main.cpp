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
	test_ring_layout_tiny_rate();
	test_codec_encode_walking_bits();
	test_codec_encode_clamps();
	test_codec_read_input_matches_shared_decoder();
	test_codec_wrap_large_sample_time();
	test_codec_nan_is_silence();
	test_codec_read_input_channel();
	test_feedback_parse();
	test_feedback_out_of_range_rejected_by_pacer();
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
	test_engine_start_queues_transfers();
	test_engine_queue_depth();
	test_engine_ready_waits_for_first_anchor();
	test_engine_restart_zeroes_anchor();
	test_engine_anchors_from_bus_frames();
	test_engine_anchor_interpolates_within_ms();
	test_engine_ignores_invalid_feedback();
	test_engine_output_takes_ring_and_zeroes();
	test_engine_bulk_in_slots();
	test_engine_short_bulk_read_keeps_slots();
	test_engine_transfer_error_fails();
	test_engine_stop_stops_resubmitting();
	test_engine_golden();
	test_engine_ignores_shm_period();
	test_poll_running_while_feedback_flows();
	test_poll_restarts_after_feedback_silence();
	test_poll_restarts_after_failure();
	test_poll_rate_request();
	test_poll_same_or_unsupported_rate_ignored();
	test_poll_refreshes_bus_frame_reference();
	test_poll_keeps_earliest_bus_frame_reference();
	test_poll_bus_frame_reference_follows_drift();
	test_engine_midi_publishes_ports_and_queues_reads();
	test_engine_midi_out_packs_both_ports();
	test_engine_midi_in_routes_ports();
	test_engine_midi_in_overflow_counts_drops();
	test_engine_midi_stopped_does_not_resubmit();
	test_engine_midi_masks_shm_indices();
	test_kext_status_underrun_is_success();
	test_ploytec_stop_stops_resubmitting();
	test_ploytec_only_aborted_transfer_not_resubmitted();
	test_parser_channel_messages();
	test_parser_running_status();
	test_parser_realtime_inside_message();
	test_parser_system_common_clears_running_status();
	test_parser_stray_data_dropped();
	test_parser_sysex_short();
	test_parser_sysex_long_chunks();
	test_parser_unterminated_sysex_flushed_by_status();
	test_shm_create_sets_header_and_mode();
	test_shm_create_refuses_live_replaces_stale();
	test_shm_destroy_keeps_replacement();
	test_shm_clear_stale_removes_only_dead_own_segment();
	test_handover_starts_only_when_front_and_present();
	test_handover_yields_on_switch_and_login_window();
	test_handover_unplug_and_replug();
	test_handover_busy_falls_back_to_slow_retries();
	test_handover_slow_retry_stops_when_front_lost();
	test_handover_retry_timer_after_losing_front_does_nothing();
	test_handover_failed_start_and_session_gone();
	test_volume_endpoints();
	test_volume_clamps();
	test_volume_round_trip();
	test_volume_curve_is_linear_in_db();
	test_volume_ramp_scales_buffer();
	test_channels_names();
	test_channels_out_of_range();
	test_channels_roles();
	test_channels_stereo_pair();
	test_devices_count();
	test_devices_lookup();
	test_devices_mono_channels();
	if (test_failures) {
		std::fprintf(stderr, "%d failure(s)\n", test_failures);
		return 1;
	}
	std::printf("macos tests passed\n");
	return 0;
}
