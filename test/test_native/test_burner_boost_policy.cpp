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

// ===== Water charge gate: w/boost, w/boostGap (2026-11-21) =====

using BurnerBoostPolicy::waterFullPowerAllowed;
static constexpr Temperature_t GAP = BurnerBoostPolicy::WATER_BOOST_GAP_DEFAULT;  // 15.0 K

void test_water_boost_always_is_the_zero_default() {
    // 0 must be today's behaviour: a device that never stored w/boost loads 0.
    TEST_ASSERT_EQUAL(0, BurnerBoostPolicy::WATER_BOOST_ALWAYS);
    TEST_ASSERT_TRUE(waterFullPowerAllowed(0, 550, 300, true, GAP));
    TEST_ASSERT_TRUE(waterFullPowerAllowed(0, 550, 590, true, GAP));  // even above target
}

void test_water_boost_never_denies_every_charge() {
    TEST_ASSERT_FALSE(waterFullPowerAllowed(BurnerBoostPolicy::WATER_BOOST_NEVER, 550, 300, true, GAP));
    TEST_ASSERT_FALSE(waterFullPowerAllowed(BurnerBoostPolicy::WATER_BOOST_NEVER, 650, 200, true, GAP));
}

void test_water_boost_warmup_only_while_boiler_is_cold() {
    const uint8_t W = BurnerBoostPolicy::WATER_BOOST_WARMUP;
    // The 2026-11-20 06:55 charge: output 30.4 against a 55.0 target -> 24.6 K short, boost.
    TEST_ASSERT_TRUE(waterFullPowerAllowed(W, 550, 304, true, GAP));
    // 07:09 restart at 50.5 against 55.0: 4.5 K short - stage 1 only.
    TEST_ASSERT_FALSE(waterFullPowerAllowed(W, 550, 505, true, GAP));
    // Boundary is strict: exactly gap below is no longer "more than gap below".
    TEST_ASSERT_TRUE(waterFullPowerAllowed(W, 550, 399, true, GAP));
    TEST_ASSERT_FALSE(waterFullPowerAllowed(W, 550, 400, true, GAP));
}

void test_water_boost_warmup_fails_safe_to_stage_1() {
    const uint8_t W = BurnerBoostPolicy::WATER_BOOST_WARMUP;
    TEST_ASSERT_FALSE(waterFullPowerAllowed(W, 550, 300, false, GAP));          // invalid reading
    TEST_ASSERT_FALSE(waterFullPowerAllowed(W, 550, TEMP_INVALID, true, GAP));
    TEST_ASSERT_FALSE(waterFullPowerAllowed(W, 0, 300, true, GAP));             // no target
    TEST_ASSERT_FALSE(waterFullPowerAllowed(W, TEMP_INVALID, 300, true, GAP));
    TEST_ASSERT_FALSE(waterFullPowerAllowed(7, 550, 300, true, GAP));           // unknown mode
}
