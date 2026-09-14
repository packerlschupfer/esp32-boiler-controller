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
