/**
 * @file test_emergency_stop_release.cpp
 * @brief Unit tests for EmergencyStopRelease (manual release of a latched emergency stop)
 */

#include <unity.h>
#include <cstring>

#include "../../include/modules/control/EmergencyStopRelease.h"

// setUp and tearDown are defined in test_main.cpp

using namespace EmergencyStopRelease;

namespace {
Conditions allClear() {
    Conditions c;
    c.emergencyActive = true;
    c.temperaturesOk = true;
    c.sensorsOk = true;
    c.systemErrorsClear = true;
    return c;
}
} // namespace

void test_emergency_release_when_causes_cleared() {
    TEST_ASSERT_TRUE(evaluate(allClear()) == Result::RELEASED);
    TEST_ASSERT_EQUAL_STRING("emergency_released", toString(Result::RELEASED));
}

void test_emergency_release_not_active() {
    Conditions c = allClear();
    c.emergencyActive = false;
    TEST_ASSERT_TRUE(evaluate(c) == Result::NOT_ACTIVE);
}

void test_emergency_release_refused_while_hot() {
    // 2026-09-14: a 115 °C trip must not be releasable while the boiler is still hot
    Conditions c = allClear();
    c.temperaturesOk = false;
    c.sensorsOk = false;  // temperature is reported first
    TEST_ASSERT_TRUE(evaluate(c) == Result::TEMPERATURE_HIGH);
}

void test_emergency_release_refused_on_sensor_or_system_errors() {
    Conditions c = allClear();
    c.sensorsOk = false;
    TEST_ASSERT_TRUE(evaluate(c) == Result::SENSORS_UNAVAILABLE);
    c = allClear();
    c.systemErrorsClear = false;
    TEST_ASSERT_TRUE(evaluate(c) == Result::SYSTEM_ERRORS);
    TEST_ASSERT_TRUE(std::strncmp(toString(Result::SYSTEM_ERRORS), "emergency_release_refused", 25) == 0);
}

void test_emergency_stop_onset_once_per_latch() {
    // 2026-09-14: the burner task read-and-cleared the bit; it now reacts once per onset
    bool wasSet = false;
    TEST_ASSERT_FALSE(onsetDetected(false, wasSet));
    TEST_ASSERT_TRUE(onsetDetected(true, wasSet));
    TEST_ASSERT_FALSE(onsetDetected(true, wasSet));   // still latched: no repeat
    TEST_ASSERT_FALSE(onsetDetected(false, wasSet));  // released
    TEST_ASSERT_TRUE(onsetDetected(true, wasSet));    // new emergency stop
}

void test_emergency_dissipation_until_boiler_cooled() {
    TEST_ASSERT_TRUE(dissipationPumpOn(true, 1150, true, 0));
    TEST_ASSERT_TRUE(dissipationPumpOn(true, 600, true, 0));
    TEST_ASSERT_FALSE(dissipationPumpOn(true, 599, true, 0));
    // Hysteresis: once stopped, on again only from 65.0 °C
    TEST_ASSERT_FALSE(dissipationPumpOn(true, 640, false, 0));
    TEST_ASSERT_TRUE(dissipationPumpOn(true, 650, false, 0));
}

void test_emergency_sensor_recovery_releases_only_stale_sensor_stop() {
    // 2026-09-14: sensor recovery cleared EMERGENCY_STOP whatever had set it
    TEST_ASSERT_TRUE(releasableBySensorRecovery(true, Cause::SENSOR_STALE));
    TEST_ASSERT_FALSE(releasableBySensorRecovery(true, Cause::OTHER));
    TEST_ASSERT_FALSE(releasableBySensorRecovery(true, Cause::NONE));
    TEST_ASSERT_FALSE(releasableBySensorRecovery(false, Cause::SENSOR_STALE));
}

void test_emergency_cause_merge_while_latched() {
    TEST_ASSERT_TRUE(mergeCause(false, Cause::OTHER, Cause::SENSOR_STALE) == Cause::SENSOR_STALE);
    TEST_ASSERT_TRUE(mergeCause(true, Cause::NONE, Cause::SENSOR_STALE) == Cause::SENSOR_STALE);
    TEST_ASSERT_TRUE(mergeCause(true, Cause::SENSOR_STALE, Cause::SENSOR_STALE) == Cause::SENSOR_STALE);
    // Stale sensors, then critical temperature while latched: not sensor-releasable
    TEST_ASSERT_TRUE(mergeCause(true, Cause::SENSOR_STALE, Cause::OTHER) == Cause::OTHER);
    TEST_ASSERT_TRUE(mergeCause(true, Cause::OTHER, Cause::SENSOR_STALE) == Cause::OTHER);
}

void test_emergency_dissipation_continues_after_release_until_cooled() {
    // Review 2026-09-14: a release at up to 109.9 °C stopped both pumps at once
    const uint32_t maxNoReading = 300000;
    TEST_ASSERT_TRUE(dissipationAfterRelease(true, 1000, 0, maxNoReading));
    TEST_ASSERT_TRUE(dissipationAfterRelease(true, 600, 60000, maxNoReading));
    TEST_ASSERT_FALSE(dissipationAfterRelease(true, 599, 60000, maxNoReading));
    // No usable reading: bounded by the pump overrun time
    TEST_ASSERT_TRUE(dissipationAfterRelease(false, 0, 299999, maxNoReading));
    TEST_ASSERT_FALSE(dissipationAfterRelease(false, 0, 300000, maxNoReading));
}

