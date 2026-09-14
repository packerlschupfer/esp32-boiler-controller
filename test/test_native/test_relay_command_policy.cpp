/**
 * @file test_relay_command_policy.cpp
 * @brief Unit tests for RelayCommandPolicy (no-op relay commands vs rate limiting)
 */

#include <unity.h>

#include "../../include/shared/RelayCommandPolicy.h"

// setUp and tearDown are defined in test_main.cpp

using namespace RelayCommandPolicy;

void test_relay_policy_noop_commands_skip_protection() {
    // Unchanged relay (e.g. POWER_BOOST re-sent OFF in a mode-switch batch) must
    // not count against the rate limit, emergency or not.
    TEST_ASSERT_TRUE(isNoOp(false, false));
    TEST_ASSERT_TRUE(isNoOp(true, true));
    TEST_ASSERT_FALSE(appliesProtection(false, false, false));
    TEST_ASSERT_FALSE(appliesProtection(true, true, false));
}

void test_relay_policy_real_changes_are_protected() {
    TEST_ASSERT_FALSE(isNoOp(false, true));
    TEST_ASSERT_TRUE(appliesProtection(false, true, false));
    TEST_ASSERT_TRUE(appliesProtection(true, false, false));
}

void test_relay_policy_emergency_bypasses_protection() {
    TEST_ASSERT_FALSE(appliesProtection(false, true, true));
    TEST_ASSERT_FALSE(appliesProtection(true, false, true));
}

// Replays the 2026-09-14 sequence on POWER_BOOST: batch re-sends OFF (no-op),
// then 2 ms later the power change to ON. Only the real change may be counted,
// so a 150 ms minimum-interval limiter sees exactly one toggle and allows it.
void test_relay_policy_mode_switch_then_power_change_counts_once() {
    const unsigned minIntervalMs = 150;
    bool desired = false;
    long lastToggleMs = -100000;
    unsigned counted = 0;
    bool lastAllowed = false;

    struct Cmd { long t; bool state; };
    const Cmd seq[] = { {1000, false}, {1002, true} };

    for (const Cmd& c : seq) {
        if (!appliesProtection(desired, c.state, false)) {
            lastAllowed = true;   // no-op accepted
            continue;
        }
        if (c.t - lastToggleMs < static_cast<long>(minIntervalMs)) {
            lastAllowed = false;
            continue;
        }
        lastToggleMs = c.t;
        counted++;
        desired = c.state;
        lastAllowed = true;
    }

    TEST_ASSERT_EQUAL_UINT(1, counted);
    TEST_ASSERT_TRUE(lastAllowed);
    TEST_ASSERT_TRUE(desired);
}

void test_relay_policy_pump_request_resent_until_relay_follows() {
    // Review 2026-09-14: a pump change refused by motor protection was never repeated
    TEST_ASSERT_FALSE(pumpRequestResendDue(true, true, 10000, 0));    // relay follows
    TEST_ASSERT_FALSE(pumpRequestResendDue(false, false, 10000, 0));
    TEST_ASSERT_FALSE(pumpRequestResendDue(true, false, 1999, 0));    // too soon
    TEST_ASSERT_TRUE(pumpRequestResendDue(true, false, 2000, 0));     // refused ON
    TEST_ASSERT_TRUE(pumpRequestResendDue(false, true, 9000, 5000));  // failsafe forced ON
    TEST_ASSERT_TRUE(pumpRequestResendDue(true, false, 1000, 0xFFFFFC18UL));  // across millis() wrap
}
