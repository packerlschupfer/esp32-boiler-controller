/**
 * @file test_test_mode_policy.cpp
 * @brief Unit tests for TestModePolicy - the manual relay window (2026-11-12).
 *
 * The feature is "no interlocks on the commands, by the owner's decision", so what is left to
 * test is narrow and all of it is load-bearing:
 *
 *   - the window is bounded and cannot be parked open
 *   - entry refuses on a boiler that is doing something
 *   - the stand-down blocks the automatic path WITHOUT blocking the safety path - the one
 *     place where getting it wrong would turn "bypass the control logic" into "bypass the
 *     overheat shutdown"
 *   - the exit mask puts back what it found, including the two relays (valve, alarm) that have
 *     no automatic writer to put them back for us
 *   - relay NAMES resolve through RelayIndex, so a re-map moves topic and hardware together
 */

#include <unity.h>
#include <cstdint>
#include <cstring>

#include "../../include/modules/diagnostics/TestModePolicy.h"
#include "../../include/modules/control/BusReleasePolicy.h"
#include "../../include/config/RelayIndices.h"

// setUp and tearDown are defined in test_main.cpp

using namespace TestModePolicy;

void test_test_mode_window_is_clamped_and_capped() {
    // 0 is "unspecified" and must take the DEFAULT, not the minimum: a bare "test_mode" is
    // the common case and a 30 s window would expire while the operator walked to the boiler.
    TEST_ASSERT_EQUAL_UINT32(DEFAULT_SECONDS, clampSeconds(0));
    TEST_ASSERT_EQUAL_UINT32(MIN_SECONDS, clampSeconds(1));
    TEST_ASSERT_EQUAL_UINT32(MIN_SECONDS, clampSeconds(MIN_SECONDS));
    TEST_ASSERT_EQUAL_UINT32(240, clampSeconds(240));
    TEST_ASSERT_EQUAL_UINT32(MAX_SECONDS, clampSeconds(MAX_SECONDS + 1));
    TEST_ASSERT_EQUAL_UINT32(MAX_SECONDS, clampSeconds(86400));
    TEST_ASSERT_EQUAL_UINT32(MAX_SECONDS, clampSeconds(0xFFFFFFFFu));
    // The cap is the whole reason this is a window. No caller may leave the relays manual.
    TEST_ASSERT_TRUE_MESSAGE(MAX_SECONDS <= 900, "the hard cap must stay short");
    TEST_ASSERT_TRUE_MESSAGE(DEFAULT_SECONDS <= MAX_SECONDS, "default must fit under the cap");
}

void test_test_mode_entry_is_clear_on_a_quiescent_boiler() {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Refusal::NONE),
                          static_cast<int>(mayEnter(0x00, BurnerSMState::IDLE, false, false)));
    // Pumps running do NOT block entry, unlike a bus release. There the pumps had to keep
    // turning because withholding the ticks would have killed them 10 s later; here they keep
    // being renewed, and dissipating heat is a perfectly good moment to go and listen to one.
    const uint8_t pumps = static_cast<uint8_t>((1u << RelayIndex::HEATING_PUMP) |
                                               (1u << RelayIndex::WATER_PUMP));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Refusal::NONE),
                          static_cast<int>(mayEnter(pumps, BurnerSMState::IDLE, false, false)));
}

void test_test_mode_entry_refuses_while_the_burner_fires() {
    const uint8_t burner = static_cast<uint8_t>(1u << RelayIndex::BURNER_ENABLE);
    const uint8_t water = static_cast<uint8_t>(1u << RelayIndex::WATER_MODE);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Refusal::BURNER_ACTIVE),
                          static_cast<int>(mayEnter(burner, BurnerSMState::IDLE, false, false)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Refusal::BURNER_ACTIVE),
                          static_cast<int>(mayEnter(water, BurnerSMState::IDLE, false, false)));
    // POWER_BOOST alone cannot fire anything, so it must NOT refuse - otherwise a boiler left
    // with the boost bit set could never be worked on.
    const uint8_t boostOnly = static_cast<uint8_t>(1u << RelayIndex::POWER_BOOST);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Refusal::NONE),
                          static_cast<int>(mayEnter(boostOnly, BurnerSMState::IDLE, false, false)));
}

