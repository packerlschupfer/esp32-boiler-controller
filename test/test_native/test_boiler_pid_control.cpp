/**
 * @file test_boiler_pid_control.cpp
 * @brief Boiler PID step and power level mapping, through the firmware code
 *
 * FixedPointPIDStep and BoilerPowerLevel are what PIDControlModuleFixedPoint and
 * BoilerTempController run; the former tests re-implemented only the P term
 * (review 2026-09-14 pid-8), so integral, anti-windup and hysteresis were untested.
 */

#include <unity.h>
#include <cstdint>

#include "../../include/modules/control/BoilerPowerLevel.h"

// setUp and tearDown are defined in test_main.cpp

using BoilerPowerLevel::Level;

namespace {

// BoilerTempController::initialize(): integral limits SafetyConfig::Defaults
// (+/-100000), output limits +/-PIDGainFixedPoint::POWER_ADJUSTMENT_LIMIT
FixedPointPIDStep::Limits boilerPidLimits() {
    FixedPointPIDStep::Limits limits = {
        -100000, 100000,
        static_cast<int16_t>(-PIDGainFixedPoint::POWER_ADJUSTMENT_LIMIT),
        PIDGainFixedPoint::POWER_ADJUSTMENT_LIMIT
    };
    return limits;
}

// Space heating gains measured 2026-09-14 (Kp 13.22, Ki 0.0453), fixed-point
const int32_t KP_SPACE = PIDGainFixedPoint::fromFloat(13.22f);
const int32_t KI_SPACE = PIDGainFixedPoint::fromFloat(0.0453f);
const uint32_t CYCLE_MS = BoilerPowerLevel::PID_NOMINAL_DT_MS;

// One BoilerTempController cycle (without anti-flapping)
Level cycle(Level last, FixedPointPIDStep::State& pid, int16_t target, int16_t boiler) {
    return BoilerPowerLevel::nextModulatingLevel(last, pid, boilerPidLimits(), target, boiler,
                                                 KP_SPACE, KI_SPACE, 0, CYCLE_MS,
                                                 BoilerPowerLevel::defaultThresholds());
}

} // namespace

void test_pid_step_reference_values() {
    const FixedPointPIDStep::Limits limits = boilerPidLimits();
    FixedPointPIDStep::State pid = FixedPointPIDStep::resetState();

    // 1.0 C below target: P 132, no D on the first step, integral 10*2500/1000 = 25, I 45*25/1000 = 1
    TEST_ASSERT_EQUAL_INT16(133, FixedPointPIDStep::step(pid, limits, 470, 460, 13220, 45, 5000, 2500));
    TEST_ASSERT_EQUAL_INT32(25, pid.integral);
    TEST_ASSERT_FALSE(pid.firstRun);
    TEST_ASSERT_EQUAL_INT16(460, pid.previousPV);

    // PV +0.2 C: P 105, D -5000*2*1000/2500/1000 = -4, integral 45, I 2
    TEST_ASSERT_EQUAL_INT16(103, FixedPointPIDStep::step(pid, limits, 470, 462, 13220, 45, 5000, 2500));
    TEST_ASSERT_EQUAL_INT32(45, pid.integral);

    // PV +0.2 C with Kd 1.0: D -800/1000 truncates to 0; P 79, integral 60, I 2
    TEST_ASSERT_EQUAL_INT16(81, FixedPointPIDStep::step(pid, limits, 470, 464, 13220, 45, 1000, 2500));
    TEST_ASSERT_EQUAL_INT32(60, pid.integral);
}

void test_pid_step_derivative_skipped_after_reset() {
    const FixedPointPIDStep::Limits limits = boilerPidLimits();
    FixedPointPIDStep::State pid = FixedPointPIDStep::resetState();
    // previousPV is 0 after a reset: a derivative here would be huge
    TEST_ASSERT_EQUAL_INT16(0, FixedPointPIDStep::step(pid, limits, 470, 460, 0, 0, 50000, 2500));
    // PV +1.0 C in 2.5 s: D = -50000*10*1000/2500/1000 = -200
    TEST_ASSERT_EQUAL_INT16(-200, FixedPointPIDStep::step(pid, limits, 470, 470, 0, 0, 50000, 2500));
}

