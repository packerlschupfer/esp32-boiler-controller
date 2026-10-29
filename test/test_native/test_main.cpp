/**
 * @file test_main.cpp
 * @brief Main test runner for native tests
 */

#include <unity.h>

// Forward declare all test functions
void test_float_to_temperature_conversion();
void test_temperature_to_float_conversion();
void test_temperature_addition();
void test_temperature_subtraction();
void test_temperature_comparison();
void test_temperature_formatting();
void test_invalid_temperature();
void test_temperature_edge_cases();
void test_temperature_difference();

// PID fixed-point gain conversion tests
void test_pid_gain_conversion_scales_by_1000();
void test_pid_gain_conversion_rejects_invalid();
void test_pid_scaled_gain_commands_off_above_target();
void test_pid_scaled_gain_commands_full_below_target();
void test_pid_adjustment_clamp_keeps_sign();
void test_pid_output_limit_matches_power_saturation();

// Heating curve (weather-compensated heating target)
void test_heating_curve_matches_reference_formula();
void test_heating_curve_outside_part_not_ten_times_too_small();
void test_heating_curve_clamps_to_limits();

// Space heating policy (weather mode outside threshold hysteresis)
void test_heating_diag_payload_is_the_agreed_schema();
void test_heating_diag_fits_the_mqtt_payload_limit();
void test_heating_diag_refuses_a_too_small_buffer_instead_of_truncating();
void test_heating_diag_block_reasons_are_distinct_strings();
void test_heating_diag_room_mode_and_negative_raw_serialise();
void test_heating_diag_differs_detects_every_field();
void test_heating_diag_reports_a_substituted_room_reading_as_such();

// Room temperature fallback (hold the last good reading)
void test_room_fallback_holds_the_last_reading_through_a_short_dropout();
void test_room_fallback_expires_the_hold_and_falls_back_to_target();
void test_room_fallback_with_nothing_remembered_uses_target_then_nominal();
void test_room_fallback_survives_the_millis_rollover();
void test_room_fallback_recording_a_new_reading_refreshes_the_age();
void test_room_fallback_never_returns_an_invalid_marker();

// Frost protection
void test_frost_is_inert_in_a_working_system();
void test_frost_triggers_on_either_reading();
void test_frost_holds_through_the_hysteresis_band();
void test_frost_never_triggers_on_an_invalid_reading();
void test_frost_releases_rather_than_firing_blind();
void test_frost_one_cold_sensor_holds_while_the_other_is_warm();
void test_frost_thresholds_are_the_agreed_numbers();
void test_frost_a_stale_reading_never_fires_the_burner();
void test_frost_is_one_latch_over_all_three_sensors();
void test_frost_room_trigger_cannot_fire_in_a_lived_in_house();
void test_frost_cold_mask_describes_every_combination();

// Room sensor read diagnostics
void test_room_diag_healthy_payload_is_tiny_and_has_no_error_keys();
void test_room_diag_reports_the_contention_hypothesis();
void test_room_diag_age_is_seconds_and_omitted_when_never_read();
void test_room_diag_worst_case_fits_the_mqtt_payload_limit();
void test_room_diag_refuses_a_too_small_buffer_instead_of_truncating();
void test_room_diag_differs_ignores_age_and_the_success_count();
void test_room_diag_key_table_matches_the_state_array();
void test_sensor_offset_map_places_each_field_on_its_channel();
void test_sensor_offset_map_outside_is_ch3_not_ch6();
void test_sensor_offset_map_never_offsets_the_pressure_channel();
void test_sensor_offset_map_leaves_tank_top_at_zero();
void test_sensor_offset_map_has_no_colliding_or_dropped_fields();
void test_sensor_offset_map_all_zero_in_all_zero_out();
void test_sensor_error_bit_single_failure_does_not_raise();
void test_sensor_error_bit_raises_only_after_the_confirm_window();
void test_sensor_error_bit_recovery_inside_the_window_never_raises();
void test_sensor_error_bit_clears_on_recovery_when_owned();
void test_sensor_error_bit_never_clears_a_bit_owned_by_the_fallback();
void test_sensor_error_bit_raise_is_idempotent_and_outranks_clear();
void test_sensor_error_bit_full_latch_regression_sequence();
void test_param_save_explicit_request_is_never_delayed();
void test_param_save_idle_never_saves();
void test_param_save_waits_for_the_debounce_window();
void test_param_save_coalesces_a_burst_of_clicks();
void test_param_save_survives_the_millis_wrap();
void test_space_heating_outside_threshold_start_and_stop_limits();
void test_space_heating_outside_noise_does_not_toggle();
void test_invalid_count_steps_once_per_sample_not_once_per_caller();
void test_invalid_count_floor_under_uniform_polling();
void test_invalid_count_on_the_real_mb8art_schedule_is_eight_to_ten_seconds();
void test_invalid_count_still_escalates_when_the_sensor_is_completely_dead();
void test_invalid_count_rate_limit_survives_the_millis_rollover();
void test_invalid_count_first_observation_is_never_rate_limited();
void test_bus_release_window_is_clamped_and_capped();
void test_bus_release_refuses_while_the_boiler_is_busy();
void test_bus_release_refuses_while_a_pump_is_dissipating_heat();
void test_bus_release_refuses_mid_sequence_and_under_a_latched_estop();
void test_bus_release_extending_a_window_never_shortens_it();
void test_bus_release_firing_mask_tracks_the_relay_indices();
void test_bus_release_expiry_survives_the_millis_rollover();
void test_bus_release_seconds_left_rounds_up_and_never_contradicts_released();
void test_bus_release_safe_mask_clears_firing_and_keeps_pumps();
void test_space_heating_room_backstop_is_asymmetric();
void test_space_heating_unknown_room_must_not_license_heating();
void test_space_heating_target_substitution_would_have_reopened_the_hole();
void test_space_heating_weather_decision_outside_gate_is_reported_first();
void test_space_heating_weather_decision_heats_and_blocks_on_the_room();
void test_space_heating_no_room_target_keeps_weather_only_control();

// Error context (compact boiler/error/context JSON)
void test_error_context_worst_case_fits_mqtt_payload();
void test_error_context_escapes_and_null_sensors();
void test_error_context_small_buffer_shortens_description();

// OTA validation policy (confirm or roll back an updated image)
void test_ota_validation_waits_for_min_uptime();
void test_ota_validation_needs_sensors_only();
void test_ota_validation_rolls_back_after_max_wait();
void test_ota_validation_never_reboots_a_healthy_controller_over_a_dead_lan();

// Sensor failure confirmation (burner stop on persistently missing sensors)
void test_sensor_failure_single_glitch_does_not_stop();
void test_sensor_failure_persistent_with_demand_stops();
void test_sensor_failure_without_demand_never_stops();
void test_sensor_failure_millis_wrap();

// Autotune peak/trough tracking (lagging plant)
void test_relay_extrema_peaks_include_post_switch_overshoot();
void test_relay_extrema_troughs_include_post_switch_undershoot();
void test_relay_extrema_ignores_cold_start_phase();
void test_relay_extrema_extreme_times_are_after_switch();

// Burner transition policy (explicit disable, bounded mode switch)
void test_policy_explicit_disable_stops_running_mode_only();
void test_policy_boiler_disable_stops_any_mode();
void test_policy_heating_wanted_respects_enable_and_override();
void test_policy_heating_wanted_room_mode();
void test_policy_heating_wanted_weather_mode();
void test_policy_heating_wanted_weather_mode_restart_limit_boundaries();
void test_policy_heating_wanted_room_mode_start_ignores_hysteresis();
void test_policy_mode_switch_wait_is_bounded();
void test_policy_mode_revert_requires_on_bit();
void test_policy_mode_switch_exit_records_power_level();

