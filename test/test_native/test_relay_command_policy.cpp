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

// processSingleRelay()'s composition, built from the SAME RelayCommandPolicy::gateToggle()
// the production path calls - not a retyped copy of admit()'s body. Before 2026-09-18 this
// was a third hand-written copy of the decision, so it could not detect drift in either
// real caller. The residual limit is unchanged and worth stating: the native build does not
// compile RelayControlTask.cpp, so this pins the SHARED GATE, not the statement order in
// processSingleRelay().
Admission decomposedAdmit(uint8_t relayIndex, bool desiredState, bool requestedState,
                          bool emergencyBypass, const RateLimiter& limiter,
                          uint32_t nowTicks, bool (*pumpProtectionAllows)(uint8_t, bool)) {
    switch (gateToggle(relayIndex, desiredState, requestedState, emergencyBypass,
                       limiter, nowTicks)) {
        case ToggleGate::NOT_APPLICABLE:
            return Admission::ACCEPTED;
        case ToggleGate::RATE_LIMITED:
            return Admission::RATE_LIMITED;
        case ToggleGate::ALLOWED:
            break;
    }
    if (!pumpProtectionAllows(relayIndex, requestedState)) {
        return Admission::PUMP_PROTECTED;
    }
    return Admission::ACCEPTED;
}

#define ASSERT_ADMISSION(expected, actual) \
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(actual))

}  // namespace

void test_relay_policy_decomposed_matches_admit() {
    // processSingleRelay() no longer calls admit(): the rate-limiter lock must cover
    // consumeToggle() alone, because checkPumpProtection() takes a mutex and cannot run
    // inside portENTER_CRITICAL. That leaves two copies of the same decision, which is
    // exactly the "limiter re-implemented" trap (io-6). Pin them together: for every
    // input combination the decomposed sequence must return the same Admission AND
    // leave the same side effects in the limiter arrays.
    const bool values[2] = {false, true};

    // Limiter pre-state matters: with a zeroed limiter consumeToggle() ALWAYS succeeds,
    // so a loop over a single nowTicks never produces RATE_LIMITED for a valid relay -
    // precisely the case the rate-limiter lock exists for. Seed the three reachable
    // shapes: fresh, a toggle inside the 150 tick minimum interval, and the per-window
    // cap already reached.
    enum PreState { FRESH, WITHIN_MIN_INTERVAL, AT_WINDOW_CAP, PRE_STATE_COUNT };
    const uint32_t nowTicks = 5000;

    auto seed = [&](PreState pre, uint8_t relayIndex) {
        resetRelayState();
        if (relayIndex < 1 || relayIndex > 8) {
            return;
        }
        const uint8_t idx = static_cast<uint8_t>(relayIndex - 1);
        if (pre == WITHIN_MIN_INTERVAL) {
            lastToggle[idx] = nowTicks - 100;  // 100 < 150 tick minimum
        } else if (pre == AT_WINDOW_CAP) {
            lastToggle[idx] = nowTicks - 10000;  // interval fine, cap is what refuses
            toggles[idx] = 30;                   // maxTogglesPerWindow
        }
    };

    bool sawRateLimitedOnValidRelay = false;
    bool sawPumpProtected = false;

    for (uint8_t relayIndex = 0; relayIndex <= 9; relayIndex++) {
        for (int pre = 0; pre < PRE_STATE_COUNT; pre++) {
            for (int d = 0; d < 2; d++) {
                for (int r = 0; r < 2; r++) {
                    for (int e = 0; e < 2; e++) {
                        for (int p = 0; p < 2; p++) {
                            seed(static_cast<PreState>(pre), relayIndex);
                            pumpAllows = values[p];
                            const Admission viaAdmit =
                                admit(relayIndex, values[d], values[r], values[e],
                                      makeLimiter(), nowTicks, &pumpProtection);
                            uint32_t admitLastToggle[8];
                            uint32_t admitToggles[8];
                            std::memcpy(admitLastToggle, lastToggle, sizeof(lastToggle));
                            std::memcpy(admitToggles, toggles, sizeof(toggles));
                            const unsigned admitPumpChecks = pumpChecks;
                            const uint8_t admitPumpRelay = pumpCheckRelay;
                            const bool admitPumpState = pumpCheckState;

                            seed(static_cast<PreState>(pre), relayIndex);
                            pumpAllows = values[p];
                            const Admission viaSteps =
                                decomposedAdmit(relayIndex, values[d], values[r], values[e],
                                                makeLimiter(), nowTicks, &pumpProtection);

                            ASSERT_ADMISSION(viaAdmit, viaSteps);
                            TEST_ASSERT_EQUAL_UINT(admitPumpChecks, pumpChecks);
                            // The ARGUMENTS passed to pump protection must match too, not
                            // just the call count: swapping requestedState for desiredState
                            // leaves count, return value and limiter arrays identical.
                            TEST_ASSERT_EQUAL_UINT8(admitPumpRelay, pumpCheckRelay);
                            TEST_ASSERT_EQUAL(admitPumpState, pumpCheckState);
                            TEST_ASSERT_EQUAL_MEMORY(admitLastToggle, lastToggle,
                                                     sizeof(lastToggle));
                            TEST_ASSERT_EQUAL_MEMORY(admitToggles, toggles, sizeof(toggles));

                            if (viaAdmit == Admission::RATE_LIMITED && relayIndex >= 1 &&
                                relayIndex <= 8) {
                                sawRateLimitedOnValidRelay = true;
                            }
                            if (viaAdmit == Admission::PUMP_PROTECTED) {
                                sawPumpProtected = true;
                            }
                        }
                    }
                }
            }
        }
    }

    // Guard against the equivalence check silently degenerating to the ACCEPTED path.
    TEST_ASSERT_TRUE_MESSAGE(sawRateLimitedOnValidRelay,
                             "no RATE_LIMITED case generated for a valid relay");
    TEST_ASSERT_TRUE_MESSAGE(sawPumpProtected, "no PUMP_PROTECTED case generated");
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

    // The minute rollover clears the counter, and only the counter.
    const uint32_t lastToggleBeforeReset = lastToggle[idx];
    std::memset(toggles, 0, sizeof(toggles));
    TEST_ASSERT_EQUAL_UINT32(lastToggleBeforeReset, lastToggle[idx]);

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
