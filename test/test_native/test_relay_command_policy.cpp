/**
 * @file test_relay_command_policy.cpp
 * @brief Unit tests for RelayCommandPolicy (per-relay decision of RelayControlTask)
 */

#include <unity.h>
#include <cstring>

#include "../../include/shared/RelayCommandPolicy.h"

// setUp and tearDown are defined in test_main.cpp

using namespace RelayCommandPolicy;

namespace {

uint32_t lastToggle[8];
uint32_t toggles[8];
uint32_t pumpTimers[2];

bool pumpAllows = true;
unsigned pumpChecks = 0;
uint8_t pumpCheckRelay = 0;
bool pumpCheckState = false;

bool pumpProtection(uint8_t relayIndex, bool state) {
    pumpChecks++;
    pumpCheckRelay = relayIndex;
    pumpCheckState = state;
    return pumpAllows;
}

// Values of MIN_RELAY_SWITCH_INTERVAL_MS / MAX_RELAY_TOGGLE_RATE_PER_MIN at 1 ms ticks
RateLimiter makeLimiter() {
    RateLimiter limiter = { 150, 30, lastToggle, toggles };
    return limiter;
}

void resetRelayState() {
    std::memset(lastToggle, 0, sizeof(lastToggle));
    std::memset(toggles, 0, sizeof(toggles));
    std::memset(pumpTimers, 0, sizeof(pumpTimers));
    pumpAllows = true;
    pumpChecks = 0;
    pumpCheckRelay = 0;
    pumpCheckState = false;
}

// RelayControlTask::setRelayState() -> processSingleRelay() with the policy calls
// in the same order; desiredMask stands in for g_relayState.desired.
bool sendRelay(uint8_t& desiredMask, uint8_t relayIndex, bool state, uint32_t nowTicks) {
    const bool desiredState = desiredBit(desiredMask, relayIndex);
    if (skipDuplicate(relayIndex, desiredState, state)) {
        return true;
    }
    // Mirrors processSingleRelay(): gateToggle() is the shared lock-side decision, and
    // the pump check runs outside the critical section.
    const ToggleGate gate = gateToggle(relayIndex, desiredState, state, false,
                                       makeLimiter(), nowTicks);
    if (gate == ToggleGate::RATE_LIMITED) {
        return false;
    }
    if (gate == ToggleGate::ALLOWED && !pumpProtection(relayIndex, state)) {
        return false;
    }
    const uint8_t mask = static_cast<uint8_t>(1u << (relayIndex - 1));
    desiredMask = state ? static_cast<uint8_t>(desiredMask | mask)
                        : static_cast<uint8_t>(desiredMask & ~mask);
    restartPumpTimer(relayIndex, !isNoOp(desiredState, state), nowTicks, pumpTimers);
    return true;
}


#define ASSERT_ADMISSION(expected, actual) \
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(actual))

}  // namespace

