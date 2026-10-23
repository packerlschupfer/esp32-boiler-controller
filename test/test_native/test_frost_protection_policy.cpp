/**
 * @file test_frost_protection_policy.cpp
 * @brief Unit tests for FrostProtectionPolicy (2026-10-23)
 */

#include <unity.h>
#include <cstdint>

#include "../../include/modules/control/FrostProtectionPolicy.h"

// setUp and tearDown are defined in test_main.cpp

using FrostProtectionPolicy::Readings;
using FrostProtectionPolicy::Source;
using FrostProtectionPolicy::demand;
using FrostProtectionPolicy::coldSource;
using FrostProtectionPolicy::TRIGGER_TENTHS;
using FrostProtectionPolicy::RELEASE_TENTHS;

static Readings normalHouse() {
    Readings r;
    r.flow = 350; r.flowValid = true;   // 35.0, a typical idle flow
    r.tank = 453; r.tankValid = true;   // 45.3, the charge floor
    return r;
}

void test_frost_is_inert_in_a_working_system() {
    // The guard must be incapable of firing in normal operation - that is what makes it
    // safe to add to the heating path at all. These are the real readings from the evening
    // this was written.
    const Readings r = normalHouse();
    TEST_ASSERT_FALSE(demand(false, r));
    TEST_ASSERT_FALSE(demand(true, r));   // even latched, it releases immediately
    TEST_ASSERT_TRUE(coldSource(r) == Source::NONE);
}

void test_frost_triggers_on_either_reading() {
    Readings r = normalHouse();
    r.flow = TRIGGER_TENTHS;              // exactly 5.0 - inclusive
    TEST_ASSERT_TRUE(demand(false, r));
    TEST_ASSERT_TRUE(coldSource(r) == Source::FLOW);

    r = normalHouse();
    r.tank = TRIGGER_TENTHS;
    TEST_ASSERT_TRUE(demand(false, r));
    TEST_ASSERT_TRUE(coldSource(r) == Source::TANK);

    r = normalHouse();
    r.flow = 40; r.tank = 20;
    TEST_ASSERT_TRUE(coldSource(r) == Source::BOTH);

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
    r.flow = TEMP_INVALID; r.flowValid = false;
    r.tank = TEMP_INVALID; r.tankValid = false;
    TEST_ASSERT_FALSE(demand(false, r));
    TEST_ASSERT_TRUE(coldSource(r) == Source::NONE);

    // A cold value that is marked invalid is still not evidence of frost
    r.flow = 0; r.flowValid = false;
    TEST_ASSERT_FALSE(demand(false, r));

    // ... and the same value marked valid IS
    r.flowValid = true;
    TEST_ASSERT_TRUE(demand(false, r));
}

void test_frost_releases_rather_than_firing_blind() {
    // If every reading goes invalid DURING a frost burn, stop. Continuing to fire with no
    // view of what is being heated is the worse failure, and SensorFailureConfirm plus the
    // sensor fallback are better placed to react than this guard.
    Readings r = normalHouse();
    r.flow = 20;
    TEST_ASSERT_TRUE(demand(false, r));           // burning

    r.flowValid = false; r.tankValid = false;     // both sensors drop out
    TEST_ASSERT_FALSE(demand(true, r));           // release, do not fire blind

    // One surviving cold sensor still holds the demand, though
    r = normalHouse();
    r.flow = 20; r.tankValid = false;
    TEST_ASSERT_TRUE(demand(true, r));
}

void test_frost_one_cold_sensor_holds_while_the_other_is_warm() {
    // The tank sits at 45 year-round, so in the realistic case only the flow circuit is
    // cold. A warm tank must not release a demand the flow still justifies.
    Readings r = normalHouse();
    r.flow = 25;                                   // 2.5 °C in the heating circuit
    TEST_ASSERT_TRUE(demand(false, r));
    TEST_ASSERT_TRUE(demand(true, r));             // tank 45.3 does not release it
    r.flow = RELEASE_TENTHS + 5;
    TEST_ASSERT_FALSE(demand(true, r));            // both clear now
}

void test_frost_thresholds_are_the_agreed_numbers() {
    // Pinned so a change is deliberate: 5.0 matches the previously-dead
    // FREEZE_PROTECTION_TEMP_C constant, 10.0 gives 5 K of hysteresis.
    TEST_ASSERT_EQUAL_INT16(50, TRIGGER_TENTHS);
    TEST_ASSERT_EQUAL_INT16(100, RELEASE_TENTHS);
}