void test_emergency_dissipation_without_usable_output() {
    TEST_ASSERT_TRUE(dissipationPumpOn(false, 200, false, 0));
    TEST_ASSERT_TRUE(dissipationPumpOn(false, 200, true, 0));
}

void test_emergency_safety_split_matches_the_latched_evaluation() {
    // evaluateSafety() was split out of evaluate() so re-enabling the boiler after a
    // sensor-recovery release can be gated on exactly the same conditions as releasing the
    // latch. Pin that: with the latch set, the two must agree on every outcome - if they ever
    // diverge, emergency_reset would re-enable the boiler under a condition that would have
    // refused the release.
    EmergencyStopRelease::Conditions c;
    for (int bits = 0; bits < 8; ++bits) {
        c.emergencyActive    = true;
        c.temperaturesOk     = (bits & 1) != 0;
        c.sensorsOk          = (bits & 2) != 0;
        c.systemErrorsClear  = (bits & 4) != 0;
        TEST_ASSERT_EQUAL(EmergencyStopRelease::evaluate(c), EmergencyStopRelease::evaluateSafety(c));
    }

    // And the ONLY thing evaluate() adds is the latch test: with it clear the answer is
    // NOT_ACTIVE no matter how good or bad the conditions are.
    for (int bits = 0; bits < 8; ++bits) {
        c.emergencyActive    = false;
        c.temperaturesOk     = (bits & 1) != 0;
        c.sensorsOk          = (bits & 2) != 0;
        c.systemErrorsClear  = (bits & 4) != 0;
        TEST_ASSERT_EQUAL(EmergencyStopRelease::Result::NOT_ACTIVE, EmergencyStopRelease::evaluate(c));
    }
}

void test_emergency_safety_refuses_a_re_enable_over_a_hot_boiler() {
    // The repair path in clearEmergencyStop() calls evaluateSafety() with the latch already
    // clear. It must still refuse, in priority order, or emergency_reset would turn the boiler
    // back on over a hot exchanger or a dead sensor just because nothing was latched.
    EmergencyStopRelease::Conditions c;
    c.emergencyActive = false;

    c.temperaturesOk = false; c.sensorsOk = true;  c.systemErrorsClear = true;
    TEST_ASSERT_EQUAL(EmergencyStopRelease::Result::TEMPERATURE_HIGH, EmergencyStopRelease::evaluateSafety(c));

    c.temperaturesOk = true;  c.sensorsOk = false; c.systemErrorsClear = true;
    TEST_ASSERT_EQUAL(EmergencyStopRelease::Result::SENSORS_UNAVAILABLE, EmergencyStopRelease::evaluateSafety(c));

    c.temperaturesOk = true;  c.sensorsOk = true;  c.systemErrorsClear = false;
    TEST_ASSERT_EQUAL(EmergencyStopRelease::Result::SYSTEM_ERRORS, EmergencyStopRelease::evaluateSafety(c));

    // Temperature outranks everything: it is the one with a deadline.
    c.temperaturesOk = false; c.sensorsOk = false; c.systemErrorsClear = false;
    TEST_ASSERT_EQUAL(EmergencyStopRelease::Result::TEMPERATURE_HIGH, EmergencyStopRelease::evaluateSafety(c));

    // Only an all-clear permits it.
    c.temperaturesOk = true;  c.sensorsOk = true;  c.systemErrorsClear = true;
    TEST_ASSERT_EQUAL(EmergencyStopRelease::Result::RELEASED, EmergencyStopRelease::evaluateSafety(c));

    // The new result needs its own label - the operator sees only this string, and
    // "boiler_re_enabled" is what tells them the command did something after years of
    // "emergency_not_active" meaning it had not.
    TEST_ASSERT_EQUAL_STRING("boiler_re_enabled",
                             EmergencyStopRelease::toString(EmergencyStopRelease::Result::BOILER_RE_ENABLED));
    TEST_ASSERT_EQUAL_STRING("emergency_not_active",
                             EmergencyStopRelease::toString(EmergencyStopRelease::Result::NOT_ACTIVE));
}

void test_dissipation_blind_is_bounded_not_forever() {
    using namespace EmergencyStopRelease;
    // THE DEFECT. This used to `return true` with no time input at all, while the
    // after-release rule capped the identical blind case. Since nothing periodic clears a
    // latch, the pumps ran until the module returned or someone power-cycled - circulating
    // the DHW tank through a boiler that had cooled to cellar temperature, for days if the
    // owner was away. Found 2026-10-28.
    TEST_ASSERT_TRUE (dissipationPumpOn(false, 0, true, 0));
    TEST_ASSERT_TRUE (dissipationPumpOn(false, 0, true, MAX_BLIND_DISSIPATION_MS - 1));
    TEST_ASSERT_FALSE(dissipationPumpOn(false, 0, true, MAX_BLIND_DISSIPATION_MS));
    TEST_ASSERT_FALSE(dissipationPumpOn(false, 0, true, MAX_BLIND_DISSIPATION_MS * 2));
    // A usable reading still governs when present - the cap applies ONLY when blind, so a
    // genuinely hot boiler is never abandoned because a timer expired.
    TEST_ASSERT_TRUE(dissipationPumpOn(true, 900, true, MAX_BLIND_DISSIPATION_MS * 10));
    // The two rules now agree in shape: blind -> time-capped, readable -> temperature-driven.
    TEST_ASSERT_EQUAL(dissipationPumpOn(false, 0, true, 1000, 5000),
                      dissipationAfterRelease(false, 0, 1000, 5000));
    TEST_ASSERT_EQUAL(dissipationPumpOn(false, 0, true, 6000, 5000),
                      dissipationAfterRelease(false, 0, 6000, 5000));
}
