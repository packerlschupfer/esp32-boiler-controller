/**
 * @file test_water_charge_floor.cpp
 * @brief Unit tests for WaterChargePolicy::holdChargeTarget (monotonic charge target)
 */

#include <unity.h>
#include <cstdint>

#include "../../include/modules/control/WaterChargePolicy.h"

// setUp and tearDown are defined in test_main.cpp

using WaterChargePolicy::holdChargeTarget;

void test_charge_floor_lets_a_rising_target_through() {
    // The normal case: the tank warms, the target rises, and the floor does nothing at all.
    // 7 of 7 draw-free charges in the baseline behaved this way and must be unaffected.
    TEST_ASSERT_EQUAL_INT16(560, holdChargeTarget(560, 550));
    TEST_ASSERT_EQUAL_INT16(610, holdChargeTarget(610, 550));
}

void test_charge_floor_blocks_a_falling_target() {
    // The measured shower, 2026-11-07: charge started at tank 39.7 -> target 54.7, the draw
    // pulled the tank to 32.9 so the raw target fell to 47.9. Held at the start value.
    TEST_ASSERT_EQUAL_INT16(547, holdChargeTarget(479, 547));
    TEST_ASSERT_EQUAL_INT16(550, holdChargeTarget(490, 550));
}

void test_charge_floor_is_inclusive_at_the_floor() {
    // Equal is not below, so it passes through unchanged rather than being "held".
    TEST_ASSERT_EQUAL_INT16(550, holdChargeTarget(550, 550));
}

void test_charge_floor_would_have_kept_the_burner_lit() {
    // The whole point, in the numbers that produced the defect. The water dead band turns the
    // burner OFF at target + 4.0. With the raw target the boiler at 54.0 was above the
    // threshold and cut out; with the floor held it is below and the burner stays lit.
    const int16_t floor = 547;   // charge-start target, tank 39.7 + 15.0
    const int16_t raw   = 479;   // during the draw, tank 32.9 + 15.0
    const int16_t boiler = 540;  // measured boiler output when it cut out
    const int16_t bandOff = 40;  // w/bandOff, +4.0 C

    TEST_ASSERT_TRUE(boiler > raw + bandOff);                        // raw: burner OFF
    TEST_ASSERT_FALSE(boiler > holdChargeTarget(raw, floor) + bandOff);  // held: stays ON
}

void test_charge_floor_does_nothing_without_a_draw() {
    // A charge that only ever warms never touches the floor, so this cannot explain or alter
    // the inherent 2-burst split seen on every draw-free charge.
    int16_t floorT = 550;
    for (int16_t tankTenths = 400; tankTenths <= 500; tankTenths += 10) {
        const int16_t raw = static_cast<int16_t>(tankTenths + 150);
        TEST_ASSERT_EQUAL_INT16(raw, holdChargeTarget(raw, floorT));
    }
}

void test_charge_floor_zero_floor_is_inert() {
    // chargeFloorTarget is 0 while WheaterOff. If it were ever consulted outside a charge the
    // floor must not raise anything - 0 is below every real target.
    TEST_ASSERT_EQUAL_INT16(550, holdChargeTarget(550, 0));
    TEST_ASSERT_EQUAL_INT16(380, holdChargeTarget(380, 0));
}

void test_charge_floor_holds_through_a_deep_draw() {
    // The deepest observed fall was 22.7 C. The floor must hold across the whole excursion,
    // not just small dips - those deep draws are the 3-4 burst charges this targets.
    const int16_t floor = 550;
    for (int16_t raw = 550; raw >= 320; raw -= 10) {
        TEST_ASSERT_EQUAL_INT16(floor, holdChargeTarget(raw, floor));
    }
}