void test_pid_step_zero_dt_uses_one_ms() {
    const FixedPointPIDStep::Limits limits = boilerPidLimits();
    FixedPointPIDStep::State pid = FixedPointPIDStep::resetState();
    // integral 1000 * 1 ms / 1000 = 1, I = 1000 * 1 / 1000 = 1
    TEST_ASSERT_EQUAL_INT16(1, FixedPointPIDStep::step(pid, limits, 600, -400, 0, 1000, 1000, 0));
    TEST_ASSERT_EQUAL_INT32(1, pid.integral);
    // D over 1 ms instead of a division by zero, clamped to the output limit
    TEST_ASSERT_EQUAL_INT16(-500, FixedPointPIDStep::step(pid, limits, 600, -398, 0, 1000, 1000, 0));
}

void test_pid_integral_stops_winding_at_output_limit() {
    const FixedPointPIDStep::Limits limits = boilerPidLimits();
    FixedPointPIDStep::State pid = FixedPointPIDStep::resetState();
    // Kp 1, Ki 1, boiler 10 C below target
    TEST_ASSERT_EQUAL_INT16(350, FixedPointPIDStep::step(pid, limits, 570, 470, 1000, 1000, 0, 2500));
    TEST_ASSERT_EQUAL_INT32(250, pid.integral);
    TEST_ASSERT_EQUAL_INT16(500, FixedPointPIDStep::step(pid, limits, 570, 470, 1000, 1000, 0, 2500));
    TEST_ASSERT_EQUAL_INT32(500, pid.integral);
    // Saturated at +500 (100 % power): the integral must not keep growing
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_EQUAL_INT16(500, FixedPointPIDStep::step(pid, limits, 570, 470, 1000, 1000, 0, 2500));
    }
    TEST_ASSERT_EQUAL_INT32(500, pid.integral);
    // Above target the output leaves saturation on the first cycle
    TEST_ASSERT_EQUAL_INT16(465, FixedPointPIDStep::step(pid, limits, 470, 480, 1000, 1000, 0, 2500));
    TEST_ASSERT_EQUAL_INT32(475, pid.integral);
}

void test_pid_integral_stops_winding_at_negative_output_limit() {
    const FixedPointPIDStep::Limits limits = boilerPidLimits();
    FixedPointPIDStep::State pid = FixedPointPIDStep::resetState();
    // Kp 1, Ki 1, boiler 10 C above target
    TEST_ASSERT_EQUAL_INT16(-350, FixedPointPIDStep::step(pid, limits, 470, 570, 1000, 1000, 0, 2500));
    TEST_ASSERT_EQUAL_INT32(-250, pid.integral);
    TEST_ASSERT_EQUAL_INT16(-500, FixedPointPIDStep::step(pid, limits, 470, 570, 1000, 1000, 0, 2500));
    TEST_ASSERT_EQUAL_INT32(-500, pid.integral);
    // Saturated at -500 (0 % power), the limit itself counts: the integral must not keep falling
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_EQUAL_INT16(-500, FixedPointPIDStep::step(pid, limits, 470, 570, 1000, 1000, 0, 2500));
    }
    TEST_ASSERT_EQUAL_INT32(-500, pid.integral);
    // Below target the output leaves saturation on the first cycle
    TEST_ASSERT_EQUAL_INT16(-465, FixedPointPIDStep::step(pid, limits, 470, 460, 1000, 1000, 0, 2500));
    TEST_ASSERT_EQUAL_INT32(-475, pid.integral);
}

