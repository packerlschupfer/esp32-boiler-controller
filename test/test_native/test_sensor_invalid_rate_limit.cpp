/**
 * @file test_sensor_invalid_rate_limit.cpp
 * @brief The sensor-fallback invalid counter must step per SAMPLE, not per call (2026-10-25)
 */

#include <unity.h>
#include <cstdint>

#include "../../include/modules/control/TemperatureSensorFallback.h"
#include "../../src/config/SystemConstants.h"

// setUp and tearDown are defined in test_main.cpp

static constexpr uint32_t TICK = SystemConstants::Timing::MB8ART_SENSOR_READ_INTERVAL_MS;  // 2500

void test_invalid_count_steps_once_per_sample_not_once_per_caller() {
    // THE REGRESSION. updateSensorStatus() is reached from canContinueOperation(), which is
    // called from BurnerControlTask (3 sites), WheaterControlTask (4), CentralizedFailsafe and
    // more - and the STARTUP branch calls it twice in one call. Counting per CALL let
    // INVALID_COUNT_TO_SHUTDOWN = 3 be crossed inside a single 2.5 s sample period, which
    // silently undid most of the 2026-10-23 change from 1 -> 3.
    bool counting = false;
    uint32_t last = 0;
    uint32_t now = 100000;
    int steps = 0;

    // First invalid observation counts at once - the sample that revealed it just arrived.
    if (TemperatureSensorFallback::shouldCountInvalid(counting, now, last, TICK)) {
        counting = true; last = now; steps++;
    }
    TEST_ASSERT_EQUAL_INT(1, steps);

    // Eight more callers in the SAME sample period must add nothing.
    for (int i = 0; i < 8; i++) {
        now += 50;
        if (TemperatureSensorFallback::shouldCountInvalid(counting, now, last, TICK)) {
            counting = true; last = now; steps++;
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, steps, "extra callers must not advance the debounce");
}

void test_invalid_count_reaches_shutdown_in_about_five_seconds() {
    // Three steps = first sample + two further periods = ~5 s, which is what the constant's
    // comment now claims. Simulate a caller asking every 500 ms for 6 s.
    bool counting = false;
    uint32_t last = 0;
    int steps = 0;
    uint32_t shutdownAtMs = 0;
    for (uint32_t t = 0; t <= 6000; t += 500) {
        if (TemperatureSensorFallback::shouldCountInvalid(counting, t, last, TICK)) {
            counting = true; last = t; steps++;
            if (steps == TemperatureSensorFallback::INVALID_COUNT_TO_SHUTDOWN && shutdownAtMs == 0) {
                shutdownAtMs = t;
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(5000, shutdownAtMs, "3 steps should span about 5 s");
}

void test_invalid_count_still_escalates_when_the_sensor_is_completely_dead() {
    // Gating on a NEW SAMPLE ARRIVING would have been the obvious implementation and is
    // wrong: a dead MB8ART stops advancing lastBoilerTempUpdateTimestamp, so the counter
    // would never move and the fallback would never escalate - the exact failure this layer
    // exists to catch. The rate limit is on elapsed time, so a frozen sensor still escalates.
    bool counting = false;
    uint32_t last = 0;
    int steps = 0;
    for (uint32_t t = 0; t < 10 * TICK; t += 100) {   // no samples ever arrive
        if (TemperatureSensorFallback::shouldCountInvalid(counting, t, last, TICK)) {
            counting = true; last = t; steps++;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(steps >= TemperatureSensorFallback::INVALID_COUNT_TO_SHUTDOWN,
                             "a dead sensor must still reach SHUTDOWN");
}

void test_invalid_count_rate_limit_survives_the_millis_rollover() {
    // `lastMs + intervalMs` would wrap and stall the counter for 49 days, i.e. stop
    // escalating a dead sensor once every rollover. The uint32_t subtraction does not.
    const uint32_t last = 0xFFFFFF00u;          // 256 ms before the wrap
    TEST_ASSERT_FALSE(TemperatureSensorFallback::shouldCountInvalid(true, last + 100, last, TICK));
    TEST_ASSERT_FALSE(TemperatureSensorFallback::shouldCountInvalid(true, 100u, last, TICK));  // wrapped, 356 ms
    TEST_ASSERT_TRUE(TemperatureSensorFallback::shouldCountInvalid(true, last + TICK, last, TICK));
    TEST_ASSERT_TRUE(TemperatureSensorFallback::shouldCountInvalid(true, 2500u, last, TICK));  // wrapped, ~2.75 s
}

void test_invalid_count_first_observation_is_never_rate_limited() {
    // counting=false means "first invalid since the last valid evaluation" - it must always
    // pass, whatever the clock says, including at millis()==0 during STARTUP. That is why the
    // implementation uses a separate flag rather than a 0 sentinel on the timestamp.
    TEST_ASSERT_TRUE(TemperatureSensorFallback::shouldCountInvalid(false, 0u, 0u, TICK));
    TEST_ASSERT_TRUE(TemperatureSensorFallback::shouldCountInvalid(false, 1u, 0u, TICK));
    TEST_ASSERT_TRUE(TemperatureSensorFallback::shouldCountInvalid(false, 123456u, 123456u, TICK));
    // ...whereas once counting, the same clock values are rate-limited.
    TEST_ASSERT_FALSE(TemperatureSensorFallback::shouldCountInvalid(true, 0u, 0u, TICK));
    TEST_ASSERT_FALSE(TemperatureSensorFallback::shouldCountInvalid(true, 123456u, 123456u, TICK));
}
