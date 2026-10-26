/**
 * @file test_frost_protection_policy.cpp
 * @brief Unit tests for FrostProtectionPolicy (2026-10-23)
 */

#include <unity.h>
#include <cstdint>

#include "../../include/modules/control/FrostProtectionPolicy.h"

// setUp and tearDown are defined in test_main.cpp

using FrostProtectionPolicy::Readings;
using FrostProtectionPolicy::demand;
using FrostProtectionPolicy::coldMask;
using FrostProtectionPolicy::describe;
using FrostProtectionPolicy::COLD_FLOW;
using FrostProtectionPolicy::COLD_TANK;
using FrostProtectionPolicy::COLD_ROOM;
using FrostProtectionPolicy::TRIGGER_TENTHS;
using FrostProtectionPolicy::RELEASE_TENTHS;

static Readings normalHouse() {
    Readings r;
    r.flow = 350; r.flowUsable = true;   // 35.0, a typical idle flow
    r.tank = 453; r.tankUsable = true;   // 45.3, the charge floor
    r.room = 198; r.roomUsable = true;   // 19.8, the measured room
    return r;
}

void test_frost_is_inert_in_a_working_system() {
    // The guard must be incapable of firing in normal operation - that is what makes it
    // safe to add to the heating path at all. These are the real readings from the evening
    // this was written.
    const Readings r = normalHouse();
    TEST_ASSERT_FALSE(demand(false, r));
    TEST_ASSERT_FALSE(demand(true, r));   // even latched, it releases immediately
    TEST_ASSERT_EQUAL_UINT8(0, coldMask(r));
}

void test_frost_triggers_on_either_reading() {
    Readings r = normalHouse();
    r.flow = TRIGGER_TENTHS;              // exactly 5.0 - inclusive
    TEST_ASSERT_TRUE(demand(false, r));
    TEST_ASSERT_EQUAL_UINT8(COLD_FLOW, coldMask(r));

    // ANY of the three triggers the one latch. The tank briefly drove a separate DHW-charge
    // latch (2026-10-25); reverted 2026-10-26 - it now warms the room the tank stands in,
    // which is the mechanism by which the tank is actually protected.
    r = normalHouse();
    r.tank = TRIGGER_TENTHS;
    TEST_ASSERT_TRUE(demand(false, r));
    TEST_ASSERT_EQUAL_UINT8(COLD_TANK, coldMask(r));

    // The ROOM is the reason this guard exists at all.
    r = normalHouse();
    r.room = TRIGGER_TENTHS;
    TEST_ASSERT_TRUE(demand(false, r));
    TEST_ASSERT_EQUAL_UINT8(COLD_ROOM, coldMask(r));

    r = normalHouse();
    r.flow = 40; r.tank = 20;
    TEST_ASSERT_EQUAL_UINT8(COLD_FLOW | COLD_TANK, coldMask(r));

    // One tenth above the trigger is not frost
    r = normalHouse();
    r.flow = TRIGGER_TENTHS + 1;
    TEST_ASSERT_FALSE(demand(false, r));
}

void test_frost_holds_through_the_hysteresis_band() {
    // Triggered at 5.0, it must keep firing all the way to 10.0 rather than dropping out the
    // moment the water clears the trigger - otherwise it chatters the burner at ~5 °C.
    Readings r = normalHouse();
    r.flow = 30;
    TEST_ASSERT_TRUE(demand(false, r));          // trigger at 3.0

    r.flow = 80;                                  // 8.0 - above trigger, below release
    TEST_ASSERT_TRUE(demand(true, r));            // still demanding
    TEST_ASSERT_FALSE(demand(false, r));          // but would not have started here

    r.flow = RELEASE_TENTHS;                      // exactly 10.0 - inclusive release
    TEST_ASSERT_FALSE(demand(true, r));
}

