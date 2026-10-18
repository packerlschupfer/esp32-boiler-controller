/**
 * @file test_sensor_error_bit_policy.cpp
 * @brief Unit tests for SensorErrorBitPolicy + the confirm window that feeds it.
 *
 * Regression cover for 2026-10-18: one 100 ms sensor-mutex timeout raised
 * Error::SENSOR_FAILURE, nothing cleared it while the sensors were healthy, and because
 * the bit is in CRITICAL_ERRORS and gates clearEmergencyStop() the boiler sat in an
 * emergency stop that no MQTT command could release.
 */

#include <unity.h>
#include <cstdint>

#include "../../include/modules/control/SensorErrorBitPolicy.h"
#include "../../include/modules/control/SensorFailureConfirm.h"

// setUp and tearDown are defined in test_main.cpp

using Action = SensorErrorBitPolicy::Action;

void test_sensor_error_bit_single_failure_does_not_raise() {
    // THE regression: one failed read must not raise the bit. Before the fix this was an
    // unconditional set, and the emergency stop that followed needed a reboot to clear.
    SensorFailureConfirm::State st;
    const bool confirmed = SensorFailureConfirm::shouldStop(st, /*sensorsOk=*/false,
                                                            /*heatDemand=*/true, 1000);
    TEST_ASSERT_FALSE(confirmed);
    TEST_ASSERT_EQUAL(static_cast<int>(Action::NONE),
                      static_cast<int>(SensorErrorBitPolicy::decide(confirmed, false, false)));
}

void test_sensor_error_bit_raises_only_after_the_confirm_window() {
    SensorFailureConfirm::State st;
    uint32_t now = 5000;
    TEST_ASSERT_FALSE(SensorFailureConfirm::shouldStop(st, false, true, now));   // arms
    now += SensorFailureConfirm::CONFIRM_MS - 1;
    TEST_ASSERT_FALSE(SensorFailureConfirm::shouldStop(st, false, true, now));   // 1 ms short
    now += 1;
    const bool confirmed = SensorFailureConfirm::shouldStop(st, false, true, now);
    TEST_ASSERT_TRUE(confirmed);
    TEST_ASSERT_EQUAL(static_cast<int>(Action::RAISE),
                      static_cast<int>(SensorErrorBitPolicy::decide(confirmed, false, false)));
}

void test_sensor_error_bit_recovery_inside_the_window_never_raises() {
    // A transient stall that recovers before the window elapses must leave no trace.
    SensorFailureConfirm::State st;
    uint32_t now = 0;
    for (int i = 0; i < 5; ++i) {
        now += 1000;
        TEST_ASSERT_FALSE(SensorFailureConfirm::shouldStop(st, false, true, now));
    }
    now += 1000;
    TEST_ASSERT_FALSE(SensorFailureConfirm::shouldStop(st, /*sensorsOk=*/true, true, now));
    // Window re-arms from scratch, so the next failure run starts over.
    now += 1000;
    TEST_ASSERT_FALSE(SensorFailureConfirm::shouldStop(st, false, true, now));
    now += SensorFailureConfirm::CONFIRM_MS - 1;
    TEST_ASSERT_FALSE(SensorFailureConfirm::shouldStop(st, false, true, now));
}

void test_sensor_error_bit_clears_on_recovery_when_owned() {
    TEST_ASSERT_EQUAL(static_cast<int>(Action::CLEAR),
                      static_cast<int>(SensorErrorBitPolicy::decide(false, /*readOk=*/true,
                                                                    /*ownedHere=*/true)));
}

void test_sensor_error_bit_never_clears_a_bit_owned_by_the_fallback() {
    // TemperatureSensorFallback raises this bit for a genuinely missing sensor. Our reads
    // succeeding says nothing about that, so we must not clear what we did not raise -
    // clearing it would unblock performSafetyCheck() and permit an emergency-stop release
    // while a sensor is still gone.
    TEST_ASSERT_EQUAL(static_cast<int>(Action::NONE),
                      static_cast<int>(SensorErrorBitPolicy::decide(false, /*readOk=*/true,
                                                                    /*ownedHere=*/false)));
}

void test_sensor_error_bit_raise_is_idempotent_and_outranks_clear() {
    // decide() runs every cycle, so re-raising an owned bit must stay RAISE, and a
    // confirmed failure must win even if this cycle's read happened to succeed.
    TEST_ASSERT_EQUAL(static_cast<int>(Action::RAISE),
                      static_cast<int>(SensorErrorBitPolicy::decide(true, false, true)));
    TEST_ASSERT_EQUAL(static_cast<int>(Action::RAISE),
                      static_cast<int>(SensorErrorBitPolicy::decide(true, true, true)));
}

void test_sensor_error_bit_full_latch_regression_sequence() {
    // End to end: fail long enough to raise, then recover, and assert the bit comes back
    // down. This is the sequence that needed a reboot before the fix.
    SensorFailureConfirm::State st;
    bool owned = false;
    bool bitSet = false;
    uint32_t now = 0;

    auto step = [&](bool readOk) {
        now += 1000;
        const bool confirmed = SensorFailureConfirm::shouldStop(st, readOk, true, now);
        switch (SensorErrorBitPolicy::decide(confirmed, readOk, owned)) {
            case Action::RAISE: bitSet = true;  owned = true;  break;
            case Action::CLEAR: bitSet = false; owned = false; break;
            case Action::NONE:  break;
        }
    };

    for (int i = 0; i < 11; ++i) { step(false); }   // 11 s of failures
    TEST_ASSERT_TRUE(bitSet);
    TEST_ASSERT_TRUE(owned);

    step(true);                                     // sensors readable again
    TEST_ASSERT_FALSE(bitSet);
    TEST_ASSERT_FALSE(owned);
}
