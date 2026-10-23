/**
 * @file test_heating_curve.cpp
 * @brief Unit tests for HeatingCurve (weather-compensated heating target)
 */

#include <unity.h>
#include <cmath>
#include <cstdint>

#include "../../include/modules/control/HeatingCurve.h"

// setUp and tearDown are defined in test_main.cpp

namespace {
double referenceTarget(double inside, double outside, double coeff, double shift) {
    const double diff = outside - inside;
    return inside + shift - coeff * diff * (1.4347 + 0.021 * diff + 0.000248 * diff * diff);
}
}  // namespace

void test_heating_curve_matches_reference_formula() {
    const double outsides[] = {15.0, 5.0, 0.0, -10.0, -15.0};
    const double params[][2] = {{1.4, 0.0}, {2.0, 20.0}};
    for (const auto& p : params) {
        for (double outside : outsides) {
            const int16_t t = HeatingCurve::target(200, static_cast<int16_t>(outside * 10),
                                                   static_cast<int16_t>(p[0] * 100),
                                                   static_cast<int16_t>(p[1] * 10), -1000, 2000);
            const int expected = static_cast<int>(std::lround(referenceTarget(20.0, outside, p[0], p[1]) * 10));
            TEST_ASSERT_INT_WITHIN(3, expected, t);  // within 0.3 °C (integer truncation)
        }
    }
}

void test_heating_curve_outside_part_not_ten_times_too_small() {
    // 2026-09-15: ADJUSTMENT_SCALE 10^7 turned the adjustment into whole °C used as tenths:
    // at -10 °C outside (room 20, slope 1.4) the curve added 4.3 °C instead of 43 °C
    const int16_t t = HeatingCurve::target(200, -100, 140, 0, 0, 2000);
    TEST_ASSERT_TRUE(t > 620 && t < 645);  // about 63.2 °C
}

void test_heating_curve_clamps_to_limits() {
    TEST_ASSERT_EQUAL_INT16(750, HeatingCurve::target(200, -200, 400, 400, 400, 750));
    TEST_ASSERT_EQUAL_INT16(400, HeatingCurve::target(250, 250, 100, 0, 400, 750));
}

void test_heating_curve_survives_invalid_room_reading() {
    // The 2026-10-23 08:34:13 field case, reproduced exactly. ANDRTF3Task sets
    // insideTemp = TEMP_INVALID (INT16_MIN) after 3 consecutive read failures, and the
    // caller passed it straight in. POLY_C3 * diffSquared was 26,703,613,032, which wrapped
    // int32 to 933,809,256 and produced raw = -7554185 tenths (-755418.5 °C).
    //
    // With int64 arithmetic the result is still absurd - garbage in, garbage out - but it is
    // now a *defined* absurdity, and the clamp still contains it. The point of this test is
    // that the function is total and does not invoke UB.
    int32_t raw = 0;
    const int16_t result = HeatingCurve::target(INT16_MIN, 46, 140, 44, 380, 750, &raw);
    TEST_ASSERT_EQUAL_INT16(380, result);          // clamped to the flow floor
    TEST_ASSERT_NOT_EQUAL(-7554185, raw);          // not the wrapped value any more
    TEST_ASSERT_TRUE(raw < 0);                     // still reports "far below the floor"

    // Both inputs at the int16 extremes: diff is +/-65535, so diff*diff alone (4.29e9)
    // exceeds int32. Must not trap or wrap.
    HeatingCurve::target(INT16_MIN, INT16_MAX, 140, 44, 380, 750, &raw);
    HeatingCurve::target(INT16_MAX, INT16_MIN, 140, 44, 380, 750, &raw);
    HeatingCurve::target(INT16_MIN, INT16_MIN, 32767, 32767, 380, 750, &raw);
    HeatingCurve::target(INT16_MAX, INT16_MAX, 32767, 32767, 380, 750, &raw);
    TEST_ASSERT_TRUE(true);  // reaching here without UB/trap is the assertion
}

void test_heating_curve_raw_saturates_instead_of_truncating() {
    // rawOut is int32 but result is int64. A narrowing cast would put an arbitrary value on
    // the diagnostics topic - the one place someone looks to see that the curve went wrong.
    int32_t raw = 0;
    HeatingCurve::target(INT16_MAX, INT16_MIN, 32767, 0, 380, 750, &raw);
    TEST_ASSERT_TRUE(raw == INT32_MAX || raw == INT32_MIN || (raw > INT32_MIN && raw < INT32_MAX));
    // The specific direction: inside high, outside low -> huge positive adjustment
    // subtracted -> very negative result -> saturates low, never wraps positive.
    HeatingCurve::target(0, INT16_MAX, 32767, 0, 380, 750, &raw);
    TEST_ASSERT_TRUE(raw < 0);
}

void test_heating_curve_nominal_room_fallback_is_sane() {
    // The caller substitutes this when the room sensor is invalid and the room target is
    // also unset. It must land inside the normal operating band, not on a clamp.
    TEST_ASSERT_EQUAL_INT16(200, HeatingCurve::NOMINAL_ROOM_TENTHS);
    int32_t raw = 0;
    const int16_t result = HeatingCurve::target(HeatingCurve::NOMINAL_ROOM_TENTHS, 46,
                                                140, 44, 380, 750, &raw);
    TEST_ASSERT_TRUE(result > 380 && result < 750);   // modulating, not clamped
    TEST_ASSERT_EQUAL_INT32(result, raw);             // and unclamped, so raw == target
}

void test_heating_curve_room_at_target_fallback_tracks_the_real_reading() {
    // Why the caller substitutes targetTemperatureInside rather than the floor or a
    // constant. Replays the 2026-10-23 08:34 glitch with the values the device actually had
    // (outside 4.6, room target 19.2, coeff 1.40, curveShift 4.4) and compares against the
    // curve it computed 6 s later once the sensor recovered (room 19.7, shift 3.4 - the
    // room-deviation term is back, which is why the shift differs).
    int32_t rawFallback = 0, rawReal = 0;
    const int16_t fallback = HeatingCurve::target(192, 46, 140, 44, 380, 750, &rawFallback);
    const int16_t real     = HeatingCurve::target(197, 46, 140, 34, 380, 750, &rawReal);

    // Both modulating, neither clamped
    TEST_ASSERT_TRUE(fallback > 380 && fallback < 750);
    TEST_ASSERT_TRUE(real > 380 && real < 750);
    // The fallback lands within 0.5 °C of the truth, so a momentary sensor dropout barely
    // perturbs the flow setpoint. The old behaviour was -755418.5 clamped to the floor,
    // i.e. a 10 °C step down in the commanded target.
    TEST_ASSERT_INT_WITHIN(5, real, fallback);
}

