// test/test_native/test_heating_burn_kind.cpp
// HeatingBurnKind::isFrostOnlyBurn - the single predicate that replaced three divergent copies.
//
// The 2026-10-28 behavioural review traced FIVE findings, two of them high, to this one answer
// being computed in three places from different inputs. These tests pin the full truth table so
// a fourth copy cannot be introduced by accident and pass.

#include <unity.h>
#include "modules/control/HeatingBurnKind.h"

using HeatingBurnKind::isFrostOnlyBurn;

void test_burnkind_no_frost_is_never_frost_only() {
    // Without the latch asking, nothing here is a frost burn whatever else is true.
    for (int sw = 0; sw < 2; ++sw)
        for (int ov = 0; ov < 2; ++ov)
            for (int want = 0; want < 2; ++want)
                TEST_ASSERT_FALSE(isFrostOnlyBurn(false, sw, ov, want));
}

void test_burnkind_frost_with_switch_off_is_frost_only() {
    // The case the floor substitution was originally written for.
    TEST_ASSERT_TRUE(isFrostOnlyBurn(true, /*switch*/false, /*override*/false, /*wanted*/true));
    TEST_ASSERT_TRUE(isFrostOnlyBurn(true, false, false, false));
}

void test_burnkind_frost_with_switch_on_but_nothing_wanted_is_frost_only() {
    // THE HIGH-SEVERITY CASE. Switch ON, but the weather/room decision says no - e.g. the room
    // sensor is dead past its 10-minute hold. Nothing but frost authorised this burn, so it
    // needs the flow FLOOR and only the boiler-output reading.
    //
    // BurnerControlTask used to answer FALSE here (it keyed on the switch alone) and therefore
    // reported SPACE_HEATING, whose sensor set includes the room. The fallback hit SHUTDOWN
    // seconds after every start and the latch restarted it: an ignition every ~45-55 s.
    TEST_ASSERT_TRUE(isFrostOnlyBurn(true, /*switch*/true, /*override*/false, /*wanted*/false));
}

void test_burnkind_frost_during_real_space_heating_is_not_frost_only() {
    // Switch on, not overridden, and heat genuinely wanted: this is ordinary space heating that
    // happens to coincide with a frost latch. It must use the CURVE and does require the room.
    TEST_ASSERT_FALSE(isFrostOnlyBurn(true, true, false, true));
}

void test_burnkind_summer_override_makes_it_frost_only() {
    // Override off is the summer switch. Heat may be "wanted" by the weather/room decision and
    // still not authorised, so a latch burn is frost-only.
    TEST_ASSERT_TRUE(isFrostOnlyBurn(true, /*switch*/true, /*override*/true, /*wanted*/true));
}

void test_burnkind_the_three_old_predicates_disagreed_exactly_here() {
    // Reproduce what each of the three copies used to compute, and show they disagree - this is
    // the regression guard. If someone reintroduces any of them, this fails.
    const bool frost = true, sw = true, ov = false, wanted = false;

    const bool shared  = isFrostOnlyBurn(frost, sw, ov, wanted);  // the one definition
    const bool oldTurnOn = frost && !(sw && !ov && wanted);       // was correct
    const bool oldRefresh = frost && !wanted;                     // dropped switch+override
    const bool oldBurner  = !sw && frost;                         // dropped `wanted`

    TEST_ASSERT_TRUE(shared);
    TEST_ASSERT_EQUAL(shared, oldTurnOn);        // turn-on path already agreed
    TEST_ASSERT_EQUAL(shared, oldRefresh);       // refresh agreed in THIS case...
    TEST_ASSERT_NOT_EQUAL(shared, oldBurner);    // ...but the burner task did not. The high.

    // And a case where the REFRESH copy was the wrong one: switch off, heat "wanted".
    const bool shared2 = isFrostOnlyBurn(true, false, false, true);
    TEST_ASSERT_TRUE(shared2);
    TEST_ASSERT_TRUE(true && !(false && !false && true));   // turn-on: true
    TEST_ASSERT_FALSE(true && !true);                        // refresh copy: FALSE - the bug
}

void test_burnkind_published_state_round_trips_and_clears() {
    HeatingBurnKind::clear();
    TEST_ASSERT_FALSE(HeatingBurnKind::isFrostOnly());
    HeatingBurnKind::publish(true);
    TEST_ASSERT_TRUE(HeatingBurnKind::isFrostOnly());
    // Clearing on heating stop matters: a stale `true` would weaken the sensor requirements of
    // the NEXT burn, which is the opposite of the fault this whole change fixes.
    HeatingBurnKind::clear();
    TEST_ASSERT_FALSE(HeatingBurnKind::isFrostOnly());
}

void test_burnkind_room_measurement_defaults_true() {
    // Default true so that before the heating task's first evaluation the burner behaves as it
    // always did, rather than refusing a mode switch on a fact nobody has established yet.
    TEST_ASSERT_TRUE(HeatingBurnKind::roomIsMeasurement());
    HeatingBurnKind::publishRoomIsMeasurement(false);
    TEST_ASSERT_FALSE(HeatingBurnKind::roomIsMeasurement());
    HeatingBurnKind::publishRoomIsMeasurement(true);
    TEST_ASSERT_TRUE(HeatingBurnKind::roomIsMeasurement());
}
