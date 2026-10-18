/**
 * @file test_burner_transition_policy.cpp
 * @brief Unit tests for BurnerTransitionPolicy (explicit disable, bounded mode switch)
 */

#include <unity.h>
#include <cstdint>

#include "../../include/modules/control/BurnerTransitionPolicy.h"

// setUp and tearDown are defined in test_main.cpp

using namespace BurnerTransitionPolicy;

void test_policy_explicit_disable_stops_running_mode_only() {
    // Heating running: heating disabled -> stop; water disabled -> keep running
    TEST_ASSERT_TRUE(stopForExplicitDisable(false, true, false, true));
    TEST_ASSERT_FALSE(stopForExplicitDisable(false, true, true, false));
    // Water running: water disabled -> stop; heating disabled -> keep running
    TEST_ASSERT_TRUE(stopForExplicitDisable(true, true, true, false));
    TEST_ASSERT_FALSE(stopForExplicitDisable(true, true, false, true));
    // Both enabled -> no stop
    TEST_ASSERT_FALSE(stopForExplicitDisable(false, true, true, true));
    TEST_ASSERT_FALSE(stopForExplicitDisable(true, true, true, true));
}

void test_policy_boiler_disable_stops_any_mode() {
    TEST_ASSERT_TRUE(stopForExplicitDisable(false, false, true, true));
    TEST_ASSERT_TRUE(stopForExplicitDisable(true, false, true, true));
}

void test_policy_heating_wanted_respects_enable_and_override() {
    // Room mode, room 17.0 < target 18.0, but heating disabled or overridden off
    TEST_ASSERT_FALSE(heatingLikelyWanted(false, false, false, true, 100, 150, true, 170, 180, 10));
    TEST_ASSERT_FALSE(heatingLikelyWanted(true, true, false, true, 100, 150, true, 170, 180, 10));
    TEST_ASSERT_TRUE(heatingLikelyWanted(true, false, false, true, 100, 150, true, 170, 180, 10));
}

void test_policy_heating_wanted_room_mode() {
    TEST_ASSERT_TRUE(heatingLikelyWanted(true, false, false, false, 0, 0, true, 179, 180, 10));
    TEST_ASSERT_FALSE(heatingLikelyWanted(true, false, false, false, 0, 0, true, 180, 180, 10));
    TEST_ASSERT_FALSE(heatingLikelyWanted(true, false, false, false, 0, 0, false, 100, 180, 10));  // room invalid
}

void test_policy_heating_wanted_weather_mode() {
    // Outside 10.0 < threshold 15.0, room 22.6 > 18.0 + 1.0 -> overheated, not wanted
    TEST_ASSERT_FALSE(heatingLikelyWanted(true, false, true, true, 100, 150, true, 226, 180, 10, 5));
    // Outside 10.0 < 15.0, room 18.4 < restart limit 18.0 + 1.0 - 0.5 -> wanted
    // (room below target not required)
    TEST_ASSERT_TRUE(heatingLikelyWanted(true, false, true, true, 100, 150, true, 184, 180, 10, 5));
    // Room 18.5 was counted as wanted (only > 19.0 was overheated), but HeatingControlTask
    // does not restart at >= 18.5 (review 2026-09-14)
    TEST_ASSERT_FALSE(heatingLikelyWanted(true, false, true, true, 100, 150, true, 185, 180, 10, 5));
    // Outside 20.0 >= threshold 15.0 -> not wanted even with a cold room
    // (the old wait only checked room < target and would have waited forever)
    TEST_ASSERT_FALSE(heatingLikelyWanted(true, false, true, true, 200, 150, true, 150, 180, 10));
    // Outside sensor invalid -> not wanted
    TEST_ASSERT_FALSE(heatingLikelyWanted(true, false, true, false, 100, 150, true, 150, 180, 10));
}

