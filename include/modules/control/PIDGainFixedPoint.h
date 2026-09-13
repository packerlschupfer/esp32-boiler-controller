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

} // namespace PIDGainFixedPoint

#endif // PID_GAIN_FIXED_POINT_H
