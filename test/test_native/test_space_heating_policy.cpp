/**
 * @file test_space_heating_policy.cpp
 * @brief Unit tests for SpaceHeatingPolicy (weather mode outside threshold hysteresis)
 */

#include <unity.h>
#include <cstdint>

#include "../../include/modules/control/SpaceHeatingPolicy.h"

// setUp and tearDown are defined in test_main.cpp

using namespace SpaceHeatingPolicy;

void test_space_heating_outside_threshold_start_and_stop_limits() {
    // Threshold 15.0 °C: start below 15.0, keep running until 16.0
    TEST_ASSERT_TRUE(outsideColdForHeating(149, 150, false));
    TEST_ASSERT_FALSE(outsideColdForHeating(150, 150, false));
    TEST_ASSERT_TRUE(outsideColdForHeating(150, 150, true));
    TEST_ASSERT_TRUE(outsideColdForHeating(159, 150, true));
    TEST_ASSERT_FALSE(outsideColdForHeating(160, 150, true));
}

void test_space_heating_outside_noise_does_not_toggle() {
    // 2026-09-15: without hysteresis, readings around the threshold switched heating on and
    // off every 5 s cycle. Noise of +/-0.2 °C around 15.0 °C must give a single start.
    const int16_t readings[] = {151, 149, 151, 150, 148, 152, 149, 151, 150, 152};
    bool heating = false;
    int starts = 0;
    int stops = 0;
    for (int16_t r : readings) {
        const bool next = outsideColdForHeating(r, 150, heating);
        if (next && !heating) starts++;
        if (!next && heating) stops++;
        heating = next;
    }
    TEST_ASSERT_EQUAL_INT(1, starts);
    TEST_ASSERT_EQUAL_INT(0, stops);
    TEST_ASSERT_TRUE(heating);
}

// ---------------------------------------------------------------------------
// Room backstop and the full weather decision (2026-10-25)
// ---------------------------------------------------------------------------

void test_space_heating_room_backstop_is_asymmetric() {
    // Target 19.2, margin 1.0 -> limit 20.2; hysteresis 0.5 -> restart test at 19.7.
    // While heating: stop only ABOVE the limit.
    TEST_ASSERT_FALSE(roomAtBackstop(202, 192, 10, 5, true));
    TEST_ASSERT_TRUE(roomAtBackstop(203, 192, 10, 5, true));
    // While off: the predicate is the RESTART test, so the deadband satisfies it.
    TEST_ASSERT_TRUE(roomAtBackstop(197, 192, 10, 5, false));
    TEST_ASSERT_FALSE(roomAtBackstop(196, 192, 10, 5, false));
    // The live numbers from the swap: the room sat at 19.8, one tenth inside the backstop,
    // which is why heating was off and why losing the term turned it on.
    TEST_ASSERT_TRUE(roomAtBackstop(198, 192, 10, 5, false));
}

void test_space_heating_unknown_room_must_not_license_heating() {
    // THE REGRESSION. 2026-10-25 00:44 UTC: pulling the room sensor fired the burner for two
    // minutes. The backstop was computed only for a valid reading and defaulted to "not
    // overheated", so an unreadable room sensor removed the only term holding heating off.
    // Outside cold, room target configured, no usable room value -> must NOT heat.
    const WeatherDecision d = weatherDecision(true, true, false, false);
    TEST_ASSERT_FALSE_MESSAGE(d.heat, "a missing room reading must never turn heating ON");
    TEST_ASSERT_EQUAL(WeatherBlock::ROOM_UNKNOWN, d.block);
}

void test_space_heating_target_substitution_would_have_reopened_the_hole() {
    // The curve falls back to "room = target" when the hold expires, which is fine for a flow
    // setpoint but not here: target 19.2 is BELOW the 19.7 restart limit, so a TARGET
    // substitution reads as "not at backstop" and would license heating exactly as the
    // original bug did. Hence the decision keys on isMeasurement(), not on having a number.
    TEST_ASSERT_FALSE(roomAtBackstop(192, 192, 10, 5, false));          // target alone: not blocking
    TEST_ASSERT_FALSE(weatherDecision(true, true, false, false).heat);  // but the decision blocks
}

