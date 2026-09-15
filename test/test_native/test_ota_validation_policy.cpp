/**
 * @file test_ota_validation_policy.cpp
 * @brief Unit tests for OtaValidationPolicy (confirm or roll back an OTA-updated image)
 */

#include <unity.h>

#include "../../include/utils/OtaValidationPolicy.h"

// setUp and tearDown are defined in test_main.cpp

using OtaValidationPolicy::Action;
using OtaValidationPolicy::MAX_WAIT_MS;
using OtaValidationPolicy::MIN_UPTIME_MS;
using OtaValidationPolicy::decide;

void test_ota_validation_waits_for_min_uptime() {
    // Healthy but too early: a crash in the first minute must still roll back
    TEST_ASSERT_TRUE(decide(0, true, true) == Action::WAIT);
    TEST_ASSERT_TRUE(decide(MIN_UPTIME_MS - 1, true, true) == Action::WAIT);
    TEST_ASSERT_TRUE(decide(MIN_UPTIME_MS, true, true) == Action::MARK_VALID);
}

void test_ota_validation_needs_sensors_and_network() {
    TEST_ASSERT_TRUE(decide(MIN_UPTIME_MS, false, true) == Action::WAIT);
    TEST_ASSERT_TRUE(decide(MIN_UPTIME_MS, true, false) == Action::WAIT);
    TEST_ASSERT_TRUE(decide(MAX_WAIT_MS - 1, false, false) == Action::WAIT);
}

void test_ota_validation_rolls_back_after_max_wait() {
    TEST_ASSERT_TRUE(decide(MAX_WAIT_MS, false, true) == Action::ROLLBACK);
    TEST_ASSERT_TRUE(decide(MAX_WAIT_MS, true, false) == Action::ROLLBACK);
    // Healthy late still confirms instead of rolling back
    TEST_ASSERT_TRUE(decide(MAX_WAIT_MS + 1, true, true) == Action::MARK_VALID);
}
