/**
 * @file test_emergency_stop_release.cpp
 * @brief Unit tests for EmergencyStopRelease (manual release of a latched emergency stop)
 */

#include <unity.h>
#include <cstring>

#include "../../include/modules/control/EmergencyStopRelease.h"

// setUp and tearDown are defined in test_main.cpp

using namespace EmergencyStopRelease;

namespace {
Conditions allClear() {
    Conditions c;
    c.emergencyActive = true;
    c.temperaturesOk = true;
    c.sensorsOk = true;
    c.systemErrorsClear = true;
    return c;
}
} // namespace

void test_emergency_release_when_causes_cleared() {
    TEST_ASSERT_TRUE(evaluate(allClear()) == Result::RELEASED);
    TEST_ASSERT_EQUAL_STRING("emergency_released", toString(Result::RELEASED));
}

void test_emergency_release_not_active() {
    Conditions c = allClear();
    c.emergencyActive = false;
    TEST_ASSERT_TRUE(evaluate(c) == Result::NOT_ACTIVE);
}

void test_emergency_release_refused_while_hot() {
    // 2026-09-14: a 115 °C trip must not be releasable while the boiler is still hot
    Conditions c = allClear();
    c.temperaturesOk = false;
    c.sensorsOk = false;  // temperature is reported first
    TEST_ASSERT_TRUE(evaluate(c) == Result::TEMPERATURE_HIGH);
}

void test_emergency_release_refused_on_sensor_or_system_errors() {
    Conditions c = allClear();
    c.sensorsOk = false;
    TEST_ASSERT_TRUE(evaluate(c) == Result::SENSORS_UNAVAILABLE);
    c = allClear();
    c.systemErrorsClear = false;
    TEST_ASSERT_TRUE(evaluate(c) == Result::SYSTEM_ERRORS);
    TEST_ASSERT_TRUE(std::strncmp(toString(Result::SYSTEM_ERRORS), "emergency_release_refused", 25) == 0);
}

void test_emergency_stop_onset_once_per_latch() {
    // 2026-09-14: the burner task read-and-cleared the bit; it now reacts once per onset
    bool wasSet = false;
    TEST_ASSERT_FALSE(onsetDetected(false, wasSet));
    TEST_ASSERT_TRUE(onsetDetected(true, wasSet));
    TEST_ASSERT_FALSE(onsetDetected(true, wasSet));   // still latched: no repeat
    TEST_ASSERT_FALSE(onsetDetected(false, wasSet));  // released
    TEST_ASSERT_TRUE(onsetDetected(true, wasSet));    // new emergency stop
}

void test_emergency_dissipation_until_boiler_cooled() {
    TEST_ASSERT_TRUE(dissipationPumpOn(true, 1150, true));
    TEST_ASSERT_TRUE(dissipationPumpOn(true, 600, true));
    TEST_ASSERT_FALSE(dissipationPumpOn(true, 599, true));
    // Hysteresis: once stopped, on again only from 65.0 °C
    TEST_ASSERT_FALSE(dissipationPumpOn(true, 640, false));
    TEST_ASSERT_TRUE(dissipationPumpOn(true, 650, false));
}

void test_emergency_sensor_recovery_releases_only_stale_sensor_stop() {
    // 2026-09-14: sensor recovery cleared EMERGENCY_STOP whatever had set it
    TEST_ASSERT_TRUE(releasableBySensorRecovery(true, Cause::SENSOR_STALE));
    TEST_ASSERT_FALSE(releasableBySensorRecovery(true, Cause::OTHER));
    TEST_ASSERT_FALSE(releasableBySensorRecovery(true, Cause::NONE));
    TEST_ASSERT_FALSE(releasableBySensorRecovery(false, Cause::SENSOR_STALE));
}

void test_emergency_cause_merge_while_latched() {
    TEST_ASSERT_TRUE(mergeCause(false, Cause::OTHER, Cause::SENSOR_STALE) == Cause::SENSOR_STALE);
    TEST_ASSERT_TRUE(mergeCause(true, Cause::NONE, Cause::SENSOR_STALE) == Cause::SENSOR_STALE);
    TEST_ASSERT_TRUE(mergeCause(true, Cause::SENSOR_STALE, Cause::SENSOR_STALE) == Cause::SENSOR_STALE);
    // Stale sensors, then critical temperature while latched: not sensor-releasable
    TEST_ASSERT_TRUE(mergeCause(true, Cause::SENSOR_STALE, Cause::OTHER) == Cause::OTHER);
    TEST_ASSERT_TRUE(mergeCause(true, Cause::OTHER, Cause::SENSOR_STALE) == Cause::OTHER);
}

void test_emergency_dissipation_without_usable_output() {
    TEST_ASSERT_TRUE(dissipationPumpOn(false, 200, false));
    TEST_ASSERT_TRUE(dissipationPumpOn(false, 200, true));
}