// Stage C policies (power level fault escalation, water limit consistency)
void test_power_fault_escalates_on_third_fault_in_window();
void test_power_fault_window_restarts_after_ten_minutes();
void test_water_limits_valid_only_when_low_below_high();
void test_water_charge_latch_resumes_only_while_latched();

// Autotune relay settings (amplitude, hysteresis)
void test_autotune_amplitude_setting_used_within_range();
void test_autotune_hysteresis_setting_used_within_range();
void test_autotune_amplitude_vs_two_stage_swing();

// Emergency stop release (MQTT emergency_reset)
void test_emergency_release_when_causes_cleared();
void test_emergency_release_not_active();
void test_emergency_release_refused_while_hot();
void test_emergency_release_refused_on_sensor_or_system_errors();
void test_emergency_stop_onset_once_per_latch();
void test_emergency_dissipation_until_boiler_cooled();
void test_emergency_dissipation_without_usable_output();
void test_emergency_sensor_recovery_releases_only_stale_sensor_stop();
void test_emergency_cause_merge_while_latched();
void test_emergency_safety_split_matches_the_latched_evaluation();
void test_emergency_safety_refuses_a_re_enable_over_a_hot_boiler();
void test_emergency_dissipation_continues_after_release_until_cooled();

// Burner transitions (state machine scenarios through BurnerTransitions::step)
void test_bsm_step_idle_without_demand_skips_safety_check();
void test_bsm_step_stale_demand_never_starts_burner();
void test_bsm_step_heating_start_sequence();
void test_bsm_step_start_waits_for_minimum_off_time();
void test_bsm_step_request_withdrawn_during_pre_purge_aborts();
void test_bsm_step_heating_disable_stops_during_min_on_time();
void test_bsm_step_water_disable_does_not_stop_heating();
void test_bsm_step_demand_end_respects_min_on_time();
void test_bsm_step_lost_mode_request_stops_after_grace();
void test_bsm_step_flame_loss_bypasses_min_on_time();
void test_bsm_step_heating_to_water_is_seamless();
void test_bsm_step_water_to_heating_waits_for_heating_request();
void test_bsm_step_handover_stops_when_heating_not_wanted();
void test_bsm_step_handover_room_in_restart_band_stops();
void test_bsm_step_handover_wait_is_bounded();
void test_bsm_step_failed_mode_switch_stops_burner();
void test_bsm_step_revert_without_on_bit_does_not_bounce();
void test_bsm_step_revert_with_on_bit_resumes_low_power();
void test_bsm_step_mode_change_without_flame_stops();
void test_bsm_step_safety_failure_during_mode_switch_errors();
void test_bsm_step_power_level_follows_request_with_anti_flapping();
void test_bsm_step_post_purge_restarts_when_demand_returns();
void test_bsm_step_post_purge_no_restart_without_mode_request();
void test_bsm_step_post_purge_no_restart_of_disabled_mode();
void test_bsm_step_post_purge_restart_requires_safety();
void test_bsm_step_ignition_failure_retries_then_locks_out();
void test_bsm_step_ignition_retry_success_resets_counter();
void test_bsm_step_new_start_gets_full_ignition_attempts_after_lockout();
void test_bsm_step_no_mode_grace_survives_steps_in_same_millisecond();
void test_bsm_step_stray_other_mode_on_bit_does_not_bounce();
void test_bsm_step_demand_withdrawn_late_in_pre_purge_aborts();
void test_bsm_step_water_disable_during_charge_stops_now();
void test_bsm_step_explicit_disable_during_mode_switch_stops();

// Burner demand gate (who may arm the heat demand)
void test_gate_hot_boiler_request_does_not_arm();
void test_gate_cold_boiler_request_arms_immediately();
void test_gate_handover_uses_prediction_when_decision_was_for_other_target();
void test_gate_fresh_matching_decision_wins_over_temperature();
void test_gate_without_boiler_temperature_control_task_arms();
void test_gate_pid_arms_demand_it_did_not_see_armed();
void test_gate_drops_demand_rearmed_while_coasting();
void test_gate_not_permitted_never_arms();
void test_gate_power_update_only_on_pid_change();
void test_gate_fallback_target_cap();
void test_gate_no_decision_follows_pid_on_threshold();
void test_gate_band_below_target_no_short_start();
void test_gate_prediction_follows_current_level();
void test_gate_revoke_race_cannot_leave_demand_armed();
void test_gate_demand_write_change_detection();

// Modulating cycle start after a pause (stale power level, pause detection)
void test_cycle_start_pause_resets_pid_and_starts_off();
void test_cycle_start_mode_or_gain_change_keeps_level();
void test_cycle_start_pause_detection_and_read_order();
void test_prediction_stale_half_after_pause_does_not_arm_above_target();

// Autotune demand (level-triggered OFF/FULL, retry after a blocked ON edge)
void test_autotune_off_phase_drops_demand_armed_elsewhere();
void test_autotune_blocked_on_edge_is_retried();
void test_autotune_armed_but_not_high_reasserts_full();
void test_autotune_not_permitted_disarms();

// Sensor fallback check and confirmation feed (idle flapping, stale heat demand)
void test_sensor_check_only_with_request_or_armed_demand();
void test_sensor_stop_not_forced_after_normal_request_end();
void test_sensor_stop_still_fires_with_persistent_demand();

// Boiler PID step and power level mapping (firmware code, multi-cycle)
void test_pid_step_reference_values();
void test_pid_step_derivative_skipped_after_reset();
void test_pid_step_zero_dt_uses_one_ms();
void test_pid_integral_stops_winding_at_output_limit();
void test_pid_integral_stops_winding_at_negative_output_limit();
void test_pid_integral_unwinds_while_saturated_against_error();
void test_pid_step_output_clamped_before_narrowing();
void test_pid_step_derivative_truncates_toward_zero();
void test_pid_integral_clamped_to_integral_limits();
void test_power_map_off_turns_on_only_above_55_percent();
void test_power_map_half_and_full_hysteresis();
void test_power_map_exact_switch_points_from_reset();
void test_bang_bang_bands_and_hold();
void test_bang_bang_full_threshold_is_exclusive();
void test_bang_bang_pause_starts_from_off();
void test_bang_bang_pause_detection_is_wrap_safe();
void test_boiler_pid_off_above_target_stays_off_through_band();
void test_boiler_pid_holds_half_near_target();
void test_boiler_pid_cold_start_full_then_half_before_target();

// Relay command policy (no-op commands vs rate limiting)
void test_relay_policy_noop_commands_skip_protection();
void test_relay_policy_real_changes_are_protected();
void test_relay_policy_emergency_bypasses_protection();
void test_relay_policy_desired_bit_is_zero_based();
void test_relay_policy_invalid_relay_index_rejected();
void test_relay_policy_duplicate_off_to_burner_relays_not_skipped();
void test_relay_policy_batch_resend_then_power_on_accepted();
void test_relay_policy_rate_limiter_interval_and_window();
void test_relay_policy_noop_and_emergency_skip_limiter_and_pump_check();
void test_relay_policy_pump_protection_checked_after_rate_limit();
void test_relay_policy_pump_timer_only_on_real_change();
void test_relay_policy_pump_request_resent_until_relay_follows();
void test_relay_policy_gate_toggle_expectations();
void test_relay_policy_decide_is_exhaustive();
void test_relay_policy_admit_passes_requested_state_to_pump_check();
void test_relay_policy_window_reset_readmits();

// Scheduler command policy (enable command, space mode defaults, status reply)
void test_scheduler_enable_payload_validation();
void test_scheduler_enable_ends_active_run_only_when_disabled();
void test_scheduler_space_mode_default_targets();
void test_scheduler_disable_alias_rejects_conflicting_payload();
void test_scheduler_clear_requires_confirm_payload();
void test_scheduler_unhandled_command_always_replies();
void test_scheduler_status_lists_active_and_disabled_ids();
void test_scheduler_status_worst_case_fits_mqtt_payload();
// Burner boost policy (stage-2 lock-out for mild-weather heating)
void test_band_keeps_firing_until_the_top_of_the_band();
void test_band_relights_only_at_the_bottom_of_the_band();
void test_band_is_asymmetric_which_is_the_whole_point();
void test_band_width_is_eight_kelvin_for_heating_and_seven_for_water();
void test_band_tracks_a_moving_target();
void test_band_rejects_a_nonsense_band_as_a_pair();
void test_band_cannot_push_flow_near_the_safety_interlock();
void test_band_restarts_from_off_after_a_request_pause();

