// include/modules/control/HeatingCurve.h
#ifndef HEATING_CURVE_H
#define HEATING_CURVE_H

#include <cstdint>

/**
 * @brief Weather-compensated heating curve (header-only, native-testable).
 *
 *   target = inside + shift - coeff * diff * (1.4347 + 0.021 * diff + 0.000248 * diff^2)
 *   diff   = outside - inside (°C)
 *
 * All temperatures in tenths of °C, coeff scaled by 100 (140 = 1.4).
 *
 * coeff (x100) * diff (tenths, x10) * polynomial (x10000) is the adjustment in tenths of
 * °C scaled by 10^6. It was divided by 10^7, which yields whole °C that were then used as
 * tenths: the outside-temperature part of the curve was 10x too small, so the heating
 * target stayed at the space-heating minimum regardless of the weather (2026-09-15).
 */
namespace HeatingCurve {

    constexpr int32_t POLY_C1 = 14347;  // 1.4347   * 10000
    constexpr int32_t POLY_C2 = 210;    // 0.021    * 10000 (per °C)
    constexpr int32_t POLY_C3 = 248;    // 0.000248 * 1000000 (per °C^2)
    constexpr int64_t ADJUSTMENT_SCALE = 1000000;

    inline int16_t target(int16_t insideTenths, int16_t outsideTenths, int16_t coeffX100,
                          int16_t shiftTenths, int16_t lowerLimit, int16_t upperLimit) {
        const int32_t diff = static_cast<int32_t>(outsideTenths) - insideTenths;  // tenths
        const int32_t diffSquared = (diff * diff) / 10;                           // °C^2 * 10
        int32_t polynomial = POLY_C1;                                             // x10000
        polynomial += (POLY_C2 * diff) / 10;
        polynomial += (POLY_C3 * diffSquared) / 1000;

        const int64_t adjustment =
            static_cast<int64_t>(coeffX100) * diff * polynomial / ADJUSTMENT_SCALE;  // tenths

        int64_t result = static_cast<int64_t>(insideTenths) + shiftTenths - adjustment;
        if (result < lowerLimit) {
            result = lowerLimit;
        } else if (result > upperLimit) {
            result = upperLimit;
        }
        return static_cast<int16_t>(result);
    }

} // namespace HeatingCurve

#endif // HEATING_CURVE_H