void test_policy_heating_wanted_weather_mode_restart_limit_boundaries() {
    // HeatingControlTask not heating: roomOverheated = room >= target + margin - hysteresis
    // Target 18.0, margin 1.0, hysteresis 0.5 -> restart limit 18.5
    TEST_ASSERT_TRUE(heatingLikelyWanted(true, false, true, true, 100, 150, true, 184, 180, 10, 5));
    TEST_ASSERT_FALSE(heatingLikelyWanted(true, false, true, true, 100, 150, true, 185, 180, 10, 5));
    TEST_ASSERT_FALSE(heatingLikelyWanted(true, false, true, true, 100, 150, true, 186, 180, 10, 5));
    TEST_ASSERT_FALSE(heatingLikelyWanted(true, false, true, true, 100, 150, true, 190, 180, 10, 5));
    // Default hysteresis is the settings default 0.5 °C
    TEST_ASSERT_TRUE(heatingLikelyWanted(true, false, true, true, 100, 150, true, 184, 180, 10));
    TEST_ASSERT_FALSE(heatingLikelyWanted(true, false, true, true, 100, 150, true, 185, 180, 10));
    // Hysteresis 1.0 -> restart limit 18.0
    TEST_ASSERT_TRUE(heatingLikelyWanted(true, false, true, true, 100, 150, true, 179, 180, 10, 10));
    TEST_ASSERT_FALSE(heatingLikelyWanted(true, false, true, true, 100, 150, true, 180, 180, 10, 10));
    // Hysteresis 0 -> restart limit 19.0 is already overheated (>=)
    TEST_ASSERT_TRUE(heatingLikelyWanted(true, false, true, true, 100, 150, true, 189, 180, 10, 0));
    TEST_ASSERT_FALSE(heatingLikelyWanted(true, false, true, true, 100, 150, true, 190, 180, 10, 0));
    // Room invalid or no room target: no overheat protection, outside decides
    TEST_ASSERT_TRUE(heatingLikelyWanted(true, false, true, true, 100, 150, false, 250, 180, 10, 5));
    TEST_ASSERT_TRUE(heatingLikelyWanted(true, false, true, true, 100, 150, true, 250, 0, 10, 5));
    // Outside start rule has no hysteresis: outside < threshold
    TEST_ASSERT_TRUE(heatingLikelyWanted(true, false, true, true, 149, 150, true, 170, 180, 10, 5));
    TEST_ASSERT_FALSE(heatingLikelyWanted(true, false, true, true, 150, 150, true, 170, 180, 10, 5));
    TEST_ASSERT_FALSE(heatingLikelyWanted(true, false, true, true, 151, 150, true, 170, 180, 10, 5));
}

void test_policy_heating_wanted_room_mode_start_ignores_hysteresis() {
    // HeatingControlTask not heating in room mode: start when room < target; the
    // hysteresis only raises the stop limit (target + hysteresis)
    TEST_ASSERT_TRUE(heatingLikelyWanted(true, false, false, false, 0, 0, true, 179, 180, 10, 5));
    TEST_ASSERT_FALSE(heatingLikelyWanted(true, false, false, false, 0, 0, true, 180, 180, 10, 5));
    TEST_ASSERT_FALSE(heatingLikelyWanted(true, false, false, false, 0, 0, true, 181, 180, 10, 5));
    TEST_ASSERT_TRUE(heatingLikelyWanted(true, false, false, false, 0, 0, true, 179, 180, 10, 0));
    TEST_ASSERT_FALSE(heatingLikelyWanted(true, false, false, false, 0, 0, true, 180, 180, 10, 10));
    // No room target -> not wanted
    TEST_ASSERT_FALSE(heatingLikelyWanted(true, false, false, false, 0, 0, true, 100, 0, 10, 5));
}

void test_policy_mode_switch_wait_is_bounded() {
    // Water -> heating handover with heating wanted: wait, but only up to the limit
    TEST_ASSERT_TRUE(onNoDemandForNewMode(false, true, 0) == ModeSwitchAction::WAIT);
    TEST_ASSERT_TRUE(onNoDemandForNewMode(false, true, MODE_SWITCH_MAX_WAIT_MS - 1) == ModeSwitchAction::WAIT);
    TEST_ASSERT_TRUE(onNoDemandForNewMode(false, true, MODE_SWITCH_MAX_WAIT_MS) == ModeSwitchAction::STOP);
    // Heating not wanted (disabled / warm outside): stop immediately
    TEST_ASSERT_TRUE(onNoDemandForNewMode(false, false, 0) == ModeSwitchAction::STOP);
    // New mode water without a water request: stop immediately
    TEST_ASSERT_TRUE(onNoDemandForNewMode(true, true, 0) == ModeSwitchAction::STOP);
}

void test_policy_mode_revert_requires_on_bit() {
    TEST_ASSERT_TRUE(onModeReverted(true, 0) == RevertAction::RESUME_RUNNING);
    // ON bit withdrawn (override off / handover race): no bounce back to RUNNING
    TEST_ASSERT_TRUE(onModeReverted(false, 0) == RevertAction::WAIT);
    TEST_ASSERT_TRUE(onModeReverted(false, MODE_SWITCH_MAX_WAIT_MS) == RevertAction::STOP);
}

void test_policy_mode_switch_exit_records_power_level() {
    // Review 2026-09-14: MODE_SWITCHING -> POST_PURGE did not record OFF, so the restart
    // from POST_PURGE skipped the minimum off-time
    TEST_ASSERT_TRUE(recordsPowerLevelOnTransition(BurnerSMState::POST_PURGE));
    TEST_ASSERT_TRUE(recordsPowerLevelOnTransition(BurnerSMState::ERROR));
    TEST_ASSERT_TRUE(recordsPowerLevelOnTransition(BurnerSMState::RUNNING_LOW));
    TEST_ASSERT_TRUE(recordsPowerLevelOnTransition(BurnerSMState::RUNNING_HIGH));
    TEST_ASSERT_TRUE(recordsPowerLevelOnTransition(BurnerSMState::IDLE));
    // Level kept while switching; IGNITION records its start level on entry
    TEST_ASSERT_FALSE(recordsPowerLevelOnTransition(BurnerSMState::MODE_SWITCHING));
    TEST_ASSERT_FALSE(recordsPowerLevelOnTransition(BurnerSMState::IGNITION));
}
