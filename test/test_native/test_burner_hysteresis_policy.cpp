/**
 * @file test_burner_hysteresis_policy.cpp
 * @brief Unit tests for BurnerHysteresisPolicy (flow dead band, in degrees)
 */

#include <unity.h>
#include <cstdint>

#include "../../include/modules/control/BurnerHysteresisPolicy.h"

// setUp and tearDown are defined in test_main.cpp

using BurnerHysteresisPolicy::Band;
using BurnerHysteresisPolicy::burnerWanted;
using BurnerHysteresisPolicy::bandOrDefault;
using BurnerHysteresisPolicy::bandValid;

// The deployed space-heating band, so these tests fail if the defaults move.
static const Band HEAT = {50, 30};   // +5.0 / -3.0
static const Band WATER = {40, 30};  // +4.0 / -3.0

void test_band_keeps_firing_until_the_top_of_the_band() {
    // Lit, target 38.0. The old percent band quit at flow 40.4 (target +2.4) and that is
    // what produced the 2026-10-22 afterburn; now it runs to 43.0.
    TEST_ASSERT_TRUE(burnerWanted(true, 380, 380, HEAT));   // at target
    TEST_ASSERT_TRUE(burnerWanted(true, 380, 404, HEAT));   // where it used to stop
    TEST_ASSERT_TRUE(burnerWanted(true, 380, 429, HEAT));   // 0.1 below the edge
    TEST_ASSERT_FALSE(burnerWanted(true, 380, 430, HEAT));  // target + 5.0 exactly -> OFF
    TEST_ASSERT_FALSE(burnerWanted(true, 380, 450, HEAT));
}

void test_band_relights_only_at_the_bottom_of_the_band() {
    // Not lit, target 38.0. The old band relit at 37.2 (target -0.8), two minutes before the
    // room loop ended the request anyway. Now it waits for 35.0.
    TEST_ASSERT_FALSE(burnerWanted(false, 380, 372, HEAT));  // the 2026-10-22 relight point
    TEST_ASSERT_FALSE(burnerWanted(false, 380, 351, HEAT));  // 0.1 above the edge
    TEST_ASSERT_TRUE(burnerWanted(false, 380, 350, HEAT));   // target - 3.0 exactly -> ON
    TEST_ASSERT_TRUE(burnerWanted(false, 380, 263, HEAT));   // cold-start flow, lights at once
}

void test_band_is_asymmetric_which_is_the_whole_point() {
    // The same flow reading gives opposite answers depending on whether the burner is lit.
    // Between the edges nothing changes state - that gap is the dead band.
    for (Temperature_t flow = 351; flow <= 429; flow += 10) {
        TEST_ASSERT_TRUE(burnerWanted(true, 380, flow, HEAT));    // stays lit
        TEST_ASSERT_FALSE(burnerWanted(false, 380, flow, HEAT));  // stays out
    }
}

void test_band_width_is_eight_kelvin_for_heating_and_seven_for_water() {
    // Period is proportional to band width at constant duty, so the width is the dial.
    TEST_ASSERT_EQUAL_INT16(80, HEAT.offAbove + HEAT.onBelow);
    TEST_ASSERT_EQUAL_INT16(70, WATER.offAbove + WATER.onBelow);
    // Water is NOT wider than heating: a tank charge already overshoots onto a target that
    // chases the tank, and peak flow must stay clear of an unread thermostat dial.
    TEST_ASSERT_TRUE(WATER.offAbove < HEAT.offAbove);
}

void test_band_tracks_a_moving_target() {
    // A DHW charge recomputes target = tank + delta every cycle, so the edges move with it.
    // 2026-10-23 08:58:30: target 57.0, flow 59.3 -> still inside, keeps firing.
    TEST_ASSERT_TRUE(burnerWanted(true, 570, 593, WATER));
    // 09:04:30 restart check: target 63.0, flow 60.1 -> 2.9 below, not yet 3.0. Holds off.
    TEST_ASSERT_FALSE(burnerWanted(false, 630, 601, WATER));
    TEST_ASSERT_TRUE(burnerWanted(false, 630, 600, WATER));
}

void test_band_rejects_a_nonsense_band_as_a_pair() {
    TEST_ASSERT_TRUE(bandValid(HEAT));
    TEST_ASSERT_TRUE(bandValid(WATER));
    TEST_ASSERT_FALSE(bandValid({0, 30}));     // zero edge would chatter on sensor noise
    TEST_ASSERT_FALSE(bandValid({50, 0}));
    TEST_ASSERT_FALSE(bandValid({-50, 30}));   // negative inverts the hysteresis
    TEST_ASSERT_FALSE(bandValid({500, 30}));   // above MAX_BAND_EDGE
    TEST_ASSERT_FALSE(bandValid({4, 30}));     // below MIN_BAND_EDGE (0.5)
    TEST_ASSERT_TRUE(bandValid({5, 5}));       // exactly at the floor on both edges
    TEST_ASSERT_TRUE(bandValid({150, 150}));   // exactly at the ceiling

    // Falls back as a PAIR, per mode. Half-clamping would leave a band nobody chose.
    const Band badHeat = bandOrDefault({0, 999}, false);
    TEST_ASSERT_EQUAL_INT16(50, badHeat.offAbove);
    TEST_ASSERT_EQUAL_INT16(30, badHeat.onBelow);
    const Band badWater = bandOrDefault({0, 999}, true);
    TEST_ASSERT_EQUAL_INT16(40, badWater.offAbove);
    TEST_ASSERT_EQUAL_INT16(30, badWater.onBelow);
    // A valid band passes through untouched
    const Band good = bandOrDefault({70, 20}, false);
    TEST_ASSERT_EQUAL_INT16(70, good.offAbove);
    TEST_ASSERT_EQUAL_INT16(20, good.onBelow);
}

void test_band_cannot_push_flow_near_the_safety_interlock() {
    // Worst case: the largest target heating_high_limit allows (75.0) with the widest
    // permitted offAbove (15.0) stops the burner at 90.0, which is 20 K below the 110.0
    // MAX_BOILER_TEMP_C interlock and the boiler's STB.
    const Band widest = {BurnerHysteresisPolicy::MAX_BAND_EDGE,
                         BurnerHysteresisPolicy::MAX_BAND_EDGE};
    TEST_ASSERT_TRUE(bandValid(widest));
    TEST_ASSERT_FALSE(burnerWanted(true, 750, 900, widest));
    TEST_ASSERT_TRUE(burnerWanted(true, 750, 899, widest));
    // No int16 wrap at the ceiling of the target range either
    TEST_ASSERT_FALSE(burnerWanted(true, 1100, 1250, widest));
}

void test_band_restarts_from_off_after_a_request_pause() {
    // modulatingCycleStart() restarts every request from OFF, so the caller passes
    // currentlyOn = false. If it wrongly passed true, a new request on a warm circuit would
    // inherit the "keep firing" branch and light the burner at target +4.9.
    TEST_ASSERT_FALSE(burnerWanted(false, 380, 429, HEAT));  // correct: hold off
    TEST_ASSERT_TRUE(burnerWanted(true, 380, 429, HEAT));    // what the bug would give
}