void test_pid_integral_unwinds_while_saturated_against_error() {
    const FixedPointPIDStep::Limits limits = boilerPidLimits();
    FixedPointPIDStep::State pid = FixedPointPIDStep::resetState();
    // Kp 1, Ki 1, boiler 30 C below target: integral 750, then held while saturated high
    TEST_ASSERT_EQUAL_INT16(500, FixedPointPIDStep::step(pid, limits, 470, 170, 1000, 1000, 0, 2500));
    TEST_ASSERT_EQUAL_INT16(500, FixedPointPIDStep::step(pid, limits, 470, 170, 1000, 1000, 0, 2500));
    TEST_ASSERT_EQUAL_INT32(750, pid.integral);
    // 0.5 C above target: P -5 + I 750 is still saturated high, but the error pushes down,
    // so the integral unwinds by -5*2500/1000 = -12 per cycle
    TEST_ASSERT_EQUAL_INT16(500, FixedPointPIDStep::step(pid, limits, 470, 475, 1000, 1000, 0, 2500));
    TEST_ASSERT_EQUAL_INT32(738, pid.integral);
    for (int i = 0; i < 10; i++) {
        FixedPointPIDStep::step(pid, limits, 470, 475, 1000, 1000, 0, 2500);
    }
    TEST_ASSERT_EQUAL_INT32(618, pid.integral);

    // Mirror: saturated low with the boiler 0.5 C below target unwinds upwards
    pid = FixedPointPIDStep::resetState();
    TEST_ASSERT_EQUAL_INT16(-500, FixedPointPIDStep::step(pid, limits, 470, 770, 1000, 1000, 0, 2500));
    TEST_ASSERT_EQUAL_INT16(-500, FixedPointPIDStep::step(pid, limits, 470, 770, 1000, 1000, 0, 2500));
    TEST_ASSERT_EQUAL_INT32(-750, pid.integral);
    TEST_ASSERT_EQUAL_INT16(-500, FixedPointPIDStep::step(pid, limits, 470, 465, 1000, 1000, 0, 2500));
    TEST_ASSERT_EQUAL_INT32(-738, pid.integral);
}

void test_pid_step_output_clamped_before_narrowing() {
    const FixedPointPIDStep::Limits limits = boilerPidLimits();
    // Kp 100, error 100.0 C: P = 100000 tenths. Narrowing to int16 before the clamp
    // wraps it to -31072 and the sign flips (2026-09-13 int16 wrap bug).
    FixedPointPIDStep::State pid = FixedPointPIDStep::resetState();
    TEST_ASSERT_EQUAL_INT16(500, FixedPointPIDStep::step(pid, limits, 1000, 0, 100000, 0, 0, 2500));
    pid = FixedPointPIDStep::resetState();
    TEST_ASSERT_EQUAL_INT16(-500, FixedPointPIDStep::step(pid, limits, 0, 1000, 100000, 0, 0, 2500));
}

void test_pid_step_derivative_truncates_toward_zero() {
    const FixedPointPIDStep::Limits limits = boilerPidLimits();
    FixedPointPIDStep::State pid = FixedPointPIDStep::resetState();
    TEST_ASSERT_EQUAL_INT16(0, FixedPointPIDStep::step(pid, limits, 470, 460, 0, 0, 1000, 3));
    // Kd 1.0, PV +0.1 C in 3 ms: D = -1000*1*1000/3 = -333333, /1000 = -333 (not -334)
    TEST_ASSERT_EQUAL_INT16(-333, FixedPointPIDStep::step(pid, limits, 470, 461, 0, 0, 1000, 3));
    // PV -0.1 C: +333
    TEST_ASSERT_EQUAL_INT16(333, FixedPointPIDStep::step(pid, limits, 470, 460, 0, 0, 1000, 3));
    // Kd 0.001: -1*1*1000/3 = -333, /1000 truncates to 0
    TEST_ASSERT_EQUAL_INT16(0, FixedPointPIDStep::step(pid, limits, 470, 461, 0, 0, 1, 3));
}

void test_pid_integral_clamped_to_integral_limits() {
    const FixedPointPIDStep::Limits limits = boilerPidLimits();
    FixedPointPIDStep::State pid = FixedPointPIDStep::resetState();
    // Ki 0.001: I stays below the output limit, so only the integral clamp stops it
    int16_t adjustment = 0;
    for (int i = 0; i < 100; i++) {
        adjustment = FixedPointPIDStep::step(pid, limits, 1000, 0, 0, 1, 0, 2500);
    }
    TEST_ASSERT_EQUAL_INT32(100000, pid.integral);
    TEST_ASSERT_EQUAL_INT16(100, adjustment);
    for (int i = 0; i < 300; i++) {
        adjustment = FixedPointPIDStep::step(pid, limits, 0, 1000, 0, 1, 0, 2500);
    }
    TEST_ASSERT_EQUAL_INT32(-100000, pid.integral);
    TEST_ASSERT_EQUAL_INT16(-100, adjustment);
}

