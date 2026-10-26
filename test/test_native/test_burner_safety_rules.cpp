/**
 * @file test_burner_safety_rules.cpp
 * @brief Unit tests for BurnerSafetyRules (Layer-1 checks of BurnerSafetyValidator)
 */

#include <unity.h>

#include "../../include/modules/control/BurnerSafetyRules.h"

// setUp and tearDown are defined in test_main.cpp

using BurnerSafetyRules::Check;
using BurnerSafetyRules::Inputs;
using BurnerSafetyRules::ValidationResult;

namespace {

bool interlocksClosed = true;
unsigned interlockCalls = 0;

bool interlockProbe() {
    interlockCalls++;
    return interlocksClosed;
}

// Space heating at 60 C, fresh MB8ART data, 1.50 BAR, limits as in the firmware
Inputs safeInputs() {
    interlocksClosed = true;
    interlockCalls = 0;

    Inputs in;
    in.boilerOutput = 600;
    in.boilerOutputValid = true;
    in.boilerReturn = 450;
    in.boilerReturnValid = true;
    in.waterTank = 450;
    in.waterTankValid = true;
    in.systemPressure = 150;
    in.systemPressureValid = true;
    in.lastBoilerTempUpdateMs = 100000;
    in.nowMs = 102500;
    in.sensorStaleMs = 60000;
    in.maxBoilerTemp = SystemConstants::Temperature::MAX_BOILER_TEMP_C;
    in.maxWaterTemp = 650;
    in.minRequiredSensors = 2;
    return in;
}

Check evaluate(const Inputs& in, bool isWaterMode = false) {
    return BurnerSafetyRules::evaluate(in, isWaterMode, &interlockProbe);
}

#define ASSERT_CHECK(expected, actual) \
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(actual))

}  // namespace

void test_safety_rules_safe_readings_pass() {
    Inputs in = safeInputs();
    ASSERT_CHECK(Check::PASSED, evaluate(in));
    ASSERT_CHECK(Check::PASSED, evaluate(in, true));
    ASSERT_CHECK(ValidationResult::SAFE_TO_OPERATE, BurnerSafetyRules::toResult(evaluate(in)));
    TEST_ASSERT_EQUAL_UINT(3, interlockCalls);
}

void test_safety_rules_emergency_stop_checked_first() {
    Inputs in = safeInputs();
    in.emergencyStopActive = true;
    in.boilerOutput = 1200;
    in.boilerReturnValid = false;
    in.waterTankValid = false;
    in.systemPressureValid = false;
    ASSERT_CHECK(Check::EMERGENCY_STOP, evaluate(in, true));
    TEST_ASSERT_EQUAL_UINT(0, interlockCalls);
}

void test_safety_rules_sensor_ranges_and_count() {
    Inputs in = safeInputs();
    TEST_ASSERT_EQUAL_UINT8(3, BurnerSafetyRules::countInRangeSensors(in));

    // Boiler sensors -50.0..150.0 C, tank sensor -50.0..100.0 C (inclusive)
    in.boilerOutput = 1500;
    in.boilerReturn = -500;
    in.waterTank = 1000;
    TEST_ASSERT_EQUAL_UINT8(3, BurnerSafetyRules::countInRangeSensors(in));
    in.boilerOutput = 1501;
    TEST_ASSERT_EQUAL_UINT8(2, BurnerSafetyRules::countInRangeSensors(in));
    in.boilerReturn = -501;
    TEST_ASSERT_EQUAL_UINT8(1, BurnerSafetyRules::countInRangeSensors(in));
    in.waterTank = 1001;
    TEST_ASSERT_EQUAL_UINT8(0, BurnerSafetyRules::countInRangeSensors(in));

    // Invalid flags do not count; one valid sensor fails before the temperature limit
    in = safeInputs();
    in.boilerOutput = 1200;
    in.boilerReturnValid = false;
    in.waterTankValid = false;
    ASSERT_CHECK(Check::INSUFFICIENT_SENSORS, evaluate(in));
    ASSERT_CHECK(ValidationResult::INSUFFICIENT_SENSORS,
                 BurnerSafetyRules::toResult(Check::INSUFFICIENT_SENSORS));

    // An out-of-range output with the valid flag set is not counted but still
    // checked against the boiler limit
    in = safeInputs();
    in.boilerOutput = 1600;
    ASSERT_CHECK(Check::BOILER_TEMP_HIGH, evaluate(in));
}