void test_test_mode_entry_refuses_mid_sequence_but_not_when_parked() {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Refusal::BURNER_SEQUENCING),
                          static_cast<int>(mayEnter(0x00, BurnerSMState::PRE_PURGE, false, false)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Refusal::BURNER_SEQUENCING),
                          static_cast<int>(mayEnter(0x00, BurnerSMState::IGNITION, false, false)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Refusal::BURNER_SEQUENCING),
                          static_cast<int>(mayEnter(0x00, BurnerSMState::MODE_SWITCHING, false, false)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Refusal::BURNER_SEQUENCING),
                          static_cast<int>(mayEnter(0x00, BurnerSMState::POST_PURGE, false, false)));
    // LOCKOUT and ERROR are quiescent and are exactly when an operator wants the relays.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Refusal::NONE),
                          static_cast<int>(mayEnter(0x00, BurnerSMState::LOCKOUT, false, false)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Refusal::NONE),
                          static_cast<int>(mayEnter(0x00, BurnerSMState::ERROR, false, false)));
}

void test_test_mode_entry_refuses_during_ota_and_a_released_bus() {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Refusal::OTA_ACTIVE),
                          static_cast<int>(mayEnter(0x00, BurnerSMState::IDLE, true, false)));
    // A released bus means the relays are physically unreachable - RYN4 writes ride the same
    // RS485 bus as the sensors - so a window would accept commands that go nowhere.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Refusal::BUS_RELEASED),
                          static_cast<int>(mayEnter(0x00, BurnerSMState::IDLE, false, true)));
    // OTA outranks everything: it ends in a reboot, so nothing else about the state matters.
    const uint8_t burner = static_cast<uint8_t>(1u << RelayIndex::BURNER_ENABLE);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Refusal::OTA_ACTIVE),
                          static_cast<int>(mayEnter(burner, BurnerSMState::IGNITION, true, true)));
}

void test_test_mode_firing_mask_matches_the_bus_release_one() {
    // Two features now ask "is the burner firing?" from the same relay bits. If these ever
    // diverge one of them is wrong, and the divergence would be invisible in both.
    TEST_ASSERT_EQUAL_UINT8(BusReleasePolicy::FIRING_MASK, TestModePolicy::FIRING_MASK);
    TEST_ASSERT_EQUAL_UINT8(BusReleasePolicy::PUMP_MASK, TestModePolicy::PUMP_MASK);
    // And the mask is derived, not retyped: 0x05 is burner (bit 0) + water mode (bit 2).
    TEST_ASSERT_EQUAL_UINT8(0x05, TestModePolicy::FIRING_MASK);
    TEST_ASSERT_EQUAL_UINT8(0x30, TestModePolicy::PUMP_MASK);
}

void test_stand_down_never_blocks_an_off_command() {
    // THE MOST IMPORTANT TEST IN THIS FILE. CentralizedFailsafe stops the burner with ORDINARY
    // setRelayState() calls on burner enable, power boost and water mode - not the emergency
    // setter. A stand-down that blocked those would mean a manual window disabled the overheat
    // shutdown, which is the one thing "no interlocks" must not be allowed to mean.
    const uint8_t safetyRelays[] = { RelayIndex::BURNER_ENABLE, RelayIndex::POWER_BOOST,
                                     RelayIndex::WATER_MODE };
    for (uint8_t i = 0; i < 3; i++) {
        TEST_ASSERT_FALSE_MESSAGE(
            standDownBlocks(safetyRelays[i], /*turningOn=*/false, /*isEmergency=*/false),
            "an OFF from the safety path must always reach the hardware");
    }
    // Every relay, both emergency and not: an OFF is never blocked.
    for (uint8_t idx = 0; idx < RelayIndex::MAX_RELAYS; idx++) {
        TEST_ASSERT_FALSE(standDownBlocks(idx, false, false));
        TEST_ASSERT_FALSE(standDownBlocks(idx, false, true));
    }
}

