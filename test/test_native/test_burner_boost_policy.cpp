/**
 * @file test_burner_boost_policy.cpp
 * @brief Unit tests for BurnerBoostPolicy (stage-2 lock-out for mild-weather heating)
 */

#include <unity.h>
#include <cstdint>

#include "../../include/modules/control/BurnerBoostPolicy.h"

// setUp and tearDown are defined in test_main.cpp

using BurnerBoostPolicy::fullPowerAllowed;

// The defaults this policy is deployed with, so the tests fail if either moves.
static constexpr Temperature_t BOOST_MIN = 500;   // h/boostMin default 50.0 °C
static constexpr Temperature_t FLOOR     = 380;   // h/sLo
static constexpr Temperature_t CEILING   = 750;   // h/sHi

void test_boost_denied_for_mild_weather_heating_targets() {
    // The measured case: 2026-10-19, outside 11.5 °C, curve target 38.0 clamped at the
    // floor. Boost ran for 11 minutes here before this policy existed.
    TEST_ASSERT_FALSE(fullPowerAllowed(false, false, 380, BOOST_MIN));
    TEST_ASSERT_FALSE(fullPowerAllowed(false, false, 390, BOOST_MIN));
    TEST_ASSERT_FALSE(fullPowerAllowed(false, false, 449, BOOST_MIN));
    TEST_ASSERT_FALSE(fullPowerAllowed(false, false, 499, BOOST_MIN));
}

void test_boost_allowed_for_cold_weather_heating_targets() {
    // 50.0 °C is about +4 °C outside on the 2026-10-19 curve; below that, deep winter.
    TEST_ASSERT_TRUE(fullPowerAllowed(false, false, 500, BOOST_MIN));
    TEST_ASSERT_TRUE(fullPowerAllowed(false, false, 556, BOOST_MIN));  // ~0 °C outside
    TEST_ASSERT_TRUE(fullPowerAllowed(false, false, 700, BOOST_MIN));  // ~-12 °C outside
}

void test_boost_threshold_is_inclusive_at_exactly_the_limit() {
    // A target sitting exactly on h/boostMin qualifies. Stated explicitly because the
    // off-by-one here is the difference between a gate that engages at the documented
    // temperature and one that engages 0.1 °C later.
    TEST_ASSERT_TRUE(fullPowerAllowed(false, false, BOOST_MIN, BOOST_MIN));
    TEST_ASSERT_FALSE(fullPowerAllowed(false, false, BOOST_MIN - 1, BOOST_MIN));
}

void test_boost_always_allowed_for_water_charge() {
    // The tank is a real heat sink and the charge target is tank + 10 °C, not a weather
    // curve, so a 44.0 °C charge target must still get stage 2. This is the case the
    // policy must NOT touch: tank charges are where boost belongs.
    TEST_ASSERT_TRUE(fullPowerAllowed(true, false, 440, BOOST_MIN));
    TEST_ASSERT_TRUE(fullPowerAllowed(true, false, 380, BOOST_MIN));
    TEST_ASSERT_TRUE(fullPowerAllowed(true, false, 590, BOOST_MIN));
}

void test_boost_always_allowed_while_autotuning() {
    // PIDAutoTuner drives a deliberate full-power step; gating it would silently corrupt
    // every future autotune run at a low target (space heating on a mild day).
    TEST_ASSERT_TRUE(fullPowerAllowed(false, true, 380, BOOST_MIN));
    TEST_ASSERT_TRUE(fullPowerAllowed(false, true, 400, BOOST_MIN));
}

void test_boost_gate_degenerate_ends_of_the_parameter_range() {
    // h/boostMin at or below h/sLo: every heating target qualifies, gate effectively off.
    TEST_ASSERT_TRUE(fullPowerAllowed(false, false, FLOOR, FLOOR));
    TEST_ASSERT_TRUE(fullPowerAllowed(false, false, 600, FLOOR));
    // h/boostMin at h/sHi: only a target pinned at the ceiling qualifies.
    TEST_ASSERT_FALSE(fullPowerAllowed(false, false, 749, CEILING));
    TEST_ASSERT_TRUE(fullPowerAllowed(false, false, CEILING, CEILING));
}

void test_boost_denied_for_invalid_or_zero_target() {
    // Fail-safe direction: no usable target means stage 1 only, never stage 2. The burner
    // still runs, so this costs a slower recovery and not a cold house.
    TEST_ASSERT_FALSE(fullPowerAllowed(false, false, 0, BOOST_MIN));
    TEST_ASSERT_FALSE(fullPowerAllowed(false, false, TEMP_INVALID, BOOST_MIN));
    TEST_ASSERT_FALSE(fullPowerAllowed(false, false, -100, BOOST_MIN));
}

void test_boost_water_and_autotune_override_an_invalid_target() {
    // Both exemptions are checked before the target, deliberately: a water charge or a
    // tuning run must not lose stage 2 because of a transient bad target reading.
    TEST_ASSERT_TRUE(fullPowerAllowed(true, false, 0, BOOST_MIN));
    TEST_ASSERT_TRUE(fullPowerAllowed(false, true, 0, BOOST_MIN));
}