void test_space_heating_weather_decision_outside_gate_is_reported_first() {
    // 2026-10-18: at 20 C outside against a 13 C threshold the topic said "backstop", which
    // sent the user after a room margin that was not the cause. The gate wins the reason even
    // when the room is also at its limit, and even when the room reading is missing.
    TEST_ASSERT_EQUAL(WeatherBlock::OUTSIDE, weatherDecision(false, true, true, true).block);
    TEST_ASSERT_EQUAL(WeatherBlock::OUTSIDE, weatherDecision(false, true, false, false).block);
    TEST_ASSERT_FALSE(weatherDecision(false, true, true, false).heat);
}

void test_space_heating_weather_decision_heats_and_blocks_on_the_room() {
    // The two normal outcomes with a working sensor.
    const WeatherDecision on = weatherDecision(true, true, true, false);
    TEST_ASSERT_TRUE(on.heat);
    TEST_ASSERT_EQUAL(WeatherBlock::NONE, on.block);

    const WeatherDecision off = weatherDecision(true, true, true, true);
    TEST_ASSERT_FALSE(off.heat);
    TEST_ASSERT_EQUAL(WeatherBlock::ROOM_BACKSTOP, off.block);
}

void test_space_heating_no_room_target_keeps_weather_only_control() {
    // With no room target there is no backstop to evaluate, and weather-only control is the
    // intended mode - so a missing room reading must NOT stop heating here. Without this
    // carve-out the fix would silently disable heating for anyone running without a room
    // target configured.
    TEST_ASSERT_TRUE(weatherDecision(true, false, false, false).heat);
    TEST_ASSERT_TRUE(weatherDecision(true, false, false, true).heat);
    TEST_ASSERT_FALSE(weatherDecision(false, false, false, false).heat);
}

void test_flow_limits_inverted_pair_is_ordered_not_defaulted() {
    // h/sLo (30.0-60.0) and h/sHi (50.0-90.0) have OVERLAPPING registered ranges and no
    // cross-field validator, so an inverted pair is accepted one key at a time - each value
    // individually legal, only the pair nonsense. BurnerRequestManager clamps low-then-high, so
    // with low > high the second clamp always won and every target landed on the LOWER of the
    // two: failing safe, but honouring neither configured bound and saying nothing.
    int16_t low = 600, high = 500;          // 60.0 / 50.0 - inverted, both in range
    TEST_ASSERT_FALSE(SpaceHeatingPolicy::flowLimitsValid(low, high));
    TEST_ASSERT_TRUE(SpaceHeatingPolicy::orderFlowLimits(low, high));
    TEST_ASSERT_EQUAL_INT16(500, low);
    TEST_ASSERT_EQUAL_INT16(600, high);
    TEST_ASSERT_TRUE(SpaceHeatingPolicy::flowLimitsValid(low, high));

    // SWAPPING, not falling back to defaults: it preserves both magnitudes the user actually
    // typed and cannot invent a bound they did not. A fallback to the absolute safety limits
    // would RAISE a configured 75.0 ceiling to the 110.0 hard limit, which is the wrong
    // direction, and restating the struct defaults here would have added a fourth definition of
    // a value that already had three.
    int16_t ok_low = 380, ok_high = 750;
    TEST_ASSERT_FALSE_MESSAGE(SpaceHeatingPolicy::orderFlowLimits(ok_low, ok_high),
                              "a valid pair must be left alone and report no swap");
    TEST_ASSERT_EQUAL_INT16(380, ok_low);
    TEST_ASSERT_EQUAL_INT16(750, ok_high);

    // Equal is not inverted - nothing to swap, and the caller must not warn about it.
    int16_t eq_low = 500, eq_high = 500;
    TEST_ASSERT_FALSE(SpaceHeatingPolicy::orderFlowLimits(eq_low, eq_high));
    // ...but it is not a usable range either, so validity and orderedness are separate questions.
    TEST_ASSERT_FALSE(SpaceHeatingPolicy::flowLimitsValid(eq_low, eq_high));

    // Zero or negative is rejected by validity regardless of order (an unset pair).
    TEST_ASSERT_FALSE(SpaceHeatingPolicy::flowLimitsValid(0, 750));
    TEST_ASSERT_FALSE(SpaceHeatingPolicy::flowLimitsValid(380, 0));
}
