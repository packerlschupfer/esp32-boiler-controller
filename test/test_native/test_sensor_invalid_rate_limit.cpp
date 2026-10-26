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

void test_invalid_count_floor_under_uniform_polling() {
    // The predicate's FLOOR: with a caller asking faster than the rate limit, three steps can
    // never land sooner than first-sample + 2 x TICK = 5 s.
    //
    // This test used to be named "...in_about_five_seconds" and its message claimed 3 steps
    // span about 5 s in the real system. They do not - see the schedule test below. 5 s is a
    // lower bound that only a UNIFORM caller achieves, and the MB8ART is not uniform. Renamed
    // and re-scoped 2026-10-26; the old name was the kind of claim that makes a passing test
    // read as evidence for something it never measured.
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
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(5000, shutdownAtMs,
                                     "uniform polling floor: 3 steps cannot beat 2 x TICK");
}

void test_invalid_count_on_the_real_mb8art_schedule_is_eight_to_ten_seconds() {
    // THE REAL NUMBER. The MB8ART is read on ModbusCoordinator ticks {2,5} of a 10-tick 500 ms
    // cycle, so samples arrive 1.5 s then 3.5 s apart - a 2.5 s average, never a 2.5 s period.
    // A sample 1.5 s after a counted one fails the >= 2500 ms test and is skipped, so three
    // steps take longer than the 5 s that three samples would suggest.
    //
    // Driven from the actual tick schedule rather than a synthetic uniform caller, because the
    // uniform version is what hid this for a day.
    const int startTicks[2] = {2, 5};
    for (int si = 0; si < 2; ++si) {
        const int startTick = startTicks[si];
        bool counting = false;
        uint32_t last = 0;
        int steps = 0;
        uint32_t shutdownAtMs = 0;
        bool seen = false;
        // 60 s of ticks; MB8ART reads on tick 2 and tick 5 of every 10.
        for (uint32_t t = 0; t <= 60000; t += 500) {
            const int tick = static_cast<int>((t / 500) % 10);
            if (tick != 2 && tick != 5) continue;        // no sample on this tick
            if (!seen) {
                if (tick != startTick) continue;          // align the first failure
                seen = true;
            }
            if (TemperatureSensorFallback::shouldCountInvalid(counting, t, last, TICK)) {
                counting = true; last = t; steps++;
                if (steps == TemperatureSensorFallback::INVALID_COUNT_TO_SHUTDOWN && shutdownAtMs == 0) {
                    shutdownAtMs = t;
                    break;
                }
            }
        }
        // From tick 5: steps at +0, +3.5, +8.5 (the +5.0 sample is skipped). From tick 2:
        // steps at +0, +5.0, +10.0 (the +1.5 sample is skipped).
        const uint32_t span = shutdownAtMs - (startTick == 5 ? 2500u : 1000u);
        TEST_ASSERT_TRUE_MESSAGE(span >= 8500 && span <= 10000,
                                 "3 steps span 8.5-10 s on the real schedule, not 5 s");
    }
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

void test_invalid_count_boiler_output_escalates_sooner_than_the_shared_count() {
    // The debounce is SPLIT per sensor class (2026-10-26). The shared count rides out the
    // ANDRTF3's known room dropouts; the boiler OUTPUT - the reading that blinds this layer's
    // overheat protection and sets Error::SENSOR_FAILURE - gets a shorter one.
    //
    // The thresholds must stay ordered, and the boiler one must still reject a SINGLE bad read,
    // which is what the 2026-10-23 change from 1 to 3 was for after one failed check stopped a
    // burn mid-cycle. Both are pinned here because "shorter" and "not instant" are the two
    // properties that make the split safe rather than a revert.
    TEST_ASSERT_TRUE_MESSAGE(
        TemperatureSensorFallback::BOILER_OUTPUT_COUNT_TO_SHUTDOWN <
        TemperatureSensorFallback::INVALID_COUNT_TO_SHUTDOWN,
        "the boiler output must escalate sooner than the shared required-sensor count");
    TEST_ASSERT_TRUE_MESSAGE(
        TemperatureSensorFallback::BOILER_OUTPUT_COUNT_TO_SHUTDOWN >= 2,
        "a single bad boiler read must not shut down - that was the 2026-10-23 regression");

    // On the real MB8ART schedule (ticks {2,5} of ten) two steps land 3.5-5 s apart against the
    // 8.5-10 s of three, so the split roughly halves the blind window.
    const int startTicks[2] = {2, 5};
    for (int si = 0; si < 2; ++si) {
        bool counting = false;
        uint32_t last = 0;
        int steps = 0;
        uint32_t atMs = 0;
        bool seen = false;
        for (uint32_t t = 0; t <= 60000; t += 500) {
            const int tick = static_cast<int>((t / 500) % 10);
            if (tick != 2 && tick != 5) continue;
            if (!seen) { if (tick != startTicks[si]) continue; seen = true; }
            if (TemperatureSensorFallback::shouldCountInvalid(counting, t, last, TICK)) {
                counting = true; last = t; steps++;
                if (steps == TemperatureSensorFallback::BOILER_OUTPUT_COUNT_TO_SHUTDOWN) { atMs = t; break; }
            }
        }
        const uint32_t span = atMs - (startTicks[si] == 5 ? 2500u : 1000u);
        TEST_ASSERT_TRUE_MESSAGE(span >= 3500 && span <= 5000,
                                 "2 steps span 3.5-5 s on the real schedule");
    }
}