void test_safety_rules_stale_data_invalidates_all_sensors() {
    Inputs in = safeInputs();
    in.nowMs = in.lastBoilerTempUpdateMs + 60000;       // exactly sensorStaleMs: fresh
    TEST_ASSERT_FALSE(BurnerSafetyRules::sensorDataStale(in));
    ASSERT_CHECK(Check::PASSED, evaluate(in));

    in.nowMs = in.lastBoilerTempUpdateMs + 60001;
    TEST_ASSERT_TRUE(BurnerSafetyRules::sensorDataStale(in));
    TEST_ASSERT_EQUAL_UINT8(0, BurnerSafetyRules::validSensorCount(in));
    ASSERT_CHECK(Check::INSUFFICIENT_SENSORS, evaluate(in));

    // Staleness is checked before the temperature limit
    in.boilerOutput = 1200;
    ASSERT_CHECK(Check::INSUFFICIENT_SENSORS, evaluate(in));

    // Never updated (timestamp 0) is not treated as stale
    in = safeInputs();
    in.lastBoilerTempUpdateMs = 0;
    in.nowMs = 3600000;
    ASSERT_CHECK(Check::PASSED, evaluate(in));

    // Across millis() wrap
    in.lastBoilerTempUpdateMs = 0xFFFFF000UL;
    in.nowMs = 0x1000;
    TEST_ASSERT_FALSE(BurnerSafetyRules::sensorDataStale(in));
}

void test_safety_rules_boiler_limit_inclusive() {
    Inputs in = safeInputs();
    in.boilerReturn = 1000;               // keep the differential small
    in.boilerOutput = in.maxBoilerTemp - 1;
    ASSERT_CHECK(Check::PASSED, evaluate(in));
    in.boilerOutput = in.maxBoilerTemp;   // the limit itself triggers
    ASSERT_CHECK(Check::BOILER_TEMP_HIGH, evaluate(in));
    ASSERT_CHECK(Check::BOILER_TEMP_HIGH, evaluate(in, true));
    ASSERT_CHECK(ValidationResult::TEMPERATURE_EXCEEDED, BurnerSafetyRules::toResult(evaluate(in)));

    // Invalid output: no limit check (return and tank still give 2 sensors)
    in.boilerOutput = 1200;
    in.boilerOutputValid = false;
    ASSERT_CHECK(Check::PASSED, evaluate(in));
}

void test_safety_rules_water_limit_only_in_water_mode() {
    Inputs in = safeInputs();
    in.waterTank = in.maxWaterTemp;
    ASSERT_CHECK(Check::PASSED, evaluate(in, false));
    ASSERT_CHECK(Check::WATER_TEMP_HIGH, evaluate(in, true));
    ASSERT_CHECK(ValidationResult::TEMPERATURE_EXCEEDED,
                 BurnerSafetyRules::toResult(Check::WATER_TEMP_HIGH));

    in.waterTank = in.maxWaterTemp - 1;
    ASSERT_CHECK(Check::PASSED, evaluate(in, true));

    in.waterTank = 900;
    in.waterTankValid = false;
    ASSERT_CHECK(Check::PASSED, evaluate(in, true));

    // Boiler limit is checked before the tank limit
    in = safeInputs();
    in.boilerReturn = 1000;
    in.boilerOutput = in.maxBoilerTemp;
    in.waterTank = 900;
    ASSERT_CHECK(Check::BOILER_TEMP_HIGH, evaluate(in, true));
}