void test_stand_down_blocks_automatic_on_commands() {
    // The control path must not energise anything behind the operator's back.
    TEST_ASSERT_TRUE(standDownBlocks(RelayIndex::BURNER_ENABLE, true, false));
    TEST_ASSERT_TRUE(standDownBlocks(RelayIndex::POWER_BOOST, true, false));
    TEST_ASSERT_TRUE(standDownBlocks(RelayIndex::WATER_MODE, true, false));
    TEST_ASSERT_TRUE(standDownBlocks(RelayIndex::HEATING_PUMP, true, false));
    TEST_ASSERT_TRUE(standDownBlocks(RelayIndex::WATER_PUMP, true, false));
    TEST_ASSERT_TRUE(standDownBlocks(RelayIndex::VALVE, true, false));
    TEST_ASSERT_TRUE(standDownBlocks(RelayIndex::SPARE_7, true, false));
}

void test_stand_down_exempts_the_alarm_and_the_emergency_path() {
    // ALARM: BurnerStateMachine raises it on lockout ENTRY, an edge. Swallowing that would
    // silence a real alarm for the length of the window and it would never be re-raised.
    TEST_ASSERT_FALSE(standDownBlocks(RelayIndex::ALARM, true, false));
    // The emergency setter is how CentralizedFailsafe keeps the pumps turning during heat
    // dissipation. It bypasses the stand-down in both directions, for every relay.
    for (uint8_t idx = 0; idx < RelayIndex::MAX_RELAYS; idx++) {
        TEST_ASSERT_FALSE(standDownBlocks(idx, true, /*isEmergency=*/true));
    }
}

void test_exit_mask_restores_the_relays_with_no_automatic_writer() {
    // The reason exitMask() is not simply 0. NOTHING in this firmware ever commands VALVE
    // (SystemEventsGenerated declares VALVE_OPEN/CLOSE and no handler exists), and ALARM is
    // written only on lockout edges. Zeroing them would leave a diverter or an alarm in a
    // state the control logic will never correct.
    const uint8_t entry = static_cast<uint8_t>((1u << RelayIndex::VALVE) |
                                               (1u << RelayIndex::ALARM));
    TEST_ASSERT_EQUAL_UINT8(entry, exitMask(entry, /*live=*/0x00));

    // And a bit the operator set but that was NOT set at entry is dropped.
    const uint8_t operatorSet = static_cast<uint8_t>(1u << RelayIndex::SPARE_7);
    TEST_ASSERT_EQUAL_UINT8(entry, exitMask(entry, static_cast<uint8_t>(entry | operatorSet)));
}

void test_exit_mask_always_clears_the_firing_bits() {
    // mayEnter() checks `actual`, and `desired` can legitimately lead it, so the entry
    // snapshot is NOT guaranteed to have the firing bits clear. Restoring one would re-light
    // the boiler on a decision taken before the operator started poking at the sensors.
    const uint8_t entry = static_cast<uint8_t>(0xFF);
    const uint8_t out = exitMask(entry, 0x00);
    TEST_ASSERT_EQUAL_UINT8(0, out & FIRING_MASK);
    TEST_ASSERT_EQUAL_UINT8(0, out & (1u << RelayIndex::BURNER_ENABLE));
    TEST_ASSERT_EQUAL_UINT8(0, out & (1u << RelayIndex::WATER_MODE));
}