void test_power_map_off_turns_on_only_above_55_percent() {
    const BoilerPowerLevel::Thresholds t = BoilerPowerLevel::defaultThresholds();
    TEST_ASSERT_TRUE(BoilerPowerLevel::fromPidOutput(Level::OFF, 50, t) == Level::OFF);
    TEST_ASSERT_TRUE(BoilerPowerLevel::fromPidOutput(Level::OFF, 55, t) == Level::OFF);
    TEST_ASSERT_TRUE(BoilerPowerLevel::fromPidOutput(Level::OFF, 56, t) == Level::HALF);
    TEST_ASSERT_TRUE(BoilerPowerLevel::fromPidOutput(Level::OFF, 75, t) == Level::HALF);
    TEST_ASSERT_TRUE(BoilerPowerLevel::fromPidOutput(Level::OFF, 76, t) == Level::FULL);
}

void test_power_map_half_and_full_hysteresis() {
    const BoilerPowerLevel::Thresholds t = BoilerPowerLevel::defaultThresholds();
    // HALF holds from 35 to 75 %
    TEST_ASSERT_TRUE(BoilerPowerLevel::fromPidOutput(Level::HALF, 35, t) == Level::HALF);
    TEST_ASSERT_TRUE(BoilerPowerLevel::fromPidOutput(Level::HALF, 34, t) == Level::OFF);
    TEST_ASSERT_TRUE(BoilerPowerLevel::fromPidOutput(Level::HALF, 75, t) == Level::HALF);
    TEST_ASSERT_TRUE(BoilerPowerLevel::fromPidOutput(Level::HALF, 76, t) == Level::FULL);
    // FULL holds down to 65 %
    TEST_ASSERT_TRUE(BoilerPowerLevel::fromPidOutput(Level::FULL, 65, t) == Level::FULL);
    TEST_ASSERT_TRUE(BoilerPowerLevel::fromPidOutput(Level::FULL, 64, t) == Level::HALF);
    TEST_ASSERT_TRUE(BoilerPowerLevel::fromPidOutput(Level::FULL, 34, t) == Level::OFF);
}

void test_bang_bang_bands_and_hold() {
    const BoilerPowerLevel::BangBangBands b = BoilerPowerLevel::defaultBangBangBands();
    TEST_ASSERT_TRUE(BoilerPowerLevel::fromBangBangError(Level::OFF, 30, b) == Level::OFF);
    TEST_ASSERT_TRUE(BoilerPowerLevel::fromBangBangError(Level::OFF, 31, b) == Level::HALF);
    TEST_ASSERT_TRUE(BoilerPowerLevel::fromBangBangError(Level::OFF, 101, b) == Level::FULL);
    TEST_ASSERT_TRUE(BoilerPowerLevel::fromBangBangError(Level::HALF, -50, b) == Level::HALF);
    TEST_ASSERT_TRUE(BoilerPowerLevel::fromBangBangError(Level::HALF, -51, b) == Level::OFF);
    // Only the band -5.0..+3.0 C holds the level; between on and full it is HALF
    TEST_ASSERT_TRUE(BoilerPowerLevel::fromBangBangError(Level::FULL, 30, b) == Level::FULL);
    TEST_ASSERT_TRUE(BoilerPowerLevel::fromBangBangError(Level::FULL, 50, b) == Level::HALF);
}

