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

    /**
     * @brief What the curve is anchored on, and the shift that goes with it.
     *
     * THE DEFECT THIS FIXES (found 2026-10-26, present since the room shift was added)
     *
     * The room reading used to be passed as `insideTenths` AND used for the
     * Parallelverschiebung, so it fed the curve twice and the two terms fought - with the
     * curve's own term winning. Lowering the room by 1 K lowers the base by 1 K and shrinks
     * `diff = outside - inside`, which shrinks the weather adjustment; together those beat
     * the `+roomCurveShiftFactor` the shift adds. Measured sensitivity was **+0.70 K/K at
     * 12 C outside down to +0.20 K/K at 0 C** - the WRONG SIGN. A house 3 K cold at 6 C
     * outside was commanded 44.9 C flow, LESS than the 46.3 it would get if it were already
     * at temperature, where the documented intent is 52.3.
     *
     * A Heizkurve is parameterised on the DESIGN indoor temperature, not the live room: the
     * live room belongs only in the shift. Anchoring on the target makes the sensitivity
     * exactly `-roomCurveShiftFactor` at every outside temperature, which is what the
     * comment in HeatingControlModule always claimed.
     *
     * Tuning the factor cannot fix it: the value needed merely to reach ZERO sensitivity
     * varies with the weather (2.71 at 12 C, 2.11 at 0 C), so no single number is right.
     *
     * Why the 2026-10-18 validation against 7 live targets missed it: the two formulations
     * are IDENTICAL when room == target, and near-identical close to it. The error only
     * appears when the room deviates - exactly when the term is supposed to act.
     *
     * The anchor and the deviation are returned together, and deliberately share one
     * condition: anchoring on a target while not applying its deviation (or the reverse)
     * is a third formulation nobody wants.
     */
    struct Anchoring {
        int16_t inside;  ///< what to pass as the curve's insideTenths
        int16_t shift;   ///< base shift plus the room-deviation term, tenths
    };

    /**
     * @param baseShift        heating/curveShift, tenths
     * @param roomTarget       heating/targetTemp, tenths; <= 0 means "none configured"
     * @param effectiveInside  the room value in use (measured, held, or a substitute)
     * @param shiftFactorX10   heating/roomCurveShiftFactor x10 (20 = 2.0)
     * @param weatherMode      heating/weatherControl
     * @param roomIsMeasurement the room value is a real reading, fresh or held
     *
     * Integer-only: the factor arrives x10 rather than as a float so this stays exactly
     * reproducible on the host. The firmware's float parameter is scaled at the call site.
     */
    inline Anchoring anchoring(int16_t baseShift, int16_t roomTarget, int16_t effectiveInside,
                               int16_t shiftFactorX10, bool weatherMode,
                               bool roomIsMeasurement) {
        // No target to anchor on, no weather curve, or no real reading to deviate from:
        // behave exactly as before - anchor on whatever room value is in use and apply no
        // deviation. With a TARGET substitution effectiveInside already IS the target, so
        // the two agree anyway; with NOMINAL there is no target by definition.
        if (!weatherMode || roomIsMeasurement == false || roomTarget <= 0) {
            return Anchoring{effectiveInside, baseShift};
        }

        // Deviation is positive when the room is COLD, and raises the shift.
        const int32_t deviation = static_cast<int32_t>(roomTarget) - effectiveInside;
        const int32_t adjust = (deviation * shiftFactorX10) / 10;
        int32_t shift = static_cast<int32_t>(baseShift) + adjust;

        // The curve clamps its own output, but the shift feeds the polynomial through
        // `inside + shift`, so keep it inside int16 here rather than relying on that.
        if (shift > INT16_MAX) { shift = INT16_MAX; }
        if (shift < INT16_MIN) { shift = INT16_MIN; }

        return Anchoring{roomTarget, static_cast<int16_t>(shift)};
    }

} // namespace HeatingCurve

#endif // HEATING_CURVE_H
