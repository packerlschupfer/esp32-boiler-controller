/**
 * @file test_autotune_relay_config.cpp
 * @brief Unit tests for AutotuneRelayConfig (autotune amplitude/hysteresis settings)
 */

#include <unity.h>
#include <cmath>

#include "../../include/modules/control/AutotuneRelayConfig.h"

// setUp and tearDown are defined in test_main.cpp

using namespace AutotuneRelayConfig;

void test_autotune_amplitude_setting_used_within_range() {
    TEST_ASSERT_EQUAL_FLOAT(50.0f, amplitudeOrDefault(50.0f, 50.0f));
    TEST_ASSERT_EQUAL_FLOAT(30.0f, amplitudeOrDefault(30.0f, 50.0f));
    TEST_ASSERT_EQUAL_FLOAT(100.0f, amplitudeOrDefault(100.0f, 50.0f));
    // Out of range or not a number: default
    TEST_ASSERT_EQUAL_FLOAT(50.0f, amplitudeOrDefault(5.0f, 50.0f));
    TEST_ASSERT_EQUAL_FLOAT(50.0f, amplitudeOrDefault(150.0f, 50.0f));
    TEST_ASSERT_EQUAL_FLOAT(50.0f, amplitudeOrDefault(NAN, 50.0f));
}

void test_autotune_hysteresis_setting_used_within_range() {
    TEST_ASSERT_EQUAL_FLOAT(1.0f, hysteresisOrDefault(1.0f, 1.0f));
    TEST_ASSERT_EQUAL_FLOAT(2.0f, hysteresisOrDefault(2.0f, 1.0f));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, hysteresisOrDefault(0.2f, 1.0f));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, hysteresisOrDefault(12.0f, 1.0f));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, hysteresisOrDefault(INFINITY, 1.0f));
}

void test_autotune_amplitude_vs_two_stage_swing() {
    // Two-stage relay test OFF <-> FULL: 50 % is the physical half-swing
    TEST_ASSERT_TRUE(matchesOutputSwing(50.0f, TWO_STAGE_SWING));
    TEST_ASSERT_FALSE(matchesOutputSwing(40.0f, TWO_STAGE_SWING));
    // Ku = 4 d / (pi a) is linear in d: amplitude 40 gives 0.8x the gains
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.8f, gainScale(40.0f, TWO_STAGE_SWING));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.0f, gainScale(100.0f, TWO_STAGE_SWING));
}