void test_relay_policy_gate_toggle_expectations() {
    // A HAND-WRITTEN table, not a comparison against another copy of the implementation.
    //
    // This replaces test_relay_policy_decomposed_matches_admit, which compared admit() to
    // a local decomposedAdmit() that was a character-for-character copy of admit()'s body.
    // Any change to gateToggle moved both sides together, so it could not fail. Two real
    // regressions were shown to pass it: gateToggle returning ALLOWED instead of
    // NOT_APPLICABLE (which sends no-op AND EMERGENCY commands to pump protection - the F13
    // hazard), and evaluating consumeToggle before appliesProtection (the 2026-09-14
    // emergency-stop regression). Both are caught below by stated expectations.
    //
    // KNOWN GAP, stated rather than implied: env:native_test sets no test_build_src, so
    // src/ is not compiled here at all. Nothing in this file constrains
    // RelayControlTask::processSingleRelay() - not its statement order, not its handling of
    // each ToggleGate value, not the placement of portENTER_CRITICAL. What is pinned is
    // gateToggle(), which processSingleRelay() and admit() both call.
    struct Case {
        const char* what;
        uint8_t relay; bool desired, requested, emergency;
        uint32_t lastToggle; uint32_t count;      // limiter pre-state for this relay
        uint32_t now;
        ToggleGate expect;
    };
    const Case cases[] = {
        // A command that does not change the desired state is not a toggle, emergency or
        // not. If this ever returns ALLOWED, pump protection starts seeing no-ops.
        {"no-op OFF->OFF",            4, false, false, false,    0,  0, 5000, ToggleGate::NOT_APPLICABLE},
        {"no-op ON->ON",              4, true,  true,  false,    0,  0, 5000, ToggleGate::NOT_APPLICABLE},
        {"no-op, emergency",          4, true,  true,  true,     0,  0, 5000, ToggleGate::NOT_APPLICABLE},
        // Emergency bypasses both protections even for a real change - F13.
        {"real change, emergency",    4, false, true,  true,     0,  0, 5000, ToggleGate::NOT_APPLICABLE},
        // Ordinary real change on a fresh limiter.
        {"real change, fresh",        4, false, true,  false,    0,  0, 5000, ToggleGate::ALLOWED},
        // Minimum interval is 150 ticks: 149 refuses, 150 allows.
        {"inside min interval",       4, false, true,  false, 4851,  0, 5000, ToggleGate::RATE_LIMITED},
        {"exactly min interval",      4, false, true,  false, 4850,  0, 5000, ToggleGate::ALLOWED},
        // Per-window cap is 30.
        {"at window cap",             4, false, true,  false, 1000, 30, 5000, ToggleGate::RATE_LIMITED},
        {"one below window cap",      4, false, true,  false, 1000, 29, 5000, ToggleGate::ALLOWED},
        // Invalid indices are never allowed, and must not be reported as "not applicable".
        {"relay 0 invalid",           0, false, true,  false,    0,  0, 5000, ToggleGate::RATE_LIMITED},
        {"relay 9 invalid",           9, false, true,  false,    0,  0, 5000, ToggleGate::RATE_LIMITED},
    };

    for (const Case& c : cases) {
        resetRelayState();
        if (c.relay >= 1 && c.relay <= 8) {
            lastToggle[c.relay - 1] = c.lastToggle;
            toggles[c.relay - 1] = c.count;
        }
        const ToggleGate got = gateToggle(c.relay, c.desired, c.requested, c.emergency,
                                          makeLimiter(), c.now);
        if (got != c.expect) {
            TEST_FAIL_MESSAGE(c.what);
        }
        // A NOT_APPLICABLE outcome must leave the limiter untouched - that is what makes a
        // batch of unchanged relays free.
        if (c.expect == ToggleGate::NOT_APPLICABLE && c.relay >= 1 && c.relay <= 8) {
            TEST_ASSERT_EQUAL_UINT32(c.count, toggles[c.relay - 1]);
            TEST_ASSERT_EQUAL_UINT32(c.lastToggle, lastToggle[c.relay - 1]);
        }
    }
}

void test_relay_policy_admit_passes_requested_state_to_pump_check() {
    // admit() has no production caller - processSingleRelay() calls gateToggle() directly
    // and then checkPumpProtection() outside the lock. admit() is the testable composition
    // of those two steps. Pin the ARGUMENTS it forwards, not just the call count: swapping
    // requestedState for desiredState leaves the count, the return value and the limiter
    // arrays all identical, so a count-only assertion cannot see it.
    resetRelayState();
    pumpAllows = true;
    ASSERT_ADMISSION(Admission::ACCEPTED,
                     admit(5, false, true, false, makeLimiter(), 5000, &pumpProtection));
    TEST_ASSERT_EQUAL_UINT(1, pumpChecks);
    TEST_ASSERT_EQUAL_UINT8(5, pumpCheckRelay);
    TEST_ASSERT_TRUE(pumpCheckState);          // requestedState, not desiredState

    resetRelayState();
    pumpAllows = true;
    ASSERT_ADMISSION(Admission::ACCEPTED,
                     admit(6, true, false, false, makeLimiter(), 5000, &pumpProtection));
    TEST_ASSERT_EQUAL_UINT(1, pumpChecks);
    TEST_ASSERT_EQUAL_UINT8(6, pumpCheckRelay);
    TEST_ASSERT_FALSE(pumpCheckState);         // would be true if desiredState leaked through
}

