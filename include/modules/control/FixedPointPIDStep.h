// include/modules/control/FixedPointPIDStep.h
#ifndef FIXED_POINT_PID_STEP_H
#define FIXED_POINT_PID_STEP_H

#include <cstdint>
#include "modules/control/PIDGainFixedPoint.h"

/**
 * @brief One step of the fixed-point PID (header-only, native-testable).
 *
 * PIDControlModuleFixedPoint::calculatePIDAdjustment() (mutex, logging, timestamps)
 * and BoilerTempController::predictHeatDemand() both run this step, so the native
 * tests exercise the firmware arithmetic instead of a hand-written copy of the
 * P term (review 2026-09-14 pid-8).
 *
 * Gains are scaled by PIDGainFixedPoint::SCALE (1000); temperatures and the output
 * are tenths of °C. All divisions truncate toward zero.
 * - P = Kp * error
 * - D = -Kd * (PV - previous PV) / dt, on the process variable to avoid derivative
 *   kick on setpoint changes; skipped on the first step after a reset
 * - I = Ki * integral. The integral (error * dt in ms / SCALE) only accumulates while
 *   the output is not saturated in the error's direction (anti-windup) and is
 *   clamped to the integral limits
 * - the sum is clamped to the output limits in the wide type, then narrowed
 */
namespace FixedPointPIDStep {

    struct State {
        int32_t integral;    // accumulated error * dt (scaled)
        int16_t previousPV;  // process variable of the previous step
        bool firstRun;       // no previous PV yet: skip the derivative
    };

    struct Limits {
        int32_t integralMin;
        int32_t integralMax;
        int16_t outputMin;   // tenths of °C
        int16_t outputMax;
    };

    inline State resetState() {
        State state = {0, 0, true};
        return state;
    }

    inline int32_t clampIntegral(int32_t value, int32_t minValue, int32_t maxValue) {
        if (value < minValue) return minValue;
        if (value > maxValue) return maxValue;
        return value;
    }

    /**
     * @brief Advance the PID by one step
     * @return Adjustment in tenths of °C, within the output limits
     */
    inline int16_t step(State& state, const Limits& limits,
                        int16_t setPoint, int16_t currentTemp,
                        int32_t Kp, int32_t Ki, int32_t Kd, uint32_t dtMs) {
        const int32_t SCALE = PIDGainFixedPoint::SCALE;

        if (dtMs == 0) {
            dtMs = 1;
        }

        const int16_t error = static_cast<int16_t>(setPoint - currentTemp);

        // P = Kp * error
        const int64_t pRaw = static_cast<int64_t>(Kp) * error;
        const int32_t P = static_cast<int32_t>(pRaw / SCALE);

        // D = -Kd * pvDelta / dt (negative to oppose the PV change)
        int32_t D = 0;
        if (state.firstRun) {
            state.firstRun = false;
        } else {
            const int16_t pvDelta = static_cast<int16_t>(currentTemp - state.previousPV);
            const int64_t dRaw = static_cast<int64_t>(Kd) * (-pvDelta) * SCALE / dtMs;
            D = static_cast<int32_t>(dRaw / SCALE);
        }
        state.previousPV = currentTemp;

        // I from the existing integral
        int64_t iRaw = static_cast<int64_t>(Ki) * state.integral;
        int32_t I = static_cast<int32_t>(iRaw / SCALE);

        // Anti-windup: accumulate only if the tentative output is not saturated in the
        // direction the error pushes. Clamp in the wide type: narrowing first wraps the sign.
        const int32_t tentativeOutput = P + I + D;
        const int16_t tentativeAdjustment =
            PIDGainFixedPoint::clampToAdjustment(tentativeOutput, limits.outputMin, limits.outputMax);
        const bool wouldSaturateHigh = (tentativeAdjustment >= limits.outputMax) && (error > 0);
        const bool wouldSaturateLow = (tentativeAdjustment <= limits.outputMin) && (error < 0);

        if (!wouldSaturateHigh && !wouldSaturateLow) {
            const int64_t integralDelta = static_cast<int64_t>(error) * dtMs;
            state.integral += static_cast<int32_t>(integralDelta / SCALE);
            state.integral = clampIntegral(state.integral, limits.integralMin, limits.integralMax);

            iRaw = static_cast<int64_t>(Ki) * state.integral;
            I = static_cast<int32_t>(iRaw / SCALE);
        }

        const int32_t output = P + I + D;
        return PIDGainFixedPoint::clampToAdjustment(output, limits.outputMin, limits.outputMax);
    }

} // namespace FixedPointPIDStep

#endif // FIXED_POINT_PID_STEP_H
