/**
 * @file test_sensor_failure_confirm.cpp
 * @brief Unit tests for SensorFailureConfirm (burner stop on persistently missing sensors)
 */

#include <unity.h>

#include "../../include/modules/control/SensorFailureConfirm.h"

// setUp and tearDown are defined in test_main.cpp

using SensorFailureConfirm::CONFIRM_MS;
using SensorFailureConfirm::State;
using SensorFailureConfirm::shouldStop;

void test_sensor_failure_single_glitch_does_not_stop() {
    State s;
    TEST_ASSERT_FALSE(shouldStop(s, false, true, 1000));   // first invalid reading
    TEST_ASSERT_FALSE(shouldStop(s, true, true, 3500));    // next reading valid again
    TEST_ASSERT_FALSE(shouldStop(s, false, true, 6000));   // new glitch restarts the timer
    TEST_ASSERT_FALSE(shouldStop(s, false, true, 6000 + CONFIRM_MS - 1));
}

void test_sensor_failure_persistent_with_demand_stops() {
    State s;
    TEST_ASSERT_FALSE(shouldStop(s, false, true, 1000));
    TEST_ASSERT_FALSE(shouldStop(s, false, true, 3500));
    TEST_ASSERT_TRUE(shouldStop(s, false, true, 1000 + CONFIRM_MS));
}

void test_sensor_failure_without_demand_never_stops() {
    State s;
    TEST_ASSERT_FALSE(shouldStop(s, false, false, 1000));
    TEST_ASSERT_FALSE(shouldStop(s, false, false, 1000 + CONFIRM_MS * 3));
    // Demand arriving later starts the confirmation from then
    TEST_ASSERT_FALSE(shouldStop(s, false, true, 50000));
    TEST_ASSERT_FALSE(shouldStop(s, false, true, 50000 + CONFIRM_MS - 1));
    TEST_ASSERT_TRUE(shouldStop(s, false, true, 50000 + CONFIRM_MS));
}

void test_sensor_failure_millis_wrap() {
    State s;
    const uint32_t start = 0xFFFFFFFFu - 2000u;
    TEST_ASSERT_FALSE(shouldStop(s, false, true, start));
    TEST_ASSERT_FALSE(shouldStop(s, false, true, start + 5000u));  // wrapped
    TEST_ASSERT_TRUE(shouldStop(s, false, true, start + CONFIRM_MS));
}
