/**
 * @file test_space_heating_policy.cpp
 * @brief Unit tests for SpaceHeatingPolicy (weather mode outside threshold hysteresis)
 */

#include <unity.h>
#include <cstdint>

#include "../../include/modules/control/SpaceHeatingPolicy.h"

// setUp and tearDown are defined in test_main.cpp

using namespace SpaceHeatingPolicy;

void test_space_heating_outside_threshold_start_and_stop_limits() {
    // Threshold 15.0 °C: start below 15.0, keep running until 16.0
    TEST_ASSERT_TRUE(outsideColdForHeating(149, 150, false));
    TEST_ASSERT_FALSE(outsideColdForHeating(150, 150, false));
    TEST_ASSERT_TRUE(outsideColdForHeating(150, 150, true));
    TEST_ASSERT_TRUE(outsideColdForHeating(159, 150, true));
    TEST_ASSERT_FALSE(outsideColdForHeating(160, 150, true));
}

void test_space_heating_outside_noise_does_not_toggle() {
    // 2026-09-15: without hysteresis, readings around the threshold switched heating on and
    // off every 5 s cycle. Noise of +/-0.2 °C around 15.0 °C must give a single start.
    const int16_t readings[] = {151, 149, 151, 150, 148, 152, 149, 151, 150, 152};
    bool heating = false;
    int starts = 0;
    int stops = 0;
    for (int16_t r : readings) {
        const bool next = outsideColdForHeating(r, 150, heating);
        if (next && !heating) starts++;
        if (!next && heating) stops++;
        heating = next;
    }
    TEST_ASSERT_EQUAL_INT(1, starts);
    TEST_ASSERT_EQUAL_INT(0, stops);
    TEST_ASSERT_TRUE(heating);
}