void test_relay_policy_window_reset_readmits() {
    // updateRateLimitCounters() memsets toggleCount every minute (one of the three things
    // rateLimitMux guards). Nothing exercised the limiter's response to that clear, so a
    // change that stopped re-admitting after a window rollover would not have been caught.
    resetRelayState();
    RateLimiter limiter = makeLimiter();
    const uint8_t relayIndex = 3;
    const uint8_t idx = relayIndex - 1;

    // Reach the cap, spacing toggles past the 150 tick minimum interval.
    uint32_t now = 1000;
    for (uint32_t i = 0; i < 30; i++) {
        TEST_ASSERT_TRUE(consumeToggle(limiter, relayIndex, now));
        now += 200;
    }
    TEST_ASSERT_EQUAL_UINT32(30, toggles[idx]);

    // At the cap: refused even though the interval is satisfied.
    now += 200;
    TEST_ASSERT_FALSE(consumeToggle(limiter, relayIndex, now));
    TEST_ASSERT_EQUAL_UINT32(30, toggles[idx]);

    // Simulate the minute rollover. NOTE: this is the TEST's own memset, not production's -
    // updateRateLimitCounters() lives in RelayControlTask.cpp, which native tests do not
    // compile, so the 60 s threshold, the wrap arithmetic and "only toggleCount is cleared"
    // are NOT pinned here. An earlier version asserted that memset-ing `toggles` left
    // `lastToggle` unchanged, which is two different arrays and cannot fail. What is worth
    // pinning is consumeToggle's behaviour once the counter is cleared, below.
    const uint32_t lastToggleBeforeReset = lastToggle[idx];
    std::memset(toggles, 0, sizeof(toggles));

    // Re-admitted, and the minimum interval still applies across the boundary.
    TEST_ASSERT_FALSE(consumeToggle(limiter, relayIndex, lastToggleBeforeReset + 100));
    now = lastToggleBeforeReset + 200;
    TEST_ASSERT_TRUE(consumeToggle(limiter, relayIndex, now));
    TEST_ASSERT_EQUAL_UINT32(1, toggles[idx]);
}

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

void test_relay_policy_desired_bit_is_zero_based() {
    // Relay numbers are 1-based, the RelayState::desired bitmask 0-based
    TEST_ASSERT_TRUE(desiredBit(0x01, 1));
    TEST_ASSERT_FALSE(desiredBit(0x01, 2));
    TEST_ASSERT_TRUE(desiredBit(0x02, 2));
    TEST_ASSERT_FALSE(desiredBit(0x02, 1));
    TEST_ASSERT_FALSE(desiredBit(0x02, 3));
    TEST_ASSERT_TRUE(desiredBit(0x80, 8));
    TEST_ASSERT_FALSE(desiredBit(0x7F, 8));
}

void test_relay_policy_invalid_relay_index_rejected() {
    // Relays are 1-8: index 0 would shift by -1 and index the limiter arrays at 255
    TEST_ASSERT_FALSE(isValidRelay(0));
    TEST_ASSERT_TRUE(isValidRelay(1));
    TEST_ASSERT_TRUE(isValidRelay(8));
    TEST_ASSERT_FALSE(isValidRelay(9));
    TEST_ASSERT_FALSE(desiredBit(0xFF, 0));
    TEST_ASSERT_FALSE(desiredBit(0xFF, 9));

    resetRelayState();
    const RateLimiter limiter = makeLimiter();
    TEST_ASSERT_FALSE(consumeToggle(limiter, 0, 1000));
    TEST_ASSERT_FALSE(consumeToggle(limiter, 9, 1000));
    ASSERT_ADMISSION(Admission::RATE_LIMITED, admit(0, false, true, false, limiter, 1000, &pumpProtection));
    ASSERT_ADMISSION(Admission::RATE_LIMITED, admit(9, false, true, false, limiter, 1000, &pumpProtection));
    TEST_ASSERT_EQUAL_UINT(0, pumpChecks);
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT32(0, toggles[i]);
        TEST_ASSERT_EQUAL_UINT32(0, lastToggle[i]);
    }

    // The first and last valid relays are still counted
    TEST_ASSERT_TRUE(consumeToggle(limiter, 1, 1000));
    TEST_ASSERT_TRUE(consumeToggle(limiter, 8, 1000));
    TEST_ASSERT_EQUAL_UINT32(1, toggles[0]);
    TEST_ASSERT_EQUAL_UINT32(1, toggles[7]);
}

