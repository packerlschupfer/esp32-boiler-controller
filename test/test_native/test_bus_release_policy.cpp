/**
 * @file test_bus_release_policy.cpp
 * @brief Unit tests for BusReleasePolicy (deliberate RS485 hand-over, 2026-10-25)
 */

#include <unity.h>
#include <cstdint>
#include <cstring>

#include "../../include/modules/control/BusReleasePolicy.h"
#include "../../include/modules/control/BurnerSMState.h"

// setUp and tearDown are defined in test_main.cpp

using namespace BusReleasePolicy;

void test_bus_release_window_is_clamped_and_capped() {
    // 0 means "unspecified" and must take the default, NOT the minimum - otherwise a bare
    // "bus_release" would hand over a 30 s window and the tool run would be cut off.
    TEST_ASSERT_EQUAL_UINT32(DEFAULT_SECONDS, clampSeconds(0));
    TEST_ASSERT_EQUAL_UINT32(MIN_SECONDS, clampSeconds(1));
    TEST_ASSERT_EQUAL_UINT32(MIN_SECONDS, clampSeconds(MIN_SECONDS));
    TEST_ASSERT_EQUAL_UINT32(120, clampSeconds(120));
    // The cap is the point: no caller can park the controller out of service.
    TEST_ASSERT_EQUAL_UINT32(MAX_SECONDS, clampSeconds(MAX_SECONDS));
    TEST_ASSERT_EQUAL_UINT32(MAX_SECONDS, clampSeconds(MAX_SECONDS + 1));
    TEST_ASSERT_EQUAL_UINT32(MAX_SECONDS, clampSeconds(86400));
    TEST_ASSERT_EQUAL_UINT32(MAX_SECONDS, clampSeconds(0xFFFFFFFFu));
    TEST_ASSERT_TRUE_MESSAGE(MAX_SECONDS <= 900, "the hard cap must stay short");
}

// Shorthand for the quiescent case: idle state machine, no latched emergency stop.
static Refusal quiet(uint8_t relays, bool demand) {
    return mayRelease(relays, demand, BurnerSMState::IDLE, false);
}

void test_bus_release_refuses_while_the_boiler_is_busy() {
    // Entry REFUSES rather than orchestrating a shutdown: there is no new stop path to get
    // wrong. The relay mask is the hardware-verified one.
    const uint8_t burner = 1u << RelayIndex::BURNER_ENABLE;
    const uint8_t water  = 1u << RelayIndex::WATER_MODE;
    const uint8_t pump   = 1u << RelayIndex::HEATING_PUMP;

    TEST_ASSERT_EQUAL(Refusal::BURNER_ACTIVE, quiet(burner, false));
    TEST_ASSERT_EQUAL(Refusal::BURNER_ACTIVE, quiet(water, false));
    TEST_ASSERT_EQUAL(Refusal::BURNER_ACTIVE, quiet(burner | water | pump, false));

    // Demand precedes the relays by a pre-purge, so clear firing bits are not enough.
    TEST_ASSERT_EQUAL(Refusal::DEMAND_PRESENT, quiet(0, true));
    TEST_ASSERT_EQUAL(Refusal::DEMAND_PRESENT, quiet(pump, true));

    // Fully idle: allowed. This is the only state that is.
    TEST_ASSERT_EQUAL(Refusal::NONE, quiet(0, false));
}

void test_bus_release_refuses_while_a_pump_is_dissipating_heat() {
    // THE REGRESSION TEST for the review's finding against my own feature. This case used to
    // assert the opposite - "a running pump is harmless and the window is short" - which was
    // wrong in the one way that matters: withholding the RYN4 ticks makes the DELAY watchdog
    // de-energise the pumps within 10 s, so a release during POST_PURGE, the pumpCooldownMs
    // overrun, the emergency-dissipation hold or ReturnPreheater's cycling traps residual heat
    // in the exchanger with no flow. The relay mask is asked instead of those four mechanisms
    // because their state is task-local in PumpControlModule, and because "is anything
    // energised that the watchdog would kill" is the actual question - it covers reasons added
    // later for free.
    TEST_ASSERT_EQUAL(Refusal::PUMP_ACTIVE, quiet(1u << RelayIndex::HEATING_PUMP, false));
    TEST_ASSERT_EQUAL(Refusal::PUMP_ACTIVE, quiet(1u << RelayIndex::WATER_PUMP, false));
    TEST_ASSERT_EQUAL(Refusal::PUMP_ACTIVE,
                      quiet((1u << RelayIndex::HEATING_PUMP) | (1u << RelayIndex::WATER_PUMP), false));

    // The pump mask must be the two pumps and nothing else. VALVE is excluded deliberately:
    // it is a diverter, so losing it returns flow to the default circuit rather than trapping
    // heat - if someone adds it here, that is a decision, not a typo.
    TEST_ASSERT_EQUAL_UINT8((1u << RelayIndex::HEATING_PUMP) | (1u << RelayIndex::WATER_PUMP),
                            PUMP_MASK);
    TEST_ASSERT_EQUAL_UINT8(0, PUMP_MASK & FIRING_MASK);
    TEST_ASSERT_EQUAL_UINT8(0, PUMP_MASK & (1u << RelayIndex::VALVE));

    // Firing still outranks a pump, so the operator is told the most urgent reason.
    TEST_ASSERT_EQUAL(Refusal::BURNER_ACTIVE,
                      quiet((1u << RelayIndex::BURNER_ENABLE) | (1u << RelayIndex::HEATING_PUMP), false));
    // Relays that are neither firing nor pumps do not block: an alarm or the spare is not a
    // thermal hazard, and the boost relay without BURNER_ENABLE commands nothing.
    TEST_ASSERT_EQUAL(Refusal::NONE, quiet(1u << RelayIndex::ALARM, false));
    TEST_ASSERT_EQUAL(Refusal::NONE, quiet(1u << RelayIndex::SPARE_7, false));
    TEST_ASSERT_EQUAL(Refusal::NONE, quiet(1u << RelayIndex::POWER_BOOST, false));
    TEST_ASSERT_EQUAL(Refusal::NONE, quiet(1u << RelayIndex::VALVE, false));
}

