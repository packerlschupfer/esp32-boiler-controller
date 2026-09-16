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
void test_space_heating_outside_threshold_start_and_stop_limits();
void test_space_heating_outside_noise_does_not_toggle();

// Error context (compact boiler/error/context JSON)
void test_error_context_worst_case_fits_mqtt_payload();
void test_error_context_escapes_and_null_sensors();
void test_error_context_small_buffer_shortens_description();

// OTA validation policy (confirm or roll back an updated image)
void test_ota_validation_waits_for_min_uptime();
void test_ota_validation_needs_sensors_and_network();
void test_ota_validation_rolls_back_after_max_wait();

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

// Scheduler command policy (enable command, space mode defaults, status reply)
void test_scheduler_enable_payload_validation();
void test_scheduler_enable_ends_active_run_only_when_disabled();
void test_scheduler_space_mode_default_targets();
void test_scheduler_status_lists_active_and_disabled_ids();
void test_scheduler_status_worst_case_fits_mqtt_payload();
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

    // Space heating policy (weather mode outside threshold hysteresis)
    RUN_TEST(test_space_heating_outside_threshold_start_and_stop_limits);
    RUN_TEST(test_space_heating_outside_noise_does_not_toggle);

    // Error context (compact boiler/error/context JSON)
    RUN_TEST(test_error_context_worst_case_fits_mqtt_payload);
    RUN_TEST(test_error_context_escapes_and_null_sensors);
    RUN_TEST(test_error_context_small_buffer_shortens_description);

    // OTA validation policy
    RUN_TEST(test_ota_validation_waits_for_min_uptime);
    RUN_TEST(test_ota_validation_needs_sensors_and_network);
    RUN_TEST(test_ota_validation_rolls_back_after_max_wait);

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

    // Scheduler command policy (enable command, space mode defaults, status reply)
    RUN_TEST(test_scheduler_enable_payload_validation);
    RUN_TEST(test_scheduler_enable_ends_active_run_only_when_disabled);
    RUN_TEST(test_scheduler_space_mode_default_targets);
    RUN_TEST(test_scheduler_status_lists_active_and_disabled_ids);
    RUN_TEST(test_scheduler_status_worst_case_fits_mqtt_payload);
    RUN_TEST(test_scheduler_status_small_buffer_stays_valid_json);

    return UNITY_END();
}