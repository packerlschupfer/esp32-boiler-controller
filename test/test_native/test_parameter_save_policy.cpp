/**
 * @file test_parameter_save_policy.cpp
 * @brief Unit tests for ParameterSavePolicy (NVS save debounce, 2026-10-18)
 */

#include <unity.h>
#include <cstdint>

#include "../../include/modules/tasks/ParameterSavePolicy.h"

// setUp and tearDown are defined in test_main.cpp

using namespace ParameterSavePolicy;

static constexpr uint32_t DEBOUNCE = 2000;

void test_param_save_explicit_request_is_never_delayed() {
    // boiler/params/save and the StateManager enable/priority/override writers must not wait,
    // with or without a pending parameter change.
    TEST_ASSERT_TRUE(shouldSave(true, false, 1000, 1000, DEBOUNCE));
    TEST_ASSERT_TRUE(shouldSave(true, true, 1000, 1000, DEBOUNCE));
}

void test_param_save_idle_never_saves() {
    // No change pending: the 1 s loop tick must not write NVS.
    TEST_ASSERT_FALSE(shouldSave(false, false, 100000, 1000, DEBOUNCE));
}

void test_param_save_waits_for_the_debounce_window() {
    TEST_ASSERT_FALSE(shouldSave(false, true, 1000, 1000, DEBOUNCE));
    TEST_ASSERT_FALSE(shouldSave(false, true, 2999, 1000, DEBOUNCE));
    TEST_ASSERT_TRUE(shouldSave(false, true, 3000, 1000, DEBOUNCE));   // exactly at the window
    TEST_ASSERT_TRUE(shouldSave(false, true, 9000, 1000, DEBOUNCE));
}

void test_param_save_coalesces_a_burst_of_clicks() {
    // The case this exists for: a thermostat card sending five values 300 ms apart used to
    // run five full saveAll() passes. One save, after the last click settles.
    uint32_t now = 10000;
    uint32_t lastChange = 0;
    bool changed = false;
    int saves = 0;
    for (int click = 0; click < 5; ++click) {
        changed = true;
        lastChange = now;
        for (int tick = 0; tick < 3; ++tick) {      // 3 x 100 ms of loop ticks between clicks
            now += 100;
            if (shouldSave(false, changed, now, lastChange, DEBOUNCE)) {
                ++saves;
                changed = false;
            }
        }
    }
    TEST_ASSERT_EQUAL_INT(0, saves);                // nothing written during the burst
    now += DEBOUNCE;
    TEST_ASSERT_TRUE(shouldSave(false, changed, now, lastChange, DEBOUNCE));
}

void test_param_save_survives_the_millis_wrap() {
    // millis() wraps every ~49.7 days. Unsigned subtraction keeps the elapsed time right;
    // a signed compare here would stall the save for the next 49 days.
    const uint32_t lastChange = 0xFFFFF000u;        // 4 096 ms before the wrap
    TEST_ASSERT_FALSE(shouldSave(false, true, 0xFFFFF400u, lastChange, DEBOUNCE));  // +1024, early
    TEST_ASSERT_TRUE(shouldSave(false, true, 0xFFFFF800u, lastChange, DEBOUNCE));   // +2048, due
    TEST_ASSERT_TRUE(shouldSave(false, true, 0x00000800u, lastChange, DEBOUNCE));   // wrapped, +6144
}