void test_exit_mask_takes_the_pump_bits_live_not_from_the_snapshot() {
    // PumpControlModule reconciles the pumps within PUMP_REQUEST_RESEND_MS whatever we leave
    // behind, so writing them would only cause a flap. Leave them exactly as they are.
    const uint8_t pumpsOn = PUMP_MASK;
    TEST_ASSERT_EQUAL_UINT8(pumpsOn, exitMask(/*entry=*/0x00, /*live=*/pumpsOn) & PUMP_MASK);
    TEST_ASSERT_EQUAL_UINT8(0, exitMask(/*entry=*/pumpsOn, /*live=*/0x00) & PUMP_MASK);
    // Both halves at once: entry had the valve, the operator left the heating pump running.
    const uint8_t entry = static_cast<uint8_t>(1u << RelayIndex::VALVE);
    const uint8_t live = static_cast<uint8_t>(1u << RelayIndex::HEATING_PUMP);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(entry | live), exitMask(entry, live));
}

void test_exit_mask_is_idempotent() {
    // Applying it twice must not drift: expireIfDue() and leave() can both be reached, and a
    // second call after the first has already stored the result must change nothing.
    const uint8_t entry = static_cast<uint8_t>(0xFF);
    const uint8_t once = exitMask(entry, 0x00);
    TEST_ASSERT_EQUAL_UINT8(once, exitMask(entry, once));
}

void test_relay_names_resolve_through_the_relay_index() {
    // NAMES, NOT NUMBERS. The 2025 override addressed relays/<1-8>/control by physical number,
    // so a re-map would have silently pointed a working topic at different hardware.
    TEST_ASSERT_EQUAL_UINT8(RelayIndex::BURNER_ENABLE, relayFromName("burner"));
    TEST_ASSERT_EQUAL_UINT8(RelayIndex::BURNER_ENABLE, relayFromName("burner_enable"));
    TEST_ASSERT_EQUAL_UINT8(RelayIndex::POWER_BOOST, relayFromName("power_boost"));
    TEST_ASSERT_EQUAL_UINT8(RelayIndex::WATER_MODE, relayFromName("water_mode"));
    TEST_ASSERT_EQUAL_UINT8(RelayIndex::VALVE, relayFromName("valve"));
    TEST_ASSERT_EQUAL_UINT8(RelayIndex::HEATING_PUMP, relayFromName("heating_pump"));
    TEST_ASSERT_EQUAL_UINT8(RelayIndex::WATER_PUMP, relayFromName("water_pump"));
    TEST_ASSERT_EQUAL_UINT8(RelayIndex::SPARE_7, relayFromName("spare"));
    TEST_ASSERT_EQUAL_UINT8(RelayIndex::ALARM, relayFromName("alarm"));
    // The add-on panel's old name for the boost relay. Accepted so an existing card keeps
    // working - the name is a misnomer (the bit is relayPowerBoost, so ON means FULL power),
    // which is why power_boost is canonical and this is only tolerated.
    TEST_ASSERT_EQUAL_UINT8(RelayIndex::POWER_BOOST, relayFromName("half_power"));
}

void test_relay_names_reject_everything_else() {
    TEST_ASSERT_EQUAL_UINT8(0xFF, relayFromName(""));
    TEST_ASSERT_EQUAL_UINT8(0xFF, relayFromName(nullptr));
    TEST_ASSERT_EQUAL_UINT8(0xFF, relayFromName("pump"));
    TEST_ASSERT_EQUAL_UINT8(0xFF, relayFromName("Burner"));   // case matters, no silent match
    TEST_ASSERT_EQUAL_UINT8(0xFF, relayFromName("burner "));  // no trimming
    // Not a number interface. "3" must not reach physical relay 3.
    TEST_ASSERT_EQUAL_UINT8(0xFF, relayFromName("3"));
    // And a name that collides with a cmd/ verb resolves to nothing, so the router's
    // "/relay/ first" ordering cannot be defeated by a relay named after a command.
    TEST_ASSERT_EQUAL_UINT8(0xFF, relayFromName("system"));
    TEST_ASSERT_EQUAL_UINT8(0xFF, relayFromName("status"));
}