void test_frost_never_triggers_on_an_invalid_reading() {
    // The whole reason the trigger needs the validity flag: TEMP_INVALID is INT16_MIN, which
    // reads as -3276.8 °C and would fire the burner instantly and permanently.
    Readings r;
    r.flow = TEMP_INVALID; r.flowUsable = false;
    r.tank = TEMP_INVALID; r.tankUsable = false;
    TEST_ASSERT_FALSE(demand(false, r));
    TEST_ASSERT_EQUAL_UINT8(0, coldMask(r));

    // A cold value that is marked invalid is still not evidence of frost
    r.flow = 0; r.flowUsable = false;
    TEST_ASSERT_FALSE(demand(false, r));

    // ... and the same value marked valid IS
    r.flowUsable = true;
    TEST_ASSERT_TRUE(demand(false, r));
}

void test_frost_releases_rather_than_firing_blind() {
    // If every reading goes invalid DURING a frost burn, stop. Continuing to fire with no
    // view of what is being heated is the worse failure, and SensorFailureConfirm plus the
    // sensor fallback are better placed to react than this guard.
    Readings r = normalHouse();
    r.flow = 20;
    TEST_ASSERT_TRUE(demand(false, r));           // burning

    r.flowUsable = false; r.roomUsable = false;    // both heat-latch sensors drop out
    TEST_ASSERT_FALSE(demand(true, r));           // release, do not fire blind

    // One surviving cold sensor still holds the demand, though
    r = normalHouse();
    r.flow = 20; r.roomUsable = false;
    TEST_ASSERT_TRUE(demand(true, r));
}

void test_frost_one_cold_sensor_holds_while_the_other_is_warm() {
    // The tank sits at 45 year-round, so in the realistic case only the flow circuit is
    // cold. A warm tank must not release a demand the flow still justifies.
    Readings r = normalHouse();
    r.flow = 25;                                   // 2.5 °C in the heating circuit
    TEST_ASSERT_TRUE(demand(false, r));
    TEST_ASSERT_TRUE(demand(true, r));             // room 19.8 does not release it
    r.flow = RELEASE_TENTHS + 5;
    TEST_ASSERT_FALSE(demand(true, r));            // both clear now
}

void test_frost_thresholds_are_the_agreed_numbers() {
    // Pinned so a change is deliberate: 5.0 matches the previously-dead
    // FREEZE_PROTECTION_TEMP_C constant, 10.0 gives 5 K of hysteresis.
    TEST_ASSERT_EQUAL_INT16(50, TRIGGER_TENTHS);
    TEST_ASSERT_EQUAL_INT16(100, RELEASE_TENTHS);
}

// --------------------------------------------------------------------------------------
// Staleness and the heat/water split (2026-10-25, user decision)
// --------------------------------------------------------------------------------------

void test_frost_a_stale_reading_never_fires_the_burner() {
    // THE USER'S RULE: "we should not fire the burner if the sensor data is stale."
    //
    // The policy cannot see timestamps - the caller folds freshness into the *Usable flags
    // (FrostProtection::evaluate). What this pins is that an unusable reading is treated
    // exactly like an invalid one, however cold it is, so a stale value can never trigger.
    //
    // This is the defect that made it necessary: nothing clears isBoilerTempOutputValid when
    // MB8ART reads STOP, so a dead bus froze the flow reading at its last value and left it
    // flagged valid forever. That cut both ways - a frozen WARM value meant frost protection
    // could never fire, and a frozen COLD one would have fired forever.
    Readings r = normalHouse();
    r.flow = 0;  r.flowUsable = false;   // 0.0 C but stale
    r.room = 10; r.roomUsable = false;   // 1.0 C but stale
    r.tank = 0;  r.tankUsable = false;
    TEST_ASSERT_FALSE(demand(false, r));
    TEST_ASSERT_EQUAL_UINT8(0, coldMask(r));

    // And the same readings, fresh, fire immediately - so the flag really is what decides,
    // not the value.
    r.flowUsable = true; r.roomUsable = true; r.tankUsable = true;
    TEST_ASSERT_TRUE(demand(false, r));
}

