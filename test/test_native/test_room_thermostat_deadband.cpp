/**
 * @file test_room_thermostat_deadband.cpp
 * @brief The asymmetric room-thermostat dead band - the region three copies of the rule
 *        disagreed about, and that no test exercised.
 *
 * S2, 2026-11-08. The tree held THREE copies of "does the room want heat?":
 *
 *                                                  turn ON below   turn OFF at/above
 *   SpaceHeatingPolicy::roomThermostatWantsHeat    setpoint        setpoint + hysteresis
 *   HeatingControlModule::checkHeatingConditions   target - hyst   target + hysteresis
 *   MockHeatingControl::checkHeatingConditions     target - hyst   target
 *
 * Only the first ran. The second was dead and has been deleted; the third was what the
 * "integration" tests actually exercised, so they asserted the mock's behaviour rather than
 * the controller's and would have passed whatever the firmware did.
 *
 * They passed for a more specific reason, which is the lesson: every test input was 18.0
 * against a 21.0 setpoint - nowhere near the dead band. The three rules only differ BETWEEN
 * setpoint - hysteresis and setpoint + hysteresis, so no test ever reached the disputed
 * region. Agreement outside it proved nothing.
 *
 * These tests live in that region on purpose.
 */

#include <unity.h>
#include <cstdint>

#include "../../include/modules/control/SpaceHeatingPolicy.h"

// setUp and tearDown are defined in test_main.cpp

using SpaceHeatingPolicy::roomThermostatWantsHeat;

// The live configuration: target 19.2 with the deployed hysteresis, and a round 21.0/0.5
// pair for the arithmetic, which is what the old tests used.
static constexpr int16_t SP = 210;   // 21.0 °C setpoint
static constexpr int16_t HY = 5;     // 0.5 °C hysteresis

void test_deadband_turns_on_strictly_below_the_setpoint() {
    // OFF -> ON happens at the setpoint itself, NOT at setpoint - hysteresis. Both deleted
    // copies used the lower threshold, so each would have left the room half a degree colder
    // before lighting.
    TEST_ASSERT_TRUE(roomThermostatWantsHeat(false, 209, SP, HY));   // 20.9 < 21.0 -> heat
    TEST_ASSERT_FALSE(roomThermostatWantsHeat(false, 210, SP, HY));  // exactly at setpoint
    TEST_ASSERT_FALSE(roomThermostatWantsHeat(false, 211, SP, HY));
}

void test_deadband_stays_on_until_setpoint_plus_hysteresis() {
    // ON -> OFF happens at setpoint + hysteresis. The mock turned off at `setpoint`, which
    // is the exact disagreement these tests exist to pin.
    TEST_ASSERT_TRUE(roomThermostatWantsHeat(true, 210, SP, HY));   // at setpoint, STILL on
    TEST_ASSERT_TRUE(roomThermostatWantsHeat(true, 214, SP, HY));   // 21.4 < 21.5, still on
    TEST_ASSERT_FALSE(roomThermostatWantsHeat(true, 215, SP, HY));  // 21.5 -> off
    TEST_ASSERT_FALSE(roomThermostatWantsHeat(true, 220, SP, HY));
}

void test_deadband_is_asymmetric_which_is_the_point() {
    // The same room temperature gives OPPOSITE answers depending on the current state. Any
    // copy of this rule that does not take `currentlyHeating` cannot be correct - and the
    // mock did not take it.
    const int16_t room = 212;  // 21.2: above the setpoint, below setpoint + hysteresis
    TEST_ASSERT_FALSE(roomThermostatWantsHeat(false, room, SP, HY));  // was off: stay off
    TEST_ASSERT_TRUE(roomThermostatWantsHeat(true, room, SP, HY));    // was on: stay on
}

void test_deadband_the_three_points_the_old_mock_got_wrong() {
    // Reproduced exactly from the divergence check that found this. At these inputs the old
    // mock answered "no heat" where the firmware answers "heat". A test at any one of them
    // would have caught the split years earlier.
    TEST_ASSERT_TRUE(roomThermostatWantsHeat(false, 205, SP, HY));  // old mock: false
    TEST_ASSERT_TRUE(roomThermostatWantsHeat(true, 205, SP, HY));   // old mock: false
    TEST_ASSERT_TRUE(roomThermostatWantsHeat(true, 210, SP, HY));   // old mock: false
}

void test_deadband_zero_hysteresis_collapses_to_a_plain_threshold() {
    // Degenerate but reachable: with no hysteresis the two branches must coincide, otherwise
    // the rule would chatter at exactly one point.
    TEST_ASSERT_TRUE(roomThermostatWantsHeat(false, 209, SP, 0));
    TEST_ASSERT_FALSE(roomThermostatWantsHeat(false, 210, SP, 0));
    TEST_ASSERT_FALSE(roomThermostatWantsHeat(true, 210, SP, 0));
}

void test_deadband_at_the_deployed_room_target() {
    // 19.2 is the live targetTemp. Sanity that the band sits where an operator would expect:
    // heat below 19.2, stop at 19.7, and 19.5 depends on which way you came.
    const int16_t sp = 192;
    TEST_ASSERT_TRUE(roomThermostatWantsHeat(false, 191, sp, HY));
    TEST_ASSERT_FALSE(roomThermostatWantsHeat(false, 195, sp, HY));
    TEST_ASSERT_TRUE(roomThermostatWantsHeat(true, 195, sp, HY));
    TEST_ASSERT_FALSE(roomThermostatWantsHeat(true, 197, sp, HY));
}