void test_bus_release_refuses_mid_sequence_and_under_a_latched_estop() {
    // Every state with relay writes due on the next tick must refuse - above all POST_PURGE,
    // where the burner is already off so the firing mask says nothing.
    const BurnerSMState busy[] = {BurnerSMState::PRE_PURGE, BurnerSMState::IGNITION,
                                  BurnerSMState::RUNNING_LOW, BurnerSMState::RUNNING_HIGH,
                                  BurnerSMState::MODE_SWITCHING, BurnerSMState::POST_PURGE};
    for (BurnerSMState st : busy) {
        TEST_ASSERT_TRUE(isSequencing(st));
        TEST_ASSERT_EQUAL(Refusal::BURNER_SEQUENCING, mayRelease(0, false, st, false));
    }

    // IDLE is quiescent, and so are the two fault states: the burner is off and no sequence
    // is running, and a lockout after a failed ignition is a plausible moment to go and work
    // on the bus. Any dissipation still under way is caught by the pump mask instead.
    const BurnerSMState quietStates[] = {BurnerSMState::IDLE, BurnerSMState::LOCKOUT,
                                         BurnerSMState::ERROR};
    for (BurnerSMState st : quietStates) {
        TEST_ASSERT_FALSE(isSequencing(st));
        TEST_ASSERT_EQUAL(Refusal::NONE, mayRelease(0, false, st, false));
    }

    // A latched emergency stop refuses even once the pumps have stopped: the operator is
    // mid-diagnosis and emergency_reset's relay writes would not land until the window ended.
    TEST_ASSERT_EQUAL(Refusal::EMERGENCY_LATCHED, mayRelease(0, false, BurnerSMState::IDLE, true));
    TEST_ASSERT_EQUAL(Refusal::EMERGENCY_LATCHED, mayRelease(0, false, BurnerSMState::LOCKOUT, true));
    // ... but a thermal reason still outranks it, because that is the one with a deadline.
    TEST_ASSERT_EQUAL(Refusal::PUMP_ACTIVE,
                      mayRelease(1u << RelayIndex::HEATING_PUMP, false, BurnerSMState::IDLE, true));

    // The label is all the operator sees, so every value needs its own and none may fall
    // through to the "unknown" placeholder - which is what happens if a value is added to the
    // enum and not to the switch.
    const Refusal all[] = {Refusal::NONE, Refusal::BURNER_ACTIVE, Refusal::DEMAND_PRESENT,
                           Refusal::PUMP_ACTIVE, Refusal::BURNER_SEQUENCING,
                           Refusal::EMERGENCY_LATCHED};
    const size_t count = sizeof(all) / sizeof(all[0]);
    for (size_t i = 0; i < count; ++i) {
        const char* label = toString(all[i]);
        TEST_ASSERT_NOT_NULL(label);
        TEST_ASSERT_TRUE(label[0] != '\0');
        TEST_ASSERT_TRUE_MESSAGE(strcmp(label, "unknown") != 0, "a Refusal has no label");
        for (size_t j = i + 1; j < count; ++j) {
            TEST_ASSERT_TRUE_MESSAGE(strcmp(label, toString(all[j])) != 0,
                                     "two Refusal values share a label");
        }
    }
}

