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