void test_boost_denied_for_mild_weather_heating_targets();
void test_boost_allowed_for_cold_weather_heating_targets();
void test_boost_threshold_is_inclusive_at_exactly_the_limit();
void test_boost_always_allowed_for_water_charge();
void test_boost_always_allowed_while_autotuning();
void test_boost_gate_degenerate_ends_of_the_parameter_range();
void test_boost_denied_for_invalid_or_zero_target();
void test_boost_water_and_autotune_override_an_invalid_target();

void test_charge_delta_bounds_match_what_the_parameter_is_registered_with();
void test_charge_delta_valid_range_is_inclusive_at_both_ends();
void test_charge_delta_rejects_nan_and_inf();
void test_charge_delta_or_default_passes_valid_values_through_unchanged();

void test_heating_curve_survives_invalid_room_reading();
void test_heating_curve_raw_saturates_instead_of_truncating();
void test_heating_curve_nominal_room_fallback_is_sane();
void test_heating_curve_room_at_target_fallback_tracks_the_real_reading();

void test_scheduler_status_small_buffer_stays_valid_json();

void test_basic_allocation();
void test_pool_exhaustion();
void test_invalid_deallocation();
void test_statistics();
void test_raii_wrapper();
void test_raii_move();
void test_contains();
void test_different_pool_sizes();
void test_allocation_pattern_stress();

// Sensor integration tests
void test_sensor_normal_data_flow();
void test_sensor_timeout_handling();
void test_partial_sensor_failure();
void test_sensor_recovery();
void test_sensor_data_validation();
void test_sensor_update_timing();

// Relay integration tests
void test_burner_startup_sequence();
void test_emergency_stop_relay_control();
void test_relay_switch_timing_protection();
void test_relay_disconnection_handling();
void test_power_level_relay_mapping();
void test_pump_relay_control();
void test_multiple_relay_coordination();
void test_relay_state_after_error();

// Control loop integration tests
void test_basic_heating_control_loop();
void test_water_heating_priority();
void test_pid_control_response();
void test_hysteresis_control();
void test_weather_compensation();
void test_control_loop_timing();
void test_emergency_stop_clears_requests();
void test_anti_flapping_control();

// Persistent storage integration tests
void test_parameter_registration();
void test_parameter_validation();
void test_save_and_load();
void test_parameter_listing();
void test_readonly_parameters();
void test_persistence_across_restarts();
void test_error_handling();
void test_nvs_space_usage();

// MQTT integration tests
void test_mqtt_connection();
void test_status_publishing();
void test_remote_control();
void test_parameter_updates();
void test_emergency_stop();
void test_diagnostics_publishing();
void test_connection_loss();
void test_qos_and_retention();

// End-to-end system tests
void test_complete_heating_cycle();
void test_water_heating_priority_scenario();
void test_emergency_stop_scenario();
void test_sensor_failure_recovery();
void test_anti_flapping_behavior();

// Temperature cycle tests
void test_temperature_simulation_basics();
void test_heating_cycle_with_hysteresis();

// Improvement 3: Concurrency tests (14 tests)
void test_concurrency_mutex_correct_order();
void test_concurrency_mutex_wrong_order_detected();
void test_concurrency_circuit_breaker_triggers_after_3_failures();
void test_concurrency_mode_switch_water_and_heating_simultaneous();
void test_concurrency_mode_switch_water_priority_wins();
void test_concurrency_mode_switch_rapid_toggle();
void test_concurrency_seamless_switch_requires_all_conditions();
void test_concurrency_sensor_reading_atomic_fields();
void test_concurrency_sensor_staleness_check_during_update();
void test_concurrency_validity_flag_consistency();
void test_concurrency_antiflapping_minimum_on_time();
void test_concurrency_antiflapping_minimum_off_time();
void test_concurrency_antiflapping_power_level_throttle();
void test_concurrency_antiflapping_concurrent_demands();


// PIDAutoTuner tests
void test_pid_circular_buffer_basic();
void test_pid_circular_buffer_overflow();
void test_pid_circular_buffer_clear();
void test_pid_initial_state();
void test_pid_start_tuning();
void test_pid_cannot_start_twice();
void test_pid_stop_tuning();
void test_pid_relay_below_setpoint();
void test_pid_relay_above_setpoint();
void test_pid_relay_hysteresis_band();
void test_pid_peak_detection();
void test_pid_trough_detection();
void test_pid_complete_oscillation_cycle();
void test_pid_timeout_failure();
void test_pid_ziegler_nichols_pi_method();
void test_pid_ziegler_nichols_pid_method();
void test_pid_progress_tracking();
void test_pid_elapsed_time();

// Autotune run time (millis() wrap, float resolution)
void test_autotune_clock_survives_millis_wrap();
void test_autotune_clock_keeps_resolution_at_high_uptime();
void test_autotune_elapsed_time_counts_from_the_first_sample();
void test_autotune_run_across_millis_wrap_keeps_running();
void test_autotune_backwards_time_base_fails_instead_of_corrupting();

// Autotune result gain set (mode captured at the start of the run)
void test_autotune_gains_go_to_the_gain_set_of_the_run();
void test_autotune_result_rejected_when_mode_changed();

// Burner safety rules (Layer-1 checks of BurnerSafetyValidator)
void test_safety_rules_safe_readings_pass();
void test_safety_rules_emergency_stop_checked_first();
void test_safety_rules_sensor_ranges_and_count();
void test_safety_rules_stale_data_invalidates_all_sensors();
void test_safety_rules_boiler_limit_inclusive();
void test_safety_rules_water_limit_only_in_water_mode();
void test_safety_rules_pressure_bounds();
void test_safety_rules_missing_pressure_blocks_unless_allowed();
void test_safety_rules_missing_pressure_checked_after_sensors_and_limits();
void test_safety_rules_interlock_open_before_thermal_shock();
void test_safety_rules_thermal_shock_above_35c();
void test_safety_rules_result_codes();

// Common setUp and tearDown
void setUp(void) {
    // Common setup - individual tests can add their own setup
}

void tearDown(void) {
    // Common teardown - individual tests can add their own teardown
}

// Scheduler clock (RTC UTC -> local schedule wall-clock, DST rule)
void test_scheduler_clock_spring_forward_skips_an_hour();
void test_scheduler_clock_fall_back_repeats_an_hour();
void test_scheduler_clock_is_the_dst_fix_a_schedule_holds_its_wall_clock();
void test_scheduler_clock_epoch_from_utc_fields_matches_known_instants();
void test_scheduler_clock_epoch_from_utc_fields_applies_no_timezone();
void test_scheduler_clock_rejects_an_unset_rtc();
void test_scheduler_clock_day_of_week_is_sunday_zero_and_local();
void test_scheduler_clock_transitions_hold_for_later_years();

// Room-temp resolution against the real SharedSensorReadings struct
void test_room_resolve_credits_the_sensors_read_time_not_now();
void test_room_resolve_falls_back_to_now_before_the_driver_ever_wrote();
void test_room_resolve_walks_the_whole_ladder_on_a_pulled_sensor();
void test_room_resolve_survives_the_millis_rollover();
void test_room_resolve_real_struct_has_the_field_the_mock_lacked();

void test_policy_frost_latch_keeps_a_burn_alive_with_the_switch_off();
void test_policy_frost_never_overrides_a_disabled_boiler();
void test_power_fault_first_fault_must_not_degrade();

