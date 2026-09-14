/**
 * @file test_stage_c_policies.cpp
 * @brief Unit tests for power-level fault escalation and water limit consistency
 */

#include <unity.h>
#include <cstdint>

#include "../../include/modules/control/BurnerTransitionPolicy.h"
#include "../../include/modules/control/WaterChargePolicy.h"

// setUp and tearDown are defined in test_main.cpp

using BurnerTransitionPolicy::POWER_FAULT_WINDOW_MS;
using BurnerTransitionPolicy::recordPowerFault;

void test_power_fault_escalates_on_third_fault_in_window() {
    uint8_t count = 0;
    uint32_t windowStart = 0;
    // A single refused power change only stops the burner
    TEST_ASSERT_FALSE(recordPowerFault(count, windowStart, 1000));
    // Repeats after the post-purge restart
    TEST_ASSERT_FALSE(recordPowerFault(count, windowStart, 60000));
    TEST_ASSERT_TRUE(recordPowerFault(count, windowStart, 120000));
}

void test_power_fault_window_restarts_after_ten_minutes() {
    uint8_t count = 0;
    uint32_t windowStart = 0;
    TEST_ASSERT_FALSE(recordPowerFault(count, windowStart, 1000));
    TEST_ASSERT_FALSE(recordPowerFault(count, windowStart, 2000));
    // Window over: counting starts again
    TEST_ASSERT_FALSE(recordPowerFault(count, windowStart, 1000 + POWER_FAULT_WINDOW_MS));
    TEST_ASSERT_FALSE(recordPowerFault(count, windowStart, 2000 + POWER_FAULT_WINDOW_MS));
    TEST_ASSERT_TRUE(recordPowerFault(count, windowStart, 3000 + POWER_FAULT_WINDOW_MS));
}

void test_water_limits_valid_only_when_low_below_high() {
    TEST_ASSERT_TRUE(WaterChargePolicy::limitsValid(400, 500));
    TEST_ASSERT_FALSE(WaterChargePolicy::limitsValid(500, 500));
    // 2026-09-14: low set to 60.0 while high was still 50.0
    TEST_ASSERT_FALSE(WaterChargePolicy::limitsValid(600, 500));
    TEST_ASSERT_FALSE(WaterChargePolicy::limitsValid(0, 500));
    TEST_ASSERT_FALSE(WaterChargePolicy::limitsValid(400, 0));
}
