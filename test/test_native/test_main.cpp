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
void test_gate_handover_uses_temperature_when_decision_was_for_other_target();
void test_gate_fresh_matching_decision_wins_over_temperature();
void test_gate_without_boiler_temperature_control_task_arms();
void test_gate_pid_arms_demand_it_did_not_see_armed();
void test_gate_drops_demand_rearmed_while_coasting();
void test_gate_not_permitted_never_arms();
void test_gate_power_update_only_on_pid_change();
void test_gate_fallback_target_cap();

// Relay command policy (no-op commands vs rate limiting)
void test_relay_policy_noop_commands_skip_protection();
void test_relay_policy_real_changes_are_protected();
void test_relay_policy_emergency_bypasses_protection();
void test_relay_policy_mode_switch_then_power_change_counts_once();
void test_relay_policy_pump_request_resent_until_relay_follows();

void test_pre_ignition_safe_conditions();
void test_pre_ignition_high_boiler_temp();
void test_pre_ignition_high_water_temp();
void test_thermal_shock_detection();
void test_operation_safe_conditions();
void test_operation_high_exhaust_temp();
void test_operation_low_return_temp();
void test_rapid_temperature_rise();
void test_hardware_interlock_always_true();
void test_history_reset();

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

// Burner state machine tests
void test_bsm_initial_state_is_idle();
void test_bsm_heat_demand_triggers_pre_purge();
void test_bsm_pre_purge_to_ignition();
void test_bsm_ignition_success_low_power();
void test_bsm_ignition_success_high_power();
void test_bsm_ignition_timeout_retry();
void test_bsm_ignition_failures_cause_lockout();
void test_bsm_lockout_auto_reset();
void test_bsm_lockout_manual_reset();
void test_bsm_demand_removal_triggers_post_purge();
void test_bsm_post_purge_to_idle();
void test_bsm_emergency_stop();
void test_bsm_safety_failure_causes_error();
void test_bsm_flame_loss_causes_error();
void test_bsm_power_level_switching();
void test_bsm_no_start_without_safety();
void test_bsm_demand_removal_during_pre_purge();
void test_bsm_mode_switch_from_running_low();
void test_bsm_mode_switch_from_running_high();
void test_bsm_mode_switch_completes_to_running();
void test_bsm_mode_switch_no_demand_goes_to_post_purge();
void test_bsm_mode_switch_safety_failure_causes_error();
void test_bsm_mode_switch_flame_loss_causes_error();
void test_bsm_mode_switch_ignored_from_idle();

// Improvement 1: Power level failsafe tests
void test_bsm_power_level_mismatch_triggers_failsafe_low();
void test_bsm_power_level_mismatch_triggers_failsafe_high();

// Improvement 2: Helper function extraction tests
void test_bsm_helper_checkSafetyShutdown_no_demand();
void test_bsm_helper_checkSafetyShutdown_demand_active();
void test_bsm_helper_checkFlameLoss_unexpected();
void test_bsm_helper_checkFlameLoss_intentional();

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

// Safety cascade integration tests
void test_safety_cascade_all_pass();
void test_safety_cascade_validator_blocks_high_temp();
void test_safety_cascade_validator_blocks_insufficient_sensors();
void test_safety_cascade_validator_blocks_thermal_shock();
void test_safety_cascade_interlocks_emergency_stop();
void test_safety_cascade_interlocks_system_errors();
void test_safety_cascade_failsafe_triggered();
void test_safety_cascade_pressure_out_of_range();
void test_safety_cascade_stale_sensor_data();

// Mode switching tests
void test_mode_switching_idle_to_heating();
void test_mode_switching_heating_to_water();
void test_mode_switching_rejected_during_transition();
void test_mode_specific_water_temp_check();

// Progressive preheating tests
void test_preheating_skipped_low_differential();
void test_preheating_starts_high_differential();
void test_preheating_progressive_durations();
void test_preheating_pump_state_during_cycle();
void test_preheating_completes_after_cycles();

// Circuit breaker pattern tests
void test_circuit_breaker_first_failure_assume_safe();
void test_circuit_breaker_third_failure_triggers_failsafe();
void test_circuit_breaker_success_resets_counter();

