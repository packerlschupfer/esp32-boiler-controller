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

// ErrorRecoveryManager tests
void test_erm_initial_state();
void test_erm_recovery_disabled();
void test_erm_unknown_error_fails();
void test_erm_in_progress_detection();
void test_erm_backoff_calculation();
void test_erm_backoff_max_delay_cap();
void test_erm_error_history_tracking();
void test_erm_error_history_expiration();
void test_erm_escalation_trigger();
void test_erm_emergency_stop_escalation();
void test_erm_stats_tracking();
void test_erm_clear_error_history();
void test_erm_custom_recovery_action();
void test_erm_multiple_components_isolated();
void test_erm_degrade_service_strategy();

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

    // ErrorRecoveryManager tests
    RUN_TEST(test_erm_initial_state);
    RUN_TEST(test_erm_recovery_disabled);
    RUN_TEST(test_erm_unknown_error_fails);
    RUN_TEST(test_erm_in_progress_detection);
    RUN_TEST(test_erm_backoff_calculation);
    RUN_TEST(test_erm_backoff_max_delay_cap);
    RUN_TEST(test_erm_error_history_tracking);
    RUN_TEST(test_erm_error_history_expiration);
    RUN_TEST(test_erm_escalation_trigger);
    RUN_TEST(test_erm_emergency_stop_escalation);
    RUN_TEST(test_erm_stats_tracking);
    RUN_TEST(test_erm_clear_error_history);
    RUN_TEST(test_erm_custom_recovery_action);
    RUN_TEST(test_erm_multiple_components_isolated);
    RUN_TEST(test_erm_degrade_service_strategy);

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

    return UNITY_END();
}