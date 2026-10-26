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

void test_ota_validation_needs_sensors_only() {
    // Sensors are the health signal, and the only one.
    TEST_ASSERT_TRUE(decide(MIN_UPTIME_MS, false, true) == Action::WAIT);
    TEST_ASSERT_TRUE(decide(MAX_WAIT_MS - 1, false, false) == Action::WAIT);
    // Sensors reading with no network is HEALTHY - this case used to WAIT and then roll back.
    TEST_ASSERT_TRUE(decide(MIN_UPTIME_MS, true, false) == Action::MARK_VALID);
}

void test_ota_validation_rolls_back_after_max_wait() {
    TEST_ASSERT_TRUE(decide(MAX_WAIT_MS, false, true) == Action::ROLLBACK);
    TEST_ASSERT_TRUE(decide(MAX_WAIT_MS, false, false) == Action::ROLLBACK);
    // Healthy late still confirms instead of rolling back
    TEST_ASSERT_TRUE(decide(MAX_WAIT_MS + 1, true, true) == Action::MARK_VALID);
}

void test_ota_validation_never_reboots_a_healthy_controller_over_a_dead_lan() {
    // THE REGRESSION TEST. Confirmation used to require networkConnected, so an Ethernet link
    // down between uptime 60 s and 600 s rolled back a perfectly healthy image and rebooted
    // the controller unattended - with no post-purge and no pump overrun if it happened
    // mid-burn. This controller is required to run with no network at all, so that window was
    // reachable on an ordinary LAN outage, and the rollback was pointless on its own terms:
    // the previous image boots into the same dead LAN and is no more reachable.
    //
    // Sweep the whole old failure window and past it: sensors reading and no network must
    // never produce a ROLLBACK, at any uptime.
    for (uint32_t t = MIN_UPTIME_MS; t <= MAX_WAIT_MS + 60000; t += 5000) {
        TEST_ASSERT_TRUE_MESSAGE(decide(t, true, false) == Action::MARK_VALID,
                                 "a healthy controller must not be rolled back for a dead LAN");
    }

    // And the network flag must not change ANY decision now - the parameter is kept only so
    // the caller can log it and so this history stays visible in the signature.
    for (uint32_t t = 0; t <= MAX_WAIT_MS + 60000; t += 7000) {
        TEST_ASSERT_TRUE(decide(t, true, true) == decide(t, true, false));
        TEST_ASSERT_TRUE(decide(t, false, true) == decide(t, false, false));
    }

    // What DOES still roll back: an image that boots but never reads a sensor, i.e. cannot
    // control the boiler. That is the one failure worth rebooting for.
    TEST_ASSERT_TRUE(decide(MAX_WAIT_MS, false, true) == Action::ROLLBACK);
}
