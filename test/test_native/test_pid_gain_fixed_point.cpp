/**
 * @file test_pid_gain_fixed_point.cpp
 * @brief Unit tests for float -> fixed-point PID gain conversion
 *
 * Regression for the boiler PID being inert: float gains were passed to the
 * fixed-point PID without scaling and truncated (Kp 34.206 -> 34 = 0.034).
 */

#include <unity.h>
#include <cstdint>
#include <cmath>

#include "../../include/modules/control/PIDGainFixedPoint.h"

// setUp and tearDown are defined in test_main.cpp

// Mirrors PIDControlModuleFixedPoint P term and BoilerTempController's
// PID adjustment -> 0..100% power mapping (P only, error in tenths of °C).
static int32_t pidPowerFromP(int32_t kpFixed, int16_t errorTenths) {
    int64_t pRaw = static_cast<int64_t>(kpFixed) * errorTenths;
    int32_t adjustment = static_cast<int32_t>(pRaw / PIDGainFixedPoint::SCALE);
    if (adjustment > 1000) adjustment = 1000;    // OUTPUT_MAX
    if (adjustment < -1000) adjustment = -1000;  // OUTPUT_MIN
    int32_t power = 50 + adjustment / 10;
    if (power < 0) power = 0;
    if (power > 100) power = 100;
    return power;
}

void test_pid_gain_conversion_scales_by_1000() {
    TEST_ASSERT_EQUAL_INT32(34206, PIDGainFixedPoint::fromFloat(34.206f));
    TEST_ASSERT_EQUAL_INT32(189, PIDGainFixedPoint::fromFloat(0.1893f));
    TEST_ASSERT_EQUAL_INT32(10000, PIDGainFixedPoint::fromFloat(10.0f));
    TEST_ASSERT_EQUAL_INT32(1000, PIDGainFixedPoint::fromFloat(1.0f));
    TEST_ASSERT_EQUAL_INT32(500, PIDGainFixedPoint::fromFloat(0.5f));
    TEST_ASSERT_EQUAL_INT32(100, PIDGainFixedPoint::fromFloat(0.1f));
}

void test_pid_gain_conversion_rejects_invalid() {
    TEST_ASSERT_EQUAL_INT32(0, PIDGainFixedPoint::fromFloat(0.0f));
    TEST_ASSERT_EQUAL_INT32(0, PIDGainFixedPoint::fromFloat(-1.0f));
    TEST_ASSERT_EQUAL_INT32(0, PIDGainFixedPoint::fromFloat(NAN));
    TEST_ASSERT_EQUAL_INT32(0, PIDGainFixedPoint::fromFloat(INFINITY));
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, PIDGainFixedPoint::fromFloat(1.0e9f));
}

void test_pid_scaled_gain_commands_off_above_target() {
    // Boiler 3°C above target (error -30 tenths) with autotuned Kp 34.206.
    // OFF threshold is pidOutput < 35 (BoilerTempController offThreshold).
    int32_t power = pidPowerFromP(PIDGainFixedPoint::fromFloat(34.206f), -30);
    TEST_ASSERT_TRUE(power < 35);

    // The old implicit truncation (34 instead of 34206) left output at ~50%:
    // never OFF (needs < 35) and never ON from OFF (needs > 55).
    TEST_ASSERT_EQUAL_INT32(50, pidPowerFromP(34, -30));
    int32_t truncatedFarAbove = pidPowerFromP(34, -320);  // even 32°C above target
    TEST_ASSERT_TRUE(truncatedFarAbove >= 35 && truncatedFarAbove <= 55);
}

void test_pid_scaled_gain_commands_full_below_target() {
    // Boiler 3°C below target: must exceed fullThreshold (75) -> FULL power.
    int32_t power = pidPowerFromP(PIDGainFixedPoint::fromFloat(34.206f), 30);
    TEST_ASSERT_TRUE(power > 75);
}