void test_invalid_count_boiler_output_escalates_sooner_than_the_shared_count();
void test_flow_limits_inverted_pair_is_ordered_not_defaulted();

// Room sensor register report (ANDRTF3 open firmware regs 50..77)
void test_report_no_device_is_not_a_zero_reading();
void test_report_distinguishes_stock_firmware_from_a_failed_read();
void test_report_always_emits_zero_failures_explicitly();
void test_report_carries_the_firmware_slot_and_uptime();
void test_report_failure_breakdown_is_published();
void test_report_fits_the_payload_with_every_counter_set();
void test_report_marks_truncation_instead_of_losing_the_tail();
void test_report_refuses_rather_than_emit_half_a_payload();
void test_report_publishes_negative_temperatures_signed();

// Heating curve anchoring - both formulations compared (2026-10-26)
void test_anchor_room_at_target_is_identical_to_the_old_behaviour();
void test_anchor_colder_room_now_raises_the_flow_target();
void test_anchor_sensitivity_is_exactly_minus_the_factor();
void test_anchor_recovery_case_measured_on_the_live_config();
void test_anchor_room_mode_is_untouched();
void test_anchor_substituted_room_gets_no_deviation();
void test_anchor_zero_factor_disables_only_the_deviation();
void test_anchor_shift_cannot_overflow_int16();
void test_anchor_commanded_flow_is_clamped_not_the_raw_sensitivity();

// Outside-sensor fallback (2026-10-28)
void test_outside_valid_reading_is_used_and_recorded();
void test_outside_dropout_holds_the_last_reading_so_the_curve_keeps_working();
void test_outside_hold_lasts_an_hour_then_expires();
void test_outside_gone_falls_back_to_the_room_not_to_no_heat();
void test_outside_gone_and_no_usable_room_is_the_only_no_heat_case();
void test_outside_hold_survives_the_millis_rollover();
void test_room_thermostat_predicate_is_asymmetric();
void test_room_thermostat_fallback_would_have_heated_the_house();

// One shared frost-only burn classification (2026-10-29)
void test_burnkind_no_frost_is_never_frost_only();
void test_burnkind_frost_with_switch_off_is_frost_only();
void test_burnkind_frost_with_switch_on_but_nothing_wanted_is_frost_only();
void test_burnkind_frost_during_real_space_heating_is_not_frost_only();
void test_burnkind_summer_override_makes_it_frost_only();
void test_burnkind_the_three_old_predicates_disagreed_exactly_here();
void test_burnkind_published_state_round_trips_and_clears();
void test_burnkind_room_measurement_defaults_true();