// Combined scenario tests
void test_complete_heating_activation_workflow();
void test_water_heating_blocked_by_tank_temp();
void test_emergency_stop_cascade();

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
    
    // Burner safety tests
    RUN_TEST(test_pre_ignition_safe_conditions);
    RUN_TEST(test_pre_ignition_high_boiler_temp);
    RUN_TEST(test_pre_ignition_high_water_temp);
    RUN_TEST(test_thermal_shock_detection);
    RUN_TEST(test_operation_safe_conditions);
    RUN_TEST(test_operation_high_exhaust_temp);
    RUN_TEST(test_operation_low_return_temp);
    RUN_TEST(test_rapid_temperature_rise);
    RUN_TEST(test_hardware_interlock_always_true);
    RUN_TEST(test_history_reset);
    
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

    // Burner state machine tests
    RUN_TEST(test_bsm_initial_state_is_idle);
    RUN_TEST(test_bsm_heat_demand_triggers_pre_purge);
    RUN_TEST(test_bsm_pre_purge_to_ignition);
    RUN_TEST(test_bsm_ignition_success_low_power);
    RUN_TEST(test_bsm_ignition_success_high_power);
    RUN_TEST(test_bsm_ignition_timeout_retry);
    RUN_TEST(test_bsm_ignition_failures_cause_lockout);
    RUN_TEST(test_bsm_lockout_auto_reset);
    RUN_TEST(test_bsm_lockout_manual_reset);
    RUN_TEST(test_bsm_demand_removal_triggers_post_purge);
    RUN_TEST(test_bsm_post_purge_to_idle);
    RUN_TEST(test_bsm_emergency_stop);
    RUN_TEST(test_bsm_safety_failure_causes_error);
    RUN_TEST(test_bsm_flame_loss_causes_error);
    RUN_TEST(test_bsm_power_level_switching);
    RUN_TEST(test_bsm_no_start_without_safety);
    RUN_TEST(test_bsm_demand_removal_during_pre_purge);

    // MODE_SWITCHING tests
    RUN_TEST(test_bsm_mode_switch_from_running_low);
    RUN_TEST(test_bsm_mode_switch_from_running_high);
    RUN_TEST(test_bsm_mode_switch_completes_to_running);
    RUN_TEST(test_bsm_mode_switch_no_demand_goes_to_post_purge);
    RUN_TEST(test_bsm_mode_switch_safety_failure_causes_error);
    RUN_TEST(test_bsm_mode_switch_flame_loss_causes_error);
    RUN_TEST(test_bsm_mode_switch_ignored_from_idle);

    // Improvement 1: Power level failsafe tests
    RUN_TEST(test_bsm_power_level_mismatch_triggers_failsafe_low);
    RUN_TEST(test_bsm_power_level_mismatch_triggers_failsafe_high);

    // Improvement 2: Helper function extraction tests
    RUN_TEST(test_bsm_helper_checkSafetyShutdown_no_demand);
    RUN_TEST(test_bsm_helper_checkSafetyShutdown_demand_active);
    RUN_TEST(test_bsm_helper_checkFlameLoss_unexpected);
    RUN_TEST(test_bsm_helper_checkFlameLoss_intentional);

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

    // Safety cascade integration tests
    RUN_TEST(test_safety_cascade_all_pass);
    RUN_TEST(test_safety_cascade_validator_blocks_high_temp);
    RUN_TEST(test_safety_cascade_validator_blocks_insufficient_sensors);
    RUN_TEST(test_safety_cascade_validator_blocks_thermal_shock);
    RUN_TEST(test_safety_cascade_interlocks_emergency_stop);
    RUN_TEST(test_safety_cascade_interlocks_system_errors);
    RUN_TEST(test_safety_cascade_failsafe_triggered);
    RUN_TEST(test_safety_cascade_pressure_out_of_range);
    RUN_TEST(test_safety_cascade_stale_sensor_data);

    // Mode switching tests
    RUN_TEST(test_mode_switching_idle_to_heating);
    RUN_TEST(test_mode_switching_heating_to_water);
    RUN_TEST(test_mode_switching_rejected_during_transition);
    RUN_TEST(test_mode_specific_water_temp_check);

    // Progressive preheating tests
    RUN_TEST(test_preheating_skipped_low_differential);
    RUN_TEST(test_preheating_starts_high_differential);
    RUN_TEST(test_preheating_progressive_durations);
    RUN_TEST(test_preheating_pump_state_during_cycle);
    RUN_TEST(test_preheating_completes_after_cycles);

    // Circuit breaker pattern tests
    RUN_TEST(test_circuit_breaker_first_failure_assume_safe);
    RUN_TEST(test_circuit_breaker_third_failure_triggers_failsafe);
    RUN_TEST(test_circuit_breaker_success_resets_counter);

    // Combined scenario tests
    RUN_TEST(test_complete_heating_activation_workflow);
    RUN_TEST(test_water_heating_blocked_by_tank_temp);
    RUN_TEST(test_emergency_stop_cascade);

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
    RUN_TEST(test_gate_handover_uses_temperature_when_decision_was_for_other_target);
    RUN_TEST(test_gate_fresh_matching_decision_wins_over_temperature);
    RUN_TEST(test_gate_without_boiler_temperature_control_task_arms);
    RUN_TEST(test_gate_pid_arms_demand_it_did_not_see_armed);
    RUN_TEST(test_gate_drops_demand_rearmed_while_coasting);
    RUN_TEST(test_gate_not_permitted_never_arms);
    RUN_TEST(test_gate_power_update_only_on_pid_change);
    RUN_TEST(test_gate_fallback_target_cap);

    // Relay command policy (no-op commands vs rate limiting)
    RUN_TEST(test_relay_policy_noop_commands_skip_protection);
    RUN_TEST(test_relay_policy_real_changes_are_protected);
    RUN_TEST(test_relay_policy_emergency_bypasses_protection);
    RUN_TEST(test_relay_policy_mode_switch_then_power_change_counts_once);
    RUN_TEST(test_relay_policy_pump_request_resent_until_relay_follows);

    return UNITY_END();
}