void test_bus_release_extending_a_window_never_shortens_it() {
    // The extend path stored `now + requested` unconditionally while its comment claimed a
    // second call "cannot shorten a window someone else is relying on" - so a 30 s request
    // against a 15-minute window cut it to 30 s. isLater() is what makes the caller keep the
    // later deadline, and it has to be rollover-safe for the same reason expired() is.
    TEST_ASSERT_TRUE(isLater(1000, 500));
    TEST_ASSERT_FALSE(isLater(500, 1000));
    TEST_ASSERT_FALSE(isLater(1000, 1000));

    // Across the uint32 wrap: a deadline just past the rollover really is later than one just
    // before it, where a plain `>` would get it backwards and allow the shortening once every
    // 49.7 days.
    const uint32_t beforeWrap = 0xFFFFF000u;
    const uint32_t afterWrap  = 0x00001000u;
    TEST_ASSERT_TRUE(isLater(afterWrap, beforeWrap));
    TEST_ASSERT_FALSE(isLater(beforeWrap, afterWrap));
}

void test_bus_release_firing_mask_tracks_the_relay_indices() {
    // The published gate on boiler/status/sensors is `r & 17`, which uses a DIFFERENT bit
    // order (bit0 burner, bit4 water mode) from RelayState (bit0 burner, bit2 water mode).
    // Deriving the mask from RelayIndex is what keeps the two from drifting apart, so pin
    // that it really is the burner-enable and water-mode bits and nothing else.
    TEST_ASSERT_EQUAL_UINT8((1u << RelayIndex::BURNER_ENABLE) | (1u << RelayIndex::WATER_MODE),
                            FIRING_MASK);
    TEST_ASSERT_EQUAL_UINT8(0, FIRING_MASK & (1u << RelayIndex::HEATING_PUMP));
    TEST_ASSERT_EQUAL_UINT8(0, FIRING_MASK & (1u << RelayIndex::WATER_PUMP));
    TEST_ASSERT_EQUAL_UINT8(0, FIRING_MASK & (1u << RelayIndex::POWER_BOOST));
}

void test_bus_release_expiry_survives_the_millis_rollover() {
    // THE LOAD-BEARING TEST. With the bus released there is no guaranteed path for a remote
    // "acquire", so the on-device deadline is the only thing that can end the window. A
    // plain `now >= deadline` breaks across the 49.7-day millis() rollover, and the way it
    // breaks matters: it would extend the window by ~49 days.
    const uint32_t nearEnd = 0xFFFFFF00u;          // 256 ms before the rollover
    const uint32_t deadline = nearEnd + 600000u;   // 10 minutes later, i.e. wrapped
    TEST_ASSERT_FALSE_MESSAGE(expired(nearEnd, deadline), "not expired before the wrap");
    TEST_ASSERT_FALSE_MESSAGE(expired(50u, deadline), "not expired just after the wrap");
    TEST_ASSERT_FALSE(expired(deadline - 1u, deadline));
    TEST_ASSERT_TRUE_MESSAGE(expired(deadline, deadline), "expires exactly at the deadline");
    TEST_ASSERT_TRUE(expired(deadline + 1u, deadline));

    // And the ordinary case, away from the boundary.
    TEST_ASSERT_FALSE(expired(1000u, 601000u));
    TEST_ASSERT_TRUE(expired(601000u, 601000u));
    TEST_ASSERT_TRUE(expired(601001u, 601000u));
}

void test_bus_release_seconds_left_rounds_up_and_never_contradicts_released() {
    // Reporting 0 while still released would make the status topic contradict itself, so a
    // part-second remainder rounds UP.
    TEST_ASSERT_EQUAL_UINT32(600, secondsLeft(0u, 600000u));
    TEST_ASSERT_EQUAL_UINT32(1, secondsLeft(599800u, 600000u));
    TEST_ASSERT_EQUAL_UINT32(1, secondsLeft(599999u, 600000u));
    TEST_ASSERT_EQUAL_UINT32(0, secondsLeft(600000u, 600000u));
    TEST_ASSERT_EQUAL_UINT32(0, secondsLeft(700000u, 600000u));
    // Rollover again: a wrapped deadline must not report a 49-day remainder.
    TEST_ASSERT_EQUAL_UINT32(600, secondsLeft(0xFFFFFF00u, 0xFFFFFF00u + 600000u));
}

void test_bus_release_safe_mask_clears_firing_and_keeps_pumps() {
    // Imposed before bus access resumes, so the first resumed write cannot energise the
    // burner on a decision taken from readings that went stale during the window.
    const uint8_t all = 0xFF;
    TEST_ASSERT_EQUAL_UINT8(0, safeDesiredMask(all) & FIRING_MASK);
    TEST_ASSERT_EQUAL_UINT8(1u << RelayIndex::HEATING_PUMP,
                            safeDesiredMask(1u << RelayIndex::HEATING_PUMP));
    TEST_ASSERT_EQUAL_UINT8(0, safeDesiredMask(FIRING_MASK));
    // Everything that is not a firing bit survives untouched.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(all & ~FIRING_MASK), safeDesiredMask(all));
    TEST_ASSERT_EQUAL_UINT8(0, safeDesiredMask(0));
}