void test_power_map_exact_switch_points_from_reset() {
    // First cycle after a PID reset at the space heating gains (Kp 13.22, Ki 0.0453),
    // power = 50 + (P + I) / 10 %
    FixedPointPIDStep::State pid;
    // 0.4 C below target: P 52, integral 10, I 0 -> 55 %: stays OFF
    pid = FixedPointPIDStep::resetState();
    TEST_ASSERT_TRUE(cycle(Level::OFF, pid, 470, 466) == Level::OFF);
    // 0.5 C: P 66, integral 12, I 0 -> 56 %: HALF
    pid = FixedPointPIDStep::resetState();
    TEST_ASSERT_TRUE(cycle(Level::OFF, pid, 470, 465) == Level::HALF);
    // 1.9 C: P 251, integral 47, I 2 -> 75 %: HALF
    pid = FixedPointPIDStep::resetState();
    TEST_ASSERT_TRUE(cycle(Level::OFF, pid, 470, 451) == Level::HALF);
    // 2.0 C: P 264, integral 50, I 2 -> 76 %: FULL
    pid = FixedPointPIDStep::resetState();
    TEST_ASSERT_TRUE(cycle(Level::OFF, pid, 470, 450) == Level::FULL);
    // 1.2 C above target: P -158, integral -30, I -1 -> 35 %: HALF holds
    pid = FixedPointPIDStep::resetState();
    TEST_ASSERT_TRUE(cycle(Level::HALF, pid, 470, 482) == Level::HALF);
    // 1.3 C above target: P -171, integral -32, I -1 -> 33 %: OFF
    pid = FixedPointPIDStep::resetState();
    TEST_ASSERT_TRUE(cycle(Level::HALF, pid, 470, 483) == Level::OFF);
}

void test_bang_bang_full_threshold_is_exclusive() {
    const BoilerPowerLevel::BangBangBands b = BoilerPowerLevel::defaultBangBangBands();
    // Exactly 10.0 C below target is HALF from every level, FULL only above it
    TEST_ASSERT_TRUE(BoilerPowerLevel::fromBangBangError(Level::OFF, 100, b) == Level::HALF);
    TEST_ASSERT_TRUE(BoilerPowerLevel::fromBangBangError(Level::HALF, 100, b) == Level::HALF);
    TEST_ASSERT_TRUE(BoilerPowerLevel::fromBangBangError(Level::FULL, 100, b) == Level::HALF);
    TEST_ASSERT_TRUE(BoilerPowerLevel::fromBangBangError(Level::HALF, 101, b) == Level::FULL);
}

void test_boiler_pid_off_above_target_stays_off_through_band() {
    FixedPointPIDStep::State pid = FixedPointPIDStep::resetState();
    // Burner at HALF, boiler 2.0 C above target: OFF on the first cycle
    Level level = cycle(Level::HALF, pid, 470, 490);
    TEST_ASSERT_TRUE(level == Level::OFF);
    // Cooling 0.1 C per cycle to 0.3 C below target: 55 % is not crossed, stays OFF
    for (int16_t boiler = 489; boiler >= 467; boiler--) {
        level = cycle(level, pid, 470, boiler);
        TEST_ASSERT_TRUE(level == Level::OFF);
    }
}

void test_boiler_pid_holds_half_near_target() {
    FixedPointPIDStep::State pid = FixedPointPIDStep::resetState();
    Level level = Level::HALF;
    for (int i = 0; i < 200; i++) {
        level = cycle(level, pid, 470, (i % 2) ? 468 : 472);
        TEST_ASSERT_TRUE(level == Level::HALF);
    }
}

void test_boiler_pid_cold_start_full_then_half_before_target() {
    FixedPointPIDStep::State pid = FixedPointPIDStep::resetState();
    Level level = cycle(Level::OFF, pid, 470, 400);
    TEST_ASSERT_TRUE(level == Level::FULL);
    bool reachedHalf = false;
    // Heating 0.2 C per cycle: never OFF below target, back to HALF before reaching it
    for (int16_t boiler = 402; boiler < 470; boiler = static_cast<int16_t>(boiler + 2)) {
        level = cycle(level, pid, 470, boiler);
        TEST_ASSERT_TRUE(level != Level::OFF);
        if (level == Level::HALF) {
            reachedHalf = true;
        }
    }
    TEST_ASSERT_TRUE(reachedHalf);
}