void test_safety_rules_pressure_bounds() {
    Inputs in = safeInputs();
    in.systemPressure = 99;
    ASSERT_CHECK(Check::PRESSURE_LOW, evaluate(in));
    in.systemPressure = 100;
    ASSERT_CHECK(Check::PASSED, evaluate(in));
    in.systemPressure = 350;
    ASSERT_CHECK(Check::PASSED, evaluate(in));
    in.systemPressure = 351;
    ASSERT_CHECK(Check::PRESSURE_HIGH, evaluate(in));
    ASSERT_CHECK(ValidationResult::PRESSURE_EXCEEDED, BurnerSafetyRules::toResult(Check::PRESSURE_LOW));
    ASSERT_CHECK(ValidationResult::PRESSURE_EXCEEDED, BurnerSafetyRules::toResult(Check::PRESSURE_HIGH));

    // Temperature limits come first; pressure failures stop before the interlocks
    interlockCalls = 0;
    in.systemPressure = 50;
    in.waterTank = 700;
    ASSERT_CHECK(Check::WATER_TEMP_HIGH, evaluate(in, true));
    ASSERT_CHECK(Check::PRESSURE_LOW, evaluate(in, false));
    TEST_ASSERT_EQUAL_UINT(0, interlockCalls);
}

void test_safety_rules_missing_pressure_blocks_unless_allowed() {
    Inputs in = safeInputs();
    in.systemPressureValid = false;
    Check check = evaluate(in);
    ASSERT_CHECK(Check::PRESSURE_MISSING, check);
    ASSERT_CHECK(ValidationResult::SENSOR_FAILURE, BurnerSafetyRules::toResult(check));
    TEST_ASSERT_FALSE(BurnerSafetyRules::pressureCheckPassed(check));
    TEST_ASSERT_EQUAL_UINT(0, interlockCalls);

    // ALLOW_NO_PRESSURE_SENSOR build: continues with the remaining checks
    in.allowMissingPressure = true;
    check = evaluate(in);
    ASSERT_CHECK(Check::PASSED, check);
    TEST_ASSERT_TRUE(BurnerSafetyRules::pressureCheckPassed(check));
    in.boilerReturn = 200;
    ASSERT_CHECK(Check::THERMAL_SHOCK, evaluate(in));
}

void test_safety_rules_missing_pressure_checked_after_sensors_and_limits() {
    // A missing pressure reading only blocks demand (SENSOR_FAILURE); checked earlier it
    // would hide an over-temperature (emergency stop) or a sensor count failure
    Inputs in = safeInputs();
    in.systemPressureValid = false;
    TEST_ASSERT_FALSE(in.allowMissingPressure);
    in.boilerReturn = 1000;               // keep the differential small
    in.boilerOutput = in.maxBoilerTemp;
    Check check = evaluate(in);
    ASSERT_CHECK(Check::BOILER_TEMP_HIGH, check);
    ASSERT_CHECK(ValidationResult::TEMPERATURE_EXCEEDED, BurnerSafetyRules::toResult(check));

    in = safeInputs();
    in.systemPressureValid = false;
    in.waterTank = in.maxWaterTemp;
    ASSERT_CHECK(Check::WATER_TEMP_HIGH, evaluate(in, true));

    // Too few sensors
    in = safeInputs();
    in.systemPressureValid = false;
    in.boilerReturnValid = false;
    in.waterTankValid = false;
    check = evaluate(in);
    ASSERT_CHECK(Check::INSUFFICIENT_SENSORS, check);
    ASSERT_CHECK(ValidationResult::INSUFFICIENT_SENSORS, BurnerSafetyRules::toResult(check));

    // Stale MB8ART data
    in = safeInputs();
    in.systemPressureValid = false;
    in.nowMs = in.lastBoilerTempUpdateMs + 60001;
    ASSERT_CHECK(Check::INSUFFICIENT_SENSORS, evaluate(in));
    TEST_ASSERT_EQUAL_UINT(0, interlockCalls);
}