void test_relay_policy_duplicate_off_to_burner_relays_not_skipped() {
    // F26: a redundant OFF to BURNER_ENABLE, POWER_BOOST or WATER_MODE is re-asserted
    TEST_ASSERT_FALSE(skipDuplicate(1, false, false));
    TEST_ASSERT_FALSE(skipDuplicate(2, false, false));
    TEST_ASSERT_FALSE(skipDuplicate(3, false, false));
    // Other relays and redundant ON commands are skipped
    TEST_ASSERT_TRUE(skipDuplicate(4, false, false));
    TEST_ASSERT_TRUE(skipDuplicate(5, false, false));
    TEST_ASSERT_TRUE(skipDuplicate(6, false, false));
    TEST_ASSERT_TRUE(skipDuplicate(8, false, false));
    TEST_ASSERT_TRUE(skipDuplicate(1, true, true));
    TEST_ASSERT_TRUE(skipDuplicate(5, true, true));
    // Real changes are never skipped
    TEST_ASSERT_FALSE(skipDuplicate(2, false, true));
    TEST_ASSERT_FALSE(skipDuplicate(5, true, false));
}

// Replays 2026-09-14 15:41:25: HEATING HALF -> WATER HALF batch (BURNER_ENABLE OFF,
// POWER_BOOST re-sent OFF, WATER_MODE ON), then POWER_BOOST ON 2 ms later.
void test_relay_policy_batch_resend_then_power_on_accepted() {
    resetRelayState();
    uint8_t desired = 0x01;  // BURNER_ENABLE on

    TEST_ASSERT_TRUE(sendRelay(desired, 1, false, 1000));
    TEST_ASSERT_TRUE(sendRelay(desired, 2, false, 1000));  // not skipped (burner relay OFF)
    TEST_ASSERT_TRUE(sendRelay(desired, 3, true, 1000));
    TEST_ASSERT_EQUAL_UINT32(0, toggles[1]);               // re-sent OFF not counted
    TEST_ASSERT_EQUAL_UINT32(0, lastToggle[1]);

    TEST_ASSERT_TRUE(sendRelay(desired, 2, true, 1002));
    TEST_ASSERT_EQUAL_HEX8(0x06, desired);
    TEST_ASSERT_EQUAL_UINT32(1, toggles[0]);
    TEST_ASSERT_EQUAL_UINT32(1, toggles[1]);
    TEST_ASSERT_EQUAL_UINT32(1, toggles[2]);
    TEST_ASSERT_EQUAL_UINT32(1002, lastToggle[1]);
    TEST_ASSERT_EQUAL_UINT(3, pumpChecks);                 // asked for real changes only

    // Before the fix the re-sent OFF was counted, which rejects the power change
    resetRelayState();
    const RateLimiter limiter = makeLimiter();
    TEST_ASSERT_TRUE(consumeToggle(limiter, 2, 1000));
    ASSERT_ADMISSION(Admission::RATE_LIMITED,
                     admit(2, false, true, false, limiter, 1002, &pumpProtection));
}

void test_relay_policy_rate_limiter_interval_and_window() {
    resetRelayState();
    const RateLimiter limiter = makeLimiter();

    ASSERT_ADMISSION(Admission::ACCEPTED, admit(4, false, true, false, limiter, 500, &pumpProtection));
    TEST_ASSERT_EQUAL_UINT32(1, toggles[3]);
    TEST_ASSERT_EQUAL_UINT32(500, lastToggle[3]);

    // Within the minimum interval: rejected and not counted
    ASSERT_ADMISSION(Admission::RATE_LIMITED, admit(4, true, false, false, limiter, 649, &pumpProtection));
    TEST_ASSERT_EQUAL_UINT32(1, toggles[3]);
    TEST_ASSERT_EQUAL_UINT32(500, lastToggle[3]);
    ASSERT_ADMISSION(Admission::ACCEPTED, admit(4, true, false, false, limiter, 650, &pumpProtection));
    TEST_ASSERT_EQUAL_UINT32(2, toggles[3]);
    TEST_ASSERT_EQUAL_UINT32(0, toggles[4]);  // other relays untouched

    // Toggles per window
    toggles[3] = 30;
    ASSERT_ADMISSION(Admission::RATE_LIMITED, admit(4, false, true, false, limiter, 10000, &pumpProtection));
    TEST_ASSERT_EQUAL_UINT32(30, toggles[3]);
    TEST_ASSERT_EQUAL_UINT32(650, lastToggle[3]);

    // Interval across tick counter wrap
    toggles[3] = 0;
    lastToggle[3] = 0xFFFFFFF0UL;
    ASSERT_ADMISSION(Admission::RATE_LIMITED, admit(4, false, true, false, limiter, 0x85, &pumpProtection));
    ASSERT_ADMISSION(Admission::ACCEPTED, admit(4, false, true, false, limiter, 0x86, &pumpProtection));
}