void test_frost_is_one_latch_over_all_three_sensors() {
    // There is ONE latch and ONE response: space heating at the flow floor. It was briefly two,
    // with a cold tank driving an autonomous DHW charge - reverted 2026-10-26 because the tank
    // trigger was unreachable first (the flow sensor shares its cellar with a fraction of the
    // mass, so it trips days earlier) while costing the most machinery and producing two of the
    // review's six high-severity findings.
    Readings r = normalHouse();
    r.tank = 20;
    TEST_ASSERT_TRUE(demand(false, r));
    r = normalHouse(); r.room = 20;
    TEST_ASSERT_TRUE(demand(false, r));
    r = normalHouse(); r.flow = 20;
    TEST_ASSERT_TRUE(demand(false, r));

    // Release needs EVERY watched reading clear, so one cold sensor holds the latch however
    // warm the others are - which is what stops a 5 -> 10 -> 5 chase between sensors.
    r = normalHouse();
    r.room = 20; r.tank = 20;
    TEST_ASSERT_TRUE(demand(false, r));
    r.tank = RELEASE_TENTHS + 50;                 // tank recovered
    TEST_ASSERT_TRUE(demand(true, r));            // room still cold
    r.room = RELEASE_TENTHS + 50;                 // room recovered too
    TEST_ASSERT_FALSE(demand(true, r));

    // And the flow alone can hold it after both others clear.
    r = normalHouse(); r.flow = 20;
    TEST_ASSERT_TRUE(demand(false, r));
    TEST_ASSERT_TRUE(demand(true, r));
    r.flow = RELEASE_TENTHS;
    TEST_ASSERT_FALSE(demand(true, r));
}

void test_frost_room_trigger_cannot_fire_in_a_lived_in_house() {
    // The room trigger is new, and it is the one with a sensor that reads plausible values all
    // day - so prove it cannot fire at any temperature a house is actually kept at. 5.0 C is
    // nearly 15 K below the lowest target this installation uses.
    Readings r = normalHouse();
    for (int16_t room = 150; room <= 250; room += 1) {   // 15.0 to 25.0 C
        r.room = room;
        TEST_ASSERT_FALSE_MESSAGE(demand(false, r), "frost must be inert at living temps");
    }
    // Even a cold empty house in winter is well clear of it.
    r.room = 100;   // 10.0 C
    TEST_ASSERT_FALSE(demand(false, r));
    r.room = 51;    // 5.1 C - one tenth above
    TEST_ASSERT_FALSE(demand(false, r));
    r.room = 50;    // 5.0 C - inclusive
    TEST_ASSERT_TRUE(demand(false, r));
}

void test_frost_cold_mask_describes_every_combination() {
    // The mask replaced a 4-value enum because three sensors need seven combinations; the log
    // line is the only record of WHY a frost burn started, so no combination may fall through.
    Readings r = normalHouse();
    TEST_ASSERT_EQUAL_STRING("none", describe(coldMask(r)));
    r.flow = 20;
    TEST_ASSERT_EQUAL_STRING("flow", describe(coldMask(r)));
    r.tank = 20;
    TEST_ASSERT_EQUAL_STRING("flow+tank", describe(coldMask(r)));
    r.room = 20;
    TEST_ASSERT_EQUAL_STRING("flow+tank+room", describe(coldMask(r)));
    r = normalHouse(); r.tank = 20;
    TEST_ASSERT_EQUAL_STRING("tank", describe(coldMask(r)));
    r = normalHouse(); r.room = 20;
    TEST_ASSERT_EQUAL_STRING("room", describe(coldMask(r)));
    r = normalHouse(); r.tank = 20; r.room = 20;
    TEST_ASSERT_EQUAL_STRING("tank+room", describe(coldMask(r)));
    r = normalHouse(); r.flow = 20; r.room = 20;
    TEST_ASSERT_EQUAL_STRING("flow+room", describe(coldMask(r)));
}
