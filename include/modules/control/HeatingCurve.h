// include/modules/control/HeatingCurve.h
#ifndef HEATING_CURVE_H
#define HEATING_CURVE_H

#include <cstdint>
#include <climits>

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

    /**
     * @param rawOut  optional: the PRE-CLAMP result in tenths. Added 2026-10-18 for the
     *                boiler/status/heating/control diagnostics topic, which has to show how
     *                far below the h/sLo floor the curve actually is - in mild weather the
     *                clamped output is a flat 40.0 and tells you nothing. Defaults to
     *                nullptr so every existing caller is unaffected.
     */
    /**
     * @brief Nominal room temperature, tenths. Substitute for an unreadable room sensor.
     *
     * Lives here rather than in the caller so the fallback is covered by this header's
     * native tests. Only used when heating/targetTemperatureInside is also unusable.
     */
    constexpr int16_t NOMINAL_ROOM_TENTHS = 200;  // 20.0 °C

    inline int16_t target(int16_t insideTenths, int16_t outsideTenths, int16_t coeffX100,
                          int16_t shiftTenths, int16_t lowerLimit, int16_t upperLimit,
                          int32_t* rawOut = nullptr) {
        const int32_t diff = static_cast<int32_t>(outsideTenths) - insideTenths;  // tenths

        // int64 from diffSquared onward. Both of these overflowed int32 and the second one
        // did so in the field: 2026-10-23 08:34:13, the room sensor was marked invalid
        // (ANDRTF3Task sets insideTemp = TEMP_INVALID = INT16_MIN after 3 consecutive read
        // failures) and the caller passed -32768 straight in. diff became 32814 tenths,
        // diffSquared 107675859, and POLY_C3 * diffSquared = 26,703,613,032 wrapped to
        // 933,809,256 - signed overflow, so UB, not merely a wrong number. The published
        // raw target was -755418.5 °C. Reproduced exactly before fixing.
        //
        // The caller no longer passes an invalid reading (HeatingControlModule), but this
        // function must stay total: with int16 inputs diff reaches +/-65535, so diff*diff
        // alone is up to 4.29e9 and cannot be held in int32 either. Division truncates
        // toward zero identically in both widths, so results for in-range inputs are
        // unchanged - the existing curve tests pin that.
        const int64_t diffSquared = (static_cast<int64_t>(diff) * diff) / 10;     // °C^2 * 10
        int64_t polynomial = POLY_C1;                                             // x10000
        polynomial += (static_cast<int64_t>(POLY_C2) * diff) / 10;
        polynomial += (static_cast<int64_t>(POLY_C3) * diffSquared) / 1000;

        const int64_t adjustment =
            static_cast<int64_t>(coeffX100) * diff * polynomial / ADJUSTMENT_SCALE;  // tenths

        int64_t result = static_cast<int64_t>(insideTenths) + shiftTenths - adjustment;
        if (rawOut != nullptr) {
            // Saturate rather than truncate: a narrowing cast of an out-of-range result
            // would put an arbitrary value on the diagnostics topic, which is the one place
            // someone looks to find out that the curve went wrong.
            if (result > INT32_MAX) {
                *rawOut = INT32_MAX;
            } else if (result < INT32_MIN) {
                *rawOut = INT32_MIN;
            } else {
                *rawOut = static_cast<int32_t>(result);
            }
        }
        if (result < lowerLimit) {
            result = lowerLimit;
        } else if (result > upperLimit) {
            result = upperLimit;
        }
        return static_cast<int16_t>(result);
    }

} // namespace HeatingCurve

#endif // HEATING_CURVE_H