int main(int argc, char **argv) {
    UNITY_BEGIN();
    
    // Temperature conversion tests
    RUN_TEST(test_float_to_temperature_conversion);
    RUN_TEST(test_temperature_to_float_conversion);
    RUN_TEST(test_temperature_addition);
    RUN_TEST(test_temperature_subtraction);
    RUN_TEST(test_temperature_comparison);
    RUN_TEST(test_temperature_formatting);
    RUN_TEST(test_invalid_temperature);
    RUN_TEST(test_temperature_edge_cases);
    RUN_TEST(test_temperature_difference);

    // Memory pool tests
    RUN_TEST(test_basic_allocation);
    RUN_TEST(test_pool_exhaustion);
    RUN_TEST(test_invalid_deallocation);
    RUN_TEST(test_statistics);
    RUN_TEST(test_raii_wrapper);
    RUN_TEST(test_raii_move);
    RUN_TEST(test_contains);
    RUN_TEST(test_different_pool_sizes);
    RUN_TEST(test_allocation_pattern_stress);
    
    // ---------------------------------------------------------------------------------
    // THE SIX "INTEGRATION" SUITES BELOW DRIVE MOCK OBJECTS, NOT FIRMWARE.
    //
    // 45 of this file's cases, measured 2026-10-26. They pin the INTENDED interactions and are
    // worth keeping, but a green run here says nothing about the code that ships - so the
    // banner is printed into the test output, where someone reading a pass will see it.
    //
    // The 2026-10-25 review counted "89 of 350"; that figure predates 610016f, which turned
    // MockSystemSettings and MockSharedSensorReadings into forwarding headers onto the REAL
    // structs. What remains is BEHAVIOURAL mocks: MockBurnerStateMachine,
    // MockBurnerRequestManager, MockHeatingControl, MockTemperatureSensorFallback, MockMB8ART,
    // MockRYN4, MockTime, and a MockPersistentStorage defined inline in its own test file.
    //
    // Converting these is not the same job as the struct swap was. Those needed four
    // unnecessary includes removed; these need the real BurnerStateMachine, drivers and storage
    // library to compile and run on the host, i.e. a FreeRTOS/NVS shim. That is a project, not
    // a cleanup, and half-doing it would be worse than the honest label.
    // ---------------------------------------------------------------------------------
    // AND THE FIGURE WAS UNDERSTATED. The 2026-10-28 review counted the test-local
    // reimplementations this banner did not: test_concurrency.cpp (14 cases, only unity +
    // MockTime, file-static mutex/event state), test_memory_pool.cpp (9 cases - a test-local
    // MemoryPool/PooledPtr under #ifdef NATIVE_TEST with no path to src/utils/MemoryPool.h,
    // and they run BEFORE this banner), and 20 of 23 in test_pid_autotuner.cpp (test-local
    // TestPIDAutoTuner/TestCircularBuffer; only the three AutotuneClock cases are real). So
    // the honest total is 88, not 45 - and some of it is printed above this line, which is
    // why the banner now states the whole picture rather than bracketing one group.
    UnityPrint("--- 88 of the cases in this suite exercise NO firmware code: the 45 ---");
    UNITY_PRINT_EOL();
    UnityPrint("--- mock-driven integration cases that follow, plus 14 in concurrency, ---");
    UNITY_PRINT_EOL();
    UnityPrint("--- 9 in memory_pool and 20 in pid_autotuner, which ran ABOVE this line ---");
    UNITY_PRINT_EOL();

    // Sensor integration tests
    RUN_TEST(test_sensor_normal_data_flow);
    RUN_TEST(test_sensor_timeout_handling);
    RUN_TEST(test_partial_sensor_failure);
    RUN_TEST(test_sensor_recovery);
    RUN_TEST(test_sensor_data_validation);
    RUN_TEST(test_sensor_update_timing);
    
    // Relay integration tests
    RUN_TEST(test_burner_startup_sequence);
    RUN_TEST(test_emergency_stop_relay_control);
    RUN_TEST(test_relay_switch_timing_protection);
    RUN_TEST(test_relay_disconnection_handling);
    RUN_TEST(test_power_level_relay_mapping);
    RUN_TEST(test_pump_relay_control);
    RUN_TEST(test_multiple_relay_coordination);
    RUN_TEST(test_relay_state_after_error);
    
    // Control loop integration tests
    RUN_TEST(test_basic_heating_control_loop);
    RUN_TEST(test_water_heating_priority);
    RUN_TEST(test_pid_control_response);
    RUN_TEST(test_hysteresis_control);
    RUN_TEST(test_weather_compensation);
    RUN_TEST(test_control_loop_timing);
    RUN_TEST(test_emergency_stop_clears_requests);
    RUN_TEST(test_anti_flapping_control);
    
    // Persistent storage integration tests
    RUN_TEST(test_parameter_registration);
    RUN_TEST(test_parameter_validation);
    RUN_TEST(test_save_and_load);
    RUN_TEST(test_parameter_listing);
    RUN_TEST(test_readonly_parameters);
    RUN_TEST(test_persistence_across_restarts);
    RUN_TEST(test_error_handling);
    RUN_TEST(test_nvs_space_usage);
    
    // MQTT integration tests
    RUN_TEST(test_mqtt_connection);
    RUN_TEST(test_status_publishing);
    RUN_TEST(test_remote_control);
    RUN_TEST(test_parameter_updates);
    RUN_TEST(test_emergency_stop);
    RUN_TEST(test_diagnostics_publishing);
    RUN_TEST(test_connection_loss);
    RUN_TEST(test_qos_and_retention);
    
    // End-to-end system tests
    RUN_TEST(test_complete_heating_cycle);
    RUN_TEST(test_water_heating_priority_scenario);
    RUN_TEST(test_emergency_stop_scenario);
    RUN_TEST(test_sensor_failure_recovery);
    RUN_TEST(test_anti_flapping_behavior);
    
    // Temperature cycle tests
    RUN_TEST(test_temperature_simulation_basics);
    RUN_TEST(test_heating_cycle_with_hysteresis);

    // Improvement 3: Concurrency tests (14 tests)
    RUN_TEST(test_concurrency_mutex_correct_order);
    RUN_TEST(test_concurrency_mutex_wrong_order_detected);
    RUN_TEST(test_concurrency_circuit_breaker_triggers_after_3_failures);
    RUN_TEST(test_concurrency_mode_switch_water_and_heating_simultaneous);
    RUN_TEST(test_concurrency_mode_switch_water_priority_wins);
    RUN_TEST(test_concurrency_mode_switch_rapid_toggle);
    RUN_TEST(test_concurrency_seamless_switch_requires_all_conditions);
    RUN_TEST(test_concurrency_sensor_reading_atomic_fields);
    RUN_TEST(test_concurrency_sensor_staleness_check_during_update);
    RUN_TEST(test_concurrency_validity_flag_consistency);
    RUN_TEST(test_concurrency_antiflapping_minimum_on_time);
    RUN_TEST(test_concurrency_antiflapping_minimum_off_time);
    RUN_TEST(test_concurrency_antiflapping_power_level_throttle);
    RUN_TEST(test_concurrency_antiflapping_concurrent_demands);


    // PIDAutoTuner tests
    RUN_TEST(test_pid_circular_buffer_basic);
    RUN_TEST(test_pid_circular_buffer_overflow);
    RUN_TEST(test_pid_circular_buffer_clear);
    RUN_TEST(test_pid_initial_state);
    RUN_TEST(test_pid_start_tuning);
    RUN_TEST(test_pid_cannot_start_twice);
    RUN_TEST(test_pid_stop_tuning);
    RUN_TEST(test_pid_relay_below_setpoint);
    RUN_TEST(test_pid_relay_above_setpoint);
    RUN_TEST(test_pid_relay_hysteresis_band);
    RUN_TEST(test_pid_peak_detection);
    RUN_TEST(test_pid_trough_detection);
    RUN_TEST(test_pid_complete_oscillation_cycle);
    RUN_TEST(test_pid_timeout_failure);
    RUN_TEST(test_pid_ziegler_nichols_pi_method);
    RUN_TEST(test_pid_ziegler_nichols_pid_method);
    RUN_TEST(test_pid_progress_tracking);
    RUN_TEST(test_pid_elapsed_time);

    // Autotune run time (millis() wrap, float resolution)
    RUN_TEST(test_autotune_clock_survives_millis_wrap);
    RUN_TEST(test_autotune_clock_keeps_resolution_at_high_uptime);
    RUN_TEST(test_autotune_elapsed_time_counts_from_the_first_sample);
    RUN_TEST(test_autotune_run_across_millis_wrap_keeps_running);
    RUN_TEST(test_autotune_backwards_time_base_fails_instead_of_corrupting);

    // Autotune result gain set (mode captured at the start of the run)
    RUN_TEST(test_autotune_gains_go_to_the_gain_set_of_the_run);
    RUN_TEST(test_autotune_result_rejected_when_mode_changed);

    // Burner safety rules (Layer-1 checks of BurnerSafetyValidator)
    RUN_TEST(test_safety_rules_safe_readings_pass);
    RUN_TEST(test_safety_rules_emergency_stop_checked_first);
    RUN_TEST(test_safety_rules_sensor_ranges_and_count);
    RUN_TEST(test_safety_rules_stale_data_invalidates_all_sensors);
    RUN_TEST(test_safety_rules_boiler_limit_inclusive);
    RUN_TEST(test_safety_rules_water_limit_only_in_water_mode);
    RUN_TEST(test_safety_rules_pressure_bounds);
    RUN_TEST(test_safety_rules_missing_pressure_blocks_unless_allowed);
    RUN_TEST(test_safety_rules_missing_pressure_checked_after_sensors_and_limits);
    RUN_TEST(test_safety_rules_interlock_open_before_thermal_shock);
    RUN_TEST(test_safety_rules_thermal_shock_above_35c);
    RUN_TEST(test_safety_rules_result_codes);

    // PID fixed-point gain conversion (regression: inert boiler PID)
    RUN_TEST(test_pid_gain_conversion_scales_by_1000);
    RUN_TEST(test_pid_gain_conversion_rejects_invalid);
    RUN_TEST(test_pid_scaled_gain_commands_off_above_target);
    RUN_TEST(test_pid_scaled_gain_commands_full_below_target);
    RUN_TEST(test_pid_adjustment_clamp_keeps_sign);
    RUN_TEST(test_pid_output_limit_matches_power_saturation);

    // Heating curve (weather-compensated heating target)
    RUN_TEST(test_heating_curve_matches_reference_formula);
    RUN_TEST(test_heating_curve_outside_part_not_ten_times_too_small);
    RUN_TEST(test_heating_curve_clamps_to_limits);
    RUN_TEST(test_heating_curve_survives_invalid_room_reading);
    RUN_TEST(test_heating_curve_raw_saturates_instead_of_truncating);
    RUN_TEST(test_heating_curve_nominal_room_fallback_is_sane);
    RUN_TEST(test_heating_curve_room_at_target_fallback_tracks_the_real_reading);

    // Space heating policy (weather mode outside threshold hysteresis)
    RUN_TEST(test_heating_diag_payload_is_the_agreed_schema);
    RUN_TEST(test_heating_diag_fits_the_mqtt_payload_limit);
    RUN_TEST(test_heating_diag_refuses_a_too_small_buffer_instead_of_truncating);
    RUN_TEST(test_heating_diag_block_reasons_are_distinct_strings);
    RUN_TEST(test_heating_diag_room_mode_and_negative_raw_serialise);
    RUN_TEST(test_heating_diag_differs_detects_every_field);
    RUN_TEST(test_heating_diag_reports_a_substituted_room_reading_as_such);

    RUN_TEST(test_room_fallback_holds_the_last_reading_through_a_short_dropout);
    RUN_TEST(test_room_fallback_expires_the_hold_and_falls_back_to_target);
    RUN_TEST(test_room_fallback_with_nothing_remembered_uses_target_then_nominal);
    RUN_TEST(test_room_fallback_survives_the_millis_rollover);
    RUN_TEST(test_room_fallback_recording_a_new_reading_refreshes_the_age);
    RUN_TEST(test_room_fallback_never_returns_an_invalid_marker);

    RUN_TEST(test_frost_is_inert_in_a_working_system);
    RUN_TEST(test_frost_triggers_on_either_reading);
    RUN_TEST(test_frost_holds_through_the_hysteresis_band);
    RUN_TEST(test_frost_never_triggers_on_an_invalid_reading);
    RUN_TEST(test_frost_releases_rather_than_firing_blind);
    RUN_TEST(test_frost_one_cold_sensor_holds_while_the_other_is_warm);
    RUN_TEST(test_frost_thresholds_are_the_agreed_numbers);
    RUN_TEST(test_frost_a_stale_reading_never_fires_the_burner);
    RUN_TEST(test_frost_is_one_latch_over_all_three_sensors);
    RUN_TEST(test_frost_room_trigger_cannot_fire_in_a_lived_in_house);
    RUN_TEST(test_frost_cold_mask_describes_every_combination);

    RUN_TEST(test_room_diag_healthy_payload_is_tiny_and_has_no_error_keys);
    RUN_TEST(test_room_diag_reports_the_contention_hypothesis);
    RUN_TEST(test_room_diag_age_is_seconds_and_omitted_when_never_read);
    RUN_TEST(test_room_diag_worst_case_fits_the_mqtt_payload_limit);
    RUN_TEST(test_room_diag_refuses_a_too_small_buffer_instead_of_truncating);
    RUN_TEST(test_room_diag_differs_ignores_age_and_the_success_count);
    RUN_TEST(test_room_diag_key_table_matches_the_state_array);
    RUN_TEST(test_sensor_offset_map_places_each_field_on_its_channel);
    RUN_TEST(test_sensor_offset_map_outside_is_ch3_not_ch6);
    RUN_TEST(test_sensor_offset_map_never_offsets_the_pressure_channel);
    RUN_TEST(test_sensor_offset_map_leaves_tank_top_at_zero);
    RUN_TEST(test_sensor_offset_map_has_no_colliding_or_dropped_fields);
    RUN_TEST(test_sensor_offset_map_all_zero_in_all_zero_out);
    RUN_TEST(test_sensor_error_bit_single_failure_does_not_raise);
    RUN_TEST(test_sensor_error_bit_raises_only_after_the_confirm_window);
    RUN_TEST(test_sensor_error_bit_recovery_inside_the_window_never_raises);
    RUN_TEST(test_sensor_error_bit_clears_on_recovery_when_owned);
    RUN_TEST(test_sensor_error_bit_never_clears_a_bit_owned_by_the_fallback);
    RUN_TEST(test_sensor_error_bit_raise_is_idempotent_and_outranks_clear);
    RUN_TEST(test_sensor_error_bit_full_latch_regression_sequence);
    RUN_TEST(test_param_save_explicit_request_is_never_delayed);
    RUN_TEST(test_param_save_idle_never_saves);
    RUN_TEST(test_param_save_waits_for_the_debounce_window);
    RUN_TEST(test_param_save_coalesces_a_burst_of_clicks);
    RUN_TEST(test_param_save_survives_the_millis_wrap);
    RUN_TEST(test_space_heating_outside_threshold_start_and_stop_limits);
    RUN_TEST(test_space_heating_outside_noise_does_not_toggle);
    RUN_TEST(test_invalid_count_steps_once_per_sample_not_once_per_caller);
    RUN_TEST(test_invalid_count_floor_under_uniform_polling);
    RUN_TEST(test_invalid_count_on_the_real_mb8art_schedule_is_eight_to_ten_seconds);
    RUN_TEST(test_invalid_count_still_escalates_when_the_sensor_is_completely_dead);
    RUN_TEST(test_invalid_count_rate_limit_survives_the_millis_rollover);
    RUN_TEST(test_invalid_count_first_observation_is_never_rate_limited);
    RUN_TEST(test_bus_release_window_is_clamped_and_capped);
    RUN_TEST(test_bus_release_refuses_while_the_boiler_is_busy);
    RUN_TEST(test_bus_release_refuses_while_a_pump_is_dissipating_heat);
    RUN_TEST(test_bus_release_refuses_mid_sequence_and_under_a_latched_estop);
    RUN_TEST(test_bus_release_extending_a_window_never_shortens_it);
    RUN_TEST(test_bus_release_firing_mask_tracks_the_relay_indices);
    RUN_TEST(test_bus_release_expiry_survives_the_millis_rollover);
    RUN_TEST(test_bus_release_seconds_left_rounds_up_and_never_contradicts_released);
    RUN_TEST(test_bus_release_safe_mask_clears_firing_and_keeps_pumps);
    RUN_TEST(test_space_heating_room_backstop_is_asymmetric);
    RUN_TEST(test_space_heating_unknown_room_must_not_license_heating);
    RUN_TEST(test_space_heating_target_substitution_would_have_reopened_the_hole);
    RUN_TEST(test_space_heating_weather_decision_outside_gate_is_reported_first);
    RUN_TEST(test_space_heating_weather_decision_heats_and_blocks_on_the_room);
    RUN_TEST(test_space_heating_no_room_target_keeps_weather_only_control);

    // Error context (compact boiler/error/context JSON)
    RUN_TEST(test_error_context_worst_case_fits_mqtt_payload);
    RUN_TEST(test_error_context_escapes_and_null_sensors);
    RUN_TEST(test_error_context_small_buffer_shortens_description);

    // OTA validation policy
    RUN_TEST(test_ota_validation_waits_for_min_uptime);
    RUN_TEST(test_ota_validation_needs_sensors_only);
    RUN_TEST(test_ota_validation_rolls_back_after_max_wait);
    RUN_TEST(test_ota_validation_never_reboots_a_healthy_controller_over_a_dead_lan);

    // Sensor failure confirmation
    RUN_TEST(test_sensor_failure_single_glitch_does_not_stop);
    RUN_TEST(test_sensor_failure_persistent_with_demand_stops);
    RUN_TEST(test_sensor_failure_without_demand_never_stops);
    RUN_TEST(test_sensor_failure_millis_wrap);

    // Autotune peak/trough tracking (lagging plant)
    RUN_TEST(test_relay_extrema_peaks_include_post_switch_overshoot);
    RUN_TEST(test_relay_extrema_troughs_include_post_switch_undershoot);
    RUN_TEST(test_relay_extrema_ignores_cold_start_phase);
    RUN_TEST(test_relay_extrema_extreme_times_are_after_switch);

    // Burner transition policy (explicit disable, bounded mode switch)
    RUN_TEST(test_policy_explicit_disable_stops_running_mode_only);
    RUN_TEST(test_policy_boiler_disable_stops_any_mode);
    RUN_TEST(test_policy_heating_wanted_respects_enable_and_override);
    RUN_TEST(test_policy_heating_wanted_room_mode);
    RUN_TEST(test_policy_heating_wanted_weather_mode);
    RUN_TEST(test_policy_heating_wanted_weather_mode_restart_limit_boundaries);
    RUN_TEST(test_policy_heating_wanted_room_mode_start_ignores_hysteresis);
    RUN_TEST(test_policy_mode_switch_wait_is_bounded);
    RUN_TEST(test_policy_mode_revert_requires_on_bit);
    RUN_TEST(test_policy_mode_switch_exit_records_power_level);

    // Stage C policies (power level fault escalation, water limit consistency)
    RUN_TEST(test_power_fault_escalates_on_third_fault_in_window);
    RUN_TEST(test_power_fault_window_restarts_after_ten_minutes);
    RUN_TEST(test_water_limits_valid_only_when_low_below_high);
    RUN_TEST(test_water_charge_latch_resumes_only_while_latched);
    RUN_TEST(test_charge_delta_bounds_match_what_the_parameter_is_registered_with);
    RUN_TEST(test_charge_delta_valid_range_is_inclusive_at_both_ends);
    RUN_TEST(test_charge_delta_rejects_nan_and_inf);
    RUN_TEST(test_charge_delta_or_default_passes_valid_values_through_unchanged);

    // Autotune relay settings (amplitude, hysteresis)
    RUN_TEST(test_autotune_amplitude_setting_used_within_range);
    RUN_TEST(test_autotune_hysteresis_setting_used_within_range);
    RUN_TEST(test_autotune_amplitude_vs_two_stage_swing);

    // Emergency stop release (MQTT emergency_reset)
    RUN_TEST(test_emergency_release_when_causes_cleared);
    RUN_TEST(test_emergency_release_not_active);
    RUN_TEST(test_emergency_release_refused_while_hot);
    RUN_TEST(test_emergency_release_refused_on_sensor_or_system_errors);
    RUN_TEST(test_emergency_stop_onset_once_per_latch);
    RUN_TEST(test_emergency_dissipation_until_boiler_cooled);
    RUN_TEST(test_emergency_dissipation_without_usable_output);
    RUN_TEST(test_emergency_sensor_recovery_releases_only_stale_sensor_stop);
    RUN_TEST(test_emergency_cause_merge_while_latched);
    RUN_TEST(test_emergency_safety_split_matches_the_latched_evaluation);
    RUN_TEST(test_emergency_safety_refuses_a_re_enable_over_a_hot_boiler);
    RUN_TEST(test_emergency_dissipation_continues_after_release_until_cooled);

    // Burner transitions (state machine scenarios through BurnerTransitions::step)
    RUN_TEST(test_bsm_step_idle_without_demand_skips_safety_check);
    RUN_TEST(test_bsm_step_stale_demand_never_starts_burner);
    RUN_TEST(test_bsm_step_heating_start_sequence);
    RUN_TEST(test_bsm_step_start_waits_for_minimum_off_time);
    RUN_TEST(test_bsm_step_request_withdrawn_during_pre_purge_aborts);
    RUN_TEST(test_bsm_step_heating_disable_stops_during_min_on_time);
    RUN_TEST(test_bsm_step_water_disable_does_not_stop_heating);
    RUN_TEST(test_bsm_step_demand_end_respects_min_on_time);
    RUN_TEST(test_bsm_step_lost_mode_request_stops_after_grace);
    RUN_TEST(test_bsm_step_flame_loss_bypasses_min_on_time);
    RUN_TEST(test_bsm_step_heating_to_water_is_seamless);
    RUN_TEST(test_bsm_step_water_to_heating_waits_for_heating_request);
    RUN_TEST(test_bsm_step_handover_stops_when_heating_not_wanted);
    RUN_TEST(test_bsm_step_handover_room_in_restart_band_stops);
    RUN_TEST(test_bsm_step_handover_wait_is_bounded);
    RUN_TEST(test_bsm_step_failed_mode_switch_stops_burner);
    RUN_TEST(test_bsm_step_revert_without_on_bit_does_not_bounce);
    RUN_TEST(test_bsm_step_revert_with_on_bit_resumes_low_power);
    RUN_TEST(test_bsm_step_mode_change_without_flame_stops);
    RUN_TEST(test_bsm_step_safety_failure_during_mode_switch_errors);
    RUN_TEST(test_bsm_step_power_level_follows_request_with_anti_flapping);
    RUN_TEST(test_bsm_step_post_purge_restarts_when_demand_returns);
    RUN_TEST(test_bsm_step_post_purge_no_restart_without_mode_request);
    RUN_TEST(test_bsm_step_post_purge_no_restart_of_disabled_mode);
    RUN_TEST(test_bsm_step_post_purge_restart_requires_safety);
    RUN_TEST(test_bsm_step_ignition_failure_retries_then_locks_out);
    RUN_TEST(test_bsm_step_ignition_retry_success_resets_counter);
    RUN_TEST(test_bsm_step_new_start_gets_full_ignition_attempts_after_lockout);
    RUN_TEST(test_bsm_step_no_mode_grace_survives_steps_in_same_millisecond);
    RUN_TEST(test_bsm_step_stray_other_mode_on_bit_does_not_bounce);
    RUN_TEST(test_bsm_step_demand_withdrawn_late_in_pre_purge_aborts);
    RUN_TEST(test_bsm_step_water_disable_during_charge_stops_now);
    RUN_TEST(test_bsm_step_explicit_disable_during_mode_switch_stops);

    // Burner demand gate (who may arm the heat demand)
    RUN_TEST(test_gate_hot_boiler_request_does_not_arm);
    RUN_TEST(test_gate_cold_boiler_request_arms_immediately);
    RUN_TEST(test_gate_handover_uses_prediction_when_decision_was_for_other_target);
    RUN_TEST(test_gate_fresh_matching_decision_wins_over_temperature);
    RUN_TEST(test_gate_without_boiler_temperature_control_task_arms);
    RUN_TEST(test_gate_pid_arms_demand_it_did_not_see_armed);
    RUN_TEST(test_gate_drops_demand_rearmed_while_coasting);
    RUN_TEST(test_gate_not_permitted_never_arms);
    RUN_TEST(test_gate_power_update_only_on_pid_change);
    RUN_TEST(test_gate_fallback_target_cap);
    RUN_TEST(test_gate_no_decision_follows_pid_on_threshold);
    RUN_TEST(test_gate_band_below_target_no_short_start);
    RUN_TEST(test_gate_prediction_follows_current_level);
    RUN_TEST(test_gate_revoke_race_cannot_leave_demand_armed);
    RUN_TEST(test_gate_demand_write_change_detection);

    // Modulating cycle start after a pause (stale power level, pause detection)
    RUN_TEST(test_cycle_start_pause_resets_pid_and_starts_off);
    RUN_TEST(test_cycle_start_mode_or_gain_change_keeps_level);
    RUN_TEST(test_cycle_start_pause_detection_and_read_order);
    RUN_TEST(test_prediction_stale_half_after_pause_does_not_arm_above_target);

    // Autotune demand (level-triggered OFF/FULL, retry after a blocked ON edge)
    RUN_TEST(test_autotune_off_phase_drops_demand_armed_elsewhere);
    RUN_TEST(test_autotune_blocked_on_edge_is_retried);
    RUN_TEST(test_autotune_armed_but_not_high_reasserts_full);
    RUN_TEST(test_autotune_not_permitted_disarms);

    // Sensor fallback check and confirmation feed (idle flapping, stale heat demand)
    RUN_TEST(test_sensor_check_only_with_request_or_armed_demand);
    RUN_TEST(test_sensor_stop_not_forced_after_normal_request_end);
    RUN_TEST(test_sensor_stop_still_fires_with_persistent_demand);

    // Boiler PID step and power level mapping (firmware code, multi-cycle)
    RUN_TEST(test_pid_step_reference_values);
    RUN_TEST(test_pid_step_derivative_skipped_after_reset);
    RUN_TEST(test_pid_step_zero_dt_uses_one_ms);
    RUN_TEST(test_pid_integral_stops_winding_at_output_limit);
    RUN_TEST(test_pid_integral_stops_winding_at_negative_output_limit);
    RUN_TEST(test_pid_integral_unwinds_while_saturated_against_error);
    RUN_TEST(test_pid_step_output_clamped_before_narrowing);
    RUN_TEST(test_pid_step_derivative_truncates_toward_zero);
    RUN_TEST(test_pid_integral_clamped_to_integral_limits);
    RUN_TEST(test_power_map_off_turns_on_only_above_55_percent);
    RUN_TEST(test_power_map_half_and_full_hysteresis);
    RUN_TEST(test_power_map_exact_switch_points_from_reset);
    RUN_TEST(test_bang_bang_bands_and_hold);
    RUN_TEST(test_bang_bang_full_threshold_is_exclusive);
    RUN_TEST(test_bang_bang_pause_starts_from_off);
    RUN_TEST(test_bang_bang_pause_detection_is_wrap_safe);
    RUN_TEST(test_boiler_pid_off_above_target_stays_off_through_band);
    RUN_TEST(test_boiler_pid_holds_half_near_target);
    RUN_TEST(test_boiler_pid_cold_start_full_then_half_before_target);

    // Relay command policy (no-op commands vs rate limiting)
    RUN_TEST(test_relay_policy_noop_commands_skip_protection);
    RUN_TEST(test_relay_policy_real_changes_are_protected);
    RUN_TEST(test_relay_policy_emergency_bypasses_protection);
    RUN_TEST(test_relay_policy_desired_bit_is_zero_based);
    RUN_TEST(test_relay_policy_invalid_relay_index_rejected);
    RUN_TEST(test_relay_policy_duplicate_off_to_burner_relays_not_skipped);
    RUN_TEST(test_relay_policy_batch_resend_then_power_on_accepted);
    RUN_TEST(test_relay_policy_rate_limiter_interval_and_window);
    RUN_TEST(test_relay_policy_noop_and_emergency_skip_limiter_and_pump_check);
    RUN_TEST(test_relay_policy_pump_protection_checked_after_rate_limit);
    RUN_TEST(test_relay_policy_pump_timer_only_on_real_change);
    RUN_TEST(test_relay_policy_pump_request_resent_until_relay_follows);
    RUN_TEST(test_relay_policy_gate_toggle_expectations);
    RUN_TEST(test_relay_policy_decide_is_exhaustive);
    RUN_TEST(test_relay_policy_admit_passes_requested_state_to_pump_check);
    RUN_TEST(test_relay_policy_window_reset_readmits);

    // Scheduler command policy (enable command, space mode defaults, status reply)
    RUN_TEST(test_scheduler_enable_payload_validation);
    RUN_TEST(test_scheduler_enable_ends_active_run_only_when_disabled);
    RUN_TEST(test_scheduler_space_mode_default_targets);
    RUN_TEST(test_scheduler_disable_alias_rejects_conflicting_payload);
    RUN_TEST(test_scheduler_clear_requires_confirm_payload);
    RUN_TEST(test_scheduler_unhandled_command_always_replies);
    RUN_TEST(test_scheduler_status_lists_active_and_disabled_ids);
    RUN_TEST(test_scheduler_status_worst_case_fits_mqtt_payload);
    RUN_TEST(test_scheduler_status_small_buffer_stays_valid_json);

    // Burner boost policy (stage-2 lock-out for mild-weather heating)
    RUN_TEST(test_band_keeps_firing_until_the_top_of_the_band);
    RUN_TEST(test_band_relights_only_at_the_bottom_of_the_band);
    RUN_TEST(test_band_is_asymmetric_which_is_the_whole_point);
    RUN_TEST(test_band_width_is_eight_kelvin_for_heating_and_seven_for_water);
    RUN_TEST(test_band_tracks_a_moving_target);
    RUN_TEST(test_band_rejects_a_nonsense_band_as_a_pair);
    RUN_TEST(test_band_cannot_push_flow_near_the_safety_interlock);
    RUN_TEST(test_band_restarts_from_off_after_a_request_pause);
    RUN_TEST(test_boost_denied_for_mild_weather_heating_targets);
    RUN_TEST(test_boost_allowed_for_cold_weather_heating_targets);
    RUN_TEST(test_boost_threshold_is_inclusive_at_exactly_the_limit);
    RUN_TEST(test_boost_always_allowed_for_water_charge);
    RUN_TEST(test_boost_always_allowed_while_autotuning);
    RUN_TEST(test_boost_gate_degenerate_ends_of_the_parameter_range);
    RUN_TEST(test_boost_denied_for_invalid_or_zero_target);
    RUN_TEST(test_boost_water_and_autotune_override_an_invalid_target);

    // Scheduler clock (RTC UTC -> local schedule wall-clock, DST rule)
    RUN_TEST(test_scheduler_clock_spring_forward_skips_an_hour);
    RUN_TEST(test_scheduler_clock_fall_back_repeats_an_hour);
    RUN_TEST(test_scheduler_clock_is_the_dst_fix_a_schedule_holds_its_wall_clock);
    RUN_TEST(test_scheduler_clock_epoch_from_utc_fields_matches_known_instants);
    RUN_TEST(test_scheduler_clock_epoch_from_utc_fields_applies_no_timezone);
    RUN_TEST(test_scheduler_clock_rejects_an_unset_rtc);
    RUN_TEST(test_scheduler_clock_day_of_week_is_sunday_zero_and_local);
    RUN_TEST(test_scheduler_clock_transitions_hold_for_later_years);

    // Room-temp resolution against the real SharedSensorReadings struct
    RUN_TEST(test_room_resolve_credits_the_sensors_read_time_not_now);
    RUN_TEST(test_room_resolve_falls_back_to_now_before_the_driver_ever_wrote);
    RUN_TEST(test_room_resolve_walks_the_whole_ladder_on_a_pulled_sensor);
    RUN_TEST(test_room_resolve_survives_the_millis_rollover);
    RUN_TEST(test_room_resolve_real_struct_has_the_field_the_mock_lacked);

    // Frost at the burner, and the DEGRADED trigger (2026-10-26 review)
    RUN_TEST(test_policy_frost_latch_keeps_a_burn_alive_with_the_switch_off);
    RUN_TEST(test_policy_frost_never_overrides_a_disabled_boiler);
    RUN_TEST(test_power_fault_first_fault_must_not_degrade);

    // Debounce split + flow-limit ordering (2026-10-26)
    RUN_TEST(test_invalid_count_boiler_output_escalates_sooner_than_the_shared_count);
    RUN_TEST(test_flow_limits_inverted_pair_is_ordered_not_defaulted);

    // Sensor-side counters, on demand (2026-10-26)
    RUN_TEST(test_report_no_device_is_not_a_zero_reading);
    RUN_TEST(test_report_distinguishes_stock_firmware_from_a_failed_read);
    RUN_TEST(test_report_always_emits_zero_failures_explicitly);
    RUN_TEST(test_report_carries_the_firmware_slot_and_uptime);
    RUN_TEST(test_report_failure_breakdown_is_published);
    RUN_TEST(test_report_fits_the_payload_with_every_counter_set);
    RUN_TEST(test_report_marks_truncation_instead_of_losing_the_tail);
    RUN_TEST(test_report_refuses_rather_than_emit_half_a_payload);
    RUN_TEST(test_report_publishes_negative_temperatures_signed);

    // The curve was anchored on the live room reading, giving the room shift the wrong sign
    RUN_TEST(test_anchor_room_at_target_is_identical_to_the_old_behaviour);
    RUN_TEST(test_anchor_colder_room_now_raises_the_flow_target);
    RUN_TEST(test_anchor_sensitivity_is_exactly_minus_the_factor);
    RUN_TEST(test_anchor_recovery_case_measured_on_the_live_config);
    RUN_TEST(test_anchor_room_mode_is_untouched);
    RUN_TEST(test_anchor_substituted_room_gets_no_deviation);
    RUN_TEST(test_anchor_zero_factor_disables_only_the_deviation);
    RUN_TEST(test_anchor_shift_cannot_overflow_int16);
    RUN_TEST(test_anchor_commanded_flow_is_clamped_not_the_raw_sensitivity);

    // A dead outside sensor must not stop space heating (2026-10-28)
    RUN_TEST(test_outside_valid_reading_is_used_and_recorded);
    RUN_TEST(test_outside_dropout_holds_the_last_reading_so_the_curve_keeps_working);
    RUN_TEST(test_outside_hold_lasts_an_hour_then_expires);
    RUN_TEST(test_outside_gone_falls_back_to_the_room_not_to_no_heat);
    RUN_TEST(test_outside_gone_and_no_usable_room_is_the_only_no_heat_case);
    RUN_TEST(test_outside_hold_survives_the_millis_rollover);
    RUN_TEST(test_room_thermostat_predicate_is_asymmetric);
    RUN_TEST(test_room_thermostat_fallback_would_have_heated_the_house);

    // The burn classification that was computed in three places (2026-10-29)
    RUN_TEST(test_burnkind_no_frost_is_never_frost_only);
    RUN_TEST(test_burnkind_frost_with_switch_off_is_frost_only);
    RUN_TEST(test_burnkind_frost_with_switch_on_but_nothing_wanted_is_frost_only);
    RUN_TEST(test_burnkind_frost_during_real_space_heating_is_not_frost_only);
    RUN_TEST(test_burnkind_summer_override_makes_it_frost_only);
    RUN_TEST(test_burnkind_the_three_old_predicates_disagreed_exactly_here);
    RUN_TEST(test_burnkind_published_state_round_trips_and_clears);
    RUN_TEST(test_burnkind_room_measurement_defaults_true);

    return UNITY_END();
}