// include/modules/control/PIDGainFixedPoint.h
#ifndef PID_GAIN_FIXED_POINT_H
#define PID_GAIN_FIXED_POINT_H

#include <cmath>
#include <cstdint>

/**
 * @brief Convert a float PID gain (as stored in SystemSettings / autotune results)
 *        to the fixed-point representation expected by PIDControlModuleFixedPoint.
 *
 * PIDControlModuleFixedPoint::calculatePIDAdjustment() takes PIDValue_t (int32_t)
 * gains scaled by PID_FIXED_POINT_SCALE (1000). Passing the float gains directly
 * compiled silently via implicit float->int32 truncation: Kp 34.206 became 34,
 * i.e. an effective Kp of 0.034 and Ki 0.189 became 0. The boiler PID output
 * stayed at ~50% forever and never commanded OFF.
 *
 * Header-only and FreeRTOS-free so it can be unit tested natively.
 * Negative or non-finite gains map to 0; the result saturates at INT32_MAX.
 */
namespace PIDGainFixedPoint {

    constexpr int32_t SCALE = 1000;  // Must equal SystemConstants::PID::PID_FIXED_POINT_SCALE

    inline int32_t fromFloat(float gain) {
        if (!std::isfinite(gain) || gain <= 0.0f) {
            return 0;
        }
        double scaled = static_cast<double>(gain) * SCALE + 0.5;
        if (scaled >= static_cast<double>(INT32_MAX)) {
            return INT32_MAX;
        }
        return static_cast<int32_t>(scaled);
    }

    /**
     * @brief Clamp a wide PID output to the adjustment range, THEN narrow to int16.
     *
     * The PID sums P+I+D in int32. Casting that to Temperature_t (int16) before
     * clamping wraps large values: -44055 became +21481, which clamped to +100%
     * and commanded the burner FULL while 5.5C above target (2026-09-13 23:22).
     */
    inline int16_t clampToAdjustment(int64_t output, int16_t minValue, int16_t maxValue) {
        if (output < minValue) return minValue;
        if (output > maxValue) return maxValue;
        return static_cast<int16_t>(output);
    }

    /**
     * @brief BoilerTempController maps the PID adjustment to burner power as
     *        50 % + adjustment / 10, clamped to 0..100 %.
     *
     * Power saturates at +/-POWER_ADJUSTMENT_LIMIT, so the boiler PID output limits (and
     * with them anti-windup) must use the same range. With the default +/-1000 the
     * integral kept growing for minutes while power was already 100 %, and the burner
     * held FULL several degrees above target (review 2026-09-14).
     */
    constexpr int16_t POWER_ADJUSTMENT_LIMIT = 500;

    inline uint8_t powerPercentFromAdjustment(int32_t adjustment) {
        int32_t power = 50 + adjustment / 10;
        if (power < 0) power = 0;
        if (power > 100) power = 100;
        return static_cast<uint8_t>(power);
    }

} // namespace PIDGainFixedPoint

#endif // PID_GAIN_FIXED_POINT_H