void test_relay_allowed_is_one_tier_covering_all_eight() {
    // The owner's decision: one tier, not a safe/hazardous split.
    for (uint8_t idx = 0; idx < RelayIndex::MAX_RELAYS; idx++) {
        TEST_ASSERT_TRUE(relayAllowed(idx));
    }
    TEST_ASSERT_FALSE(relayAllowed(RelayIndex::MAX_RELAYS));
    TEST_ASSERT_FALSE(relayAllowed(0xFF));  // the relayFromName() miss value
}

void test_test_mode_expiry_survives_the_millis_rollover() {
    // millis() wraps every 49.7 days and this controller has run longer than that. A plain
    // `now >= deadline` would either end a window instantly or extend it by 49 days.
    const uint32_t nearWrap = 0xFFFFFF00u;
    const uint32_t deadline = nearWrap + 1000u;  // wrapped past zero
    TEST_ASSERT_FALSE(expired(nearWrap, deadline));
    TEST_ASSERT_FALSE(expired(0x00000000u, deadline));
    TEST_ASSERT_TRUE(expired(deadline, deadline));
    TEST_ASSERT_TRUE(expired(deadline + 1u, deadline));
    // secondsLeft must stay sane across the same wrap rather than reporting ~49 days.
    TEST_ASSERT_EQUAL_UINT32(1, secondsLeft(nearWrap, deadline));
    TEST_ASSERT_EQUAL_UINT32(0, secondsLeft(deadline, deadline));
}

void test_test_mode_extending_never_shortens() {
    // A second "test_mode 30" must not cut a 15-minute window out from under the operator -
    // the bug ModbusBusRelease::release() actually had, with a comment claiming otherwise.
    const uint32_t longDeadline = 1000u + 900u * 1000u;
    const uint32_t shortProposed = 1000u + 30u * 1000u;
    TEST_ASSERT_FALSE(isLater(shortProposed, longDeadline));
    TEST_ASSERT_TRUE(isLater(longDeadline, shortProposed));
    // Rollover-safe too: a deadline just past the wrap is LATER than one just before it.
    TEST_ASSERT_TRUE(isLater(0x00000100u, 0xFFFFFF00u));
    TEST_ASSERT_FALSE(isLater(0xFFFFFF00u, 0x00000100u));
}

void test_test_mode_seconds_left_rounds_up() {
    // Reporting 0 while still active would contradict the `active` flag published beside it.
    TEST_ASSERT_EQUAL_UINT32(1, secondsLeft(0, 1));
    TEST_ASSERT_EQUAL_UINT32(1, secondsLeft(0, 200));
    TEST_ASSERT_EQUAL_UINT32(1, secondsLeft(0, 1000));
    TEST_ASSERT_EQUAL_UINT32(2, secondsLeft(0, 1001));
    TEST_ASSERT_EQUAL_UINT32(0, secondsLeft(5000, 5000));
}

void test_test_mode_refusal_labels_are_stable() {
    // Published on boiler/status/testmode and read by the add-on panel. Renaming one is an
    // interface change, not a tidy-up.
    TEST_ASSERT_EQUAL_STRING("ok", toString(Refusal::NONE));
    TEST_ASSERT_EQUAL_STRING("burner_active", toString(Refusal::BURNER_ACTIVE));
    TEST_ASSERT_EQUAL_STRING("burner_sequencing", toString(Refusal::BURNER_SEQUENCING));
    TEST_ASSERT_EQUAL_STRING("ota_active", toString(Refusal::OTA_ACTIVE));
    TEST_ASSERT_EQUAL_STRING("bus_released", toString(Refusal::BUS_RELEASED));
    // Shared vocabulary with the bus release, where the condition is the same condition.
    TEST_ASSERT_EQUAL_STRING(BusReleasePolicy::toString(BusReleasePolicy::Refusal::BURNER_ACTIVE),
                             toString(Refusal::BURNER_ACTIVE));
}