void test_relay_policy_noop_and_emergency_skip_limiter_and_pump_check() {
    resetRelayState();
    const RateLimiter limiter = makeLimiter();
    lastToggle[1] = 1000;
    toggles[1] = 5;

    ASSERT_ADMISSION(Admission::ACCEPTED, admit(2, false, false, false, limiter, 1001, &pumpProtection));
    ASSERT_ADMISSION(Admission::ACCEPTED, admit(2, false, true, true, limiter, 1001, &pumpProtection));
    toggles[1] = 30;
    ASSERT_ADMISSION(Admission::ACCEPTED, admit(2, true, false, true, limiter, 1001, &pumpProtection));
    TEST_ASSERT_EQUAL_UINT32(30, toggles[1]);
    TEST_ASSERT_EQUAL_UINT32(1000, lastToggle[1]);
    TEST_ASSERT_EQUAL_UINT(0, pumpChecks);

    // The same real change without bypass is limited
    ASSERT_ADMISSION(Admission::RATE_LIMITED, admit(2, false, true, false, limiter, 1001, &pumpProtection));
}

void test_relay_policy_pump_protection_checked_after_rate_limit() {
    resetRelayState();
    const RateLimiter limiter = makeLimiter();
    pumpAllows = false;

    ASSERT_ADMISSION(Admission::PUMP_PROTECTED, admit(5, false, true, false, limiter, 2000, &pumpProtection));
    TEST_ASSERT_EQUAL_UINT(1, pumpChecks);
    TEST_ASSERT_EQUAL_UINT8(5, pumpCheckRelay);
    TEST_ASSERT_TRUE(pumpCheckState);
    // Counted by the rate limiter although pump protection then refused it
    TEST_ASSERT_EQUAL_UINT32(1, toggles[4]);
    TEST_ASSERT_EQUAL_UINT32(2000, lastToggle[4]);

    // A rate-limited command does not reach pump protection
    ASSERT_ADMISSION(Admission::RATE_LIMITED, admit(5, false, true, false, limiter, 2001, &pumpProtection));
    TEST_ASSERT_EQUAL_UINT(1, pumpChecks);
}

void test_relay_policy_pump_timer_only_on_real_change() {
    uint32_t timers[2] = { 7, 7 };

    TEST_ASSERT_EQUAL_INT(NO_PUMP, restartPumpTimer(5, false, 100, timers));
    TEST_ASSERT_EQUAL_UINT32(7, timers[0]);

    TEST_ASSERT_EQUAL_INT(0, restartPumpTimer(5, true, 100, timers));   // heating pump
    TEST_ASSERT_EQUAL_UINT32(100, timers[0]);
    TEST_ASSERT_EQUAL_UINT32(7, timers[1]);

    TEST_ASSERT_EQUAL_INT(1, restartPumpTimer(6, true, 200, timers));   // water pump
    TEST_ASSERT_EQUAL_UINT32(200, timers[1]);

    TEST_ASSERT_EQUAL_INT(NO_PUMP, restartPumpTimer(2, true, 300, timers));
    TEST_ASSERT_EQUAL_INT(NO_PUMP, restartPumpTimer(4, true, 300, timers));
    TEST_ASSERT_EQUAL_UINT32(100, timers[0]);
    TEST_ASSERT_EQUAL_UINT32(200, timers[1]);

    // Emergency re-send of a running pump (setRelayStateEmergency skips dedup)
    resetRelayState();
    pumpTimers[0] = 50;
    ASSERT_ADMISSION(Admission::ACCEPTED,
                     admit(5, true, true, true, makeLimiter(), 400, &pumpProtection));
    TEST_ASSERT_EQUAL_INT(NO_PUMP, restartPumpTimer(5, !isNoOp(true, true), 400, pumpTimers));
    TEST_ASSERT_EQUAL_UINT32(50, pumpTimers[0]);
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