void test_safety_rules_interlock_open_before_thermal_shock() {
    Inputs in = safeInputs();
    in.boilerReturn = 200;  // 40.0 C differential
    interlocksClosed = false;
    const Check check = evaluate(in);
    ASSERT_CHECK(Check::HARDWARE_INTERLOCK_OPEN, check);
    ASSERT_CHECK(ValidationResult::HARDWARE_INTERLOCK_OPEN, BurnerSafetyRules::toResult(check));
    TEST_ASSERT_TRUE(BurnerSafetyRules::pressureCheckPassed(check));
    TEST_ASSERT_EQUAL_UINT(1, interlockCalls);
}

void test_safety_rules_thermal_shock_above_35c() {
    TEST_ASSERT_EQUAL_INT16(350, SystemConstants::Safety::ReturnPreheat::MAX_DIFFERENTIAL);

    Inputs in = safeInputs();
    in.boilerOutput = 800;
    in.boilerReturn = 450;   // 35.0 C: allowed
    ASSERT_CHECK(Check::PASSED, evaluate(in));
    in.boilerReturn = 449;   // 35.1 C
    ASSERT_CHECK(Check::THERMAL_SHOCK, evaluate(in));
    ASSERT_CHECK(Check::THERMAL_SHOCK, evaluate(in, true));
    ASSERT_CHECK(ValidationResult::THERMAL_SHOCK_RISK, BurnerSafetyRules::toResult(Check::THERMAL_SHOCK));

    // Needs both boiler sensors valid
    in.boilerReturnValid = false;
    ASSERT_CHECK(Check::PASSED, evaluate(in));

    // Return hotter than output
    in = safeInputs();
    in.boilerOutput = 300;
    in.boilerReturn = 800;
    ASSERT_CHECK(Check::PASSED, evaluate(in));
}

void test_safety_rules_result_codes() {
    ASSERT_CHECK(ValidationResult::EMERGENCY_STOP_ACTIVE, BurnerSafetyRules::toResult(Check::EMERGENCY_STOP));
    ASSERT_CHECK(ValidationResult::INSUFFICIENT_SENSORS, BurnerSafetyRules::toResult(Check::INSUFFICIENT_SENSORS));
    ASSERT_CHECK(ValidationResult::TEMPERATURE_EXCEEDED, BurnerSafetyRules::toResult(Check::BOILER_TEMP_HIGH));
    ASSERT_CHECK(ValidationResult::TEMPERATURE_EXCEEDED, BurnerSafetyRules::toResult(Check::WATER_TEMP_HIGH));
    ASSERT_CHECK(ValidationResult::PRESSURE_EXCEEDED, BurnerSafetyRules::toResult(Check::PRESSURE_LOW));
    ASSERT_CHECK(ValidationResult::PRESSURE_EXCEEDED, BurnerSafetyRules::toResult(Check::PRESSURE_HIGH));
    ASSERT_CHECK(ValidationResult::SENSOR_FAILURE, BurnerSafetyRules::toResult(Check::PRESSURE_MISSING));
    ASSERT_CHECK(ValidationResult::HARDWARE_INTERLOCK_OPEN, BurnerSafetyRules::toResult(Check::HARDWARE_INTERLOCK_OPEN));
    ASSERT_CHECK(ValidationResult::THERMAL_SHOCK_RISK, BurnerSafetyRules::toResult(Check::THERMAL_SHOCK));
    ASSERT_CHECK(ValidationResult::SAFE_TO_OPERATE, BurnerSafetyRules::toResult(Check::PASSED));

    TEST_ASSERT_FALSE(BurnerSafetyRules::pressureCheckPassed(Check::EMERGENCY_STOP));
    TEST_ASSERT_FALSE(BurnerSafetyRules::pressureCheckPassed(Check::WATER_TEMP_HIGH));
    TEST_ASSERT_FALSE(BurnerSafetyRules::pressureCheckPassed(Check::PRESSURE_HIGH));
    TEST_ASSERT_TRUE(BurnerSafetyRules::pressureCheckPassed(Check::THERMAL_SHOCK));
    TEST_ASSERT_TRUE(BurnerSafetyRules::pressureCheckPassed(Check::PASSED));
}
