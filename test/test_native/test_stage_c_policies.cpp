/**
 * @file test_stage_c_policies.cpp
 * @brief Unit tests for power-level fault escalation and water limit consistency
 */

#include <unity.h>
#include <cstdint>

#include "../../include/modules/control/BurnerTransitionPolicy.h"
#include "../../include/modules/control/WaterChargePolicy.h"
#include <cmath>

// setUp and tearDown are defined in test_main.cpp

using BurnerTransitionPolicy::POWER_FAULT_WINDOW_MS;
using BurnerTransitionPolicy::recordPowerFault;

void test_power_fault_escalates_on_third_fault_in_window() {
    uint8_t count = 0;
    uint32_t windowStart = 0;
    // A single refused power change only stops the burner
    TEST_ASSERT_FALSE(recordPowerFault(count, windowStart, 1000));
    // Repeats after the post-purge restart
    TEST_ASSERT_FALSE(recordPowerFault(count, windowStart, 60000));
    TEST_ASSERT_TRUE(recordPowerFault(count, windowStart, 120000));
}

void test_power_fault_window_restarts_after_ten_minutes() {
    uint8_t count = 0;
    uint32_t windowStart = 0;
    TEST_ASSERT_FALSE(recordPowerFault(count, windowStart, 1000));
    TEST_ASSERT_FALSE(recordPowerFault(count, windowStart, 2000));
    // Window over: counting starts again
    TEST_ASSERT_FALSE(recordPowerFault(count, windowStart, 1000 + POWER_FAULT_WINDOW_MS));
    TEST_ASSERT_FALSE(recordPowerFault(count, windowStart, 2000 + POWER_FAULT_WINDOW_MS));
    TEST_ASSERT_TRUE(recordPowerFault(count, windowStart, 3000 + POWER_FAULT_WINDOW_MS));
}

void test_water_limits_valid_only_when_low_below_high() {
    TEST_ASSERT_TRUE(WaterChargePolicy::limitsValid(400, 500));
    TEST_ASSERT_FALSE(WaterChargePolicy::limitsValid(500, 500));
    // 2026-09-14: low set to 60.0 while high was still 50.0
    TEST_ASSERT_FALSE(WaterChargePolicy::limitsValid(600, 500));
    TEST_ASSERT_FALSE(WaterChargePolicy::limitsValid(0, 500));
    TEST_ASSERT_FALSE(WaterChargePolicy::limitsValid(400, 0));
}

void test_water_charge_latch_resumes_only_while_latched() {
    using WaterChargePolicy::nextChargeNeeded;
    // Tank 47.0 between low 40.0 and high 50.0 (2026-09-14 18:12)
    TEST_ASSERT_TRUE(nextChargeNeeded(true, 470, 400, 500));    // charge in progress continues
    TEST_ASSERT_FALSE(nextChargeNeeded(false, 470, 400, 500));  // latch cleared by a disable: no new charge
    TEST_ASSERT_TRUE(nextChargeNeeded(false, 399, 400, 500));   // new charge only below low
    TEST_ASSERT_TRUE(nextChargeNeeded(true, 500, 400, 500));    // at high: still charging
    TEST_ASSERT_FALSE(nextChargeNeeded(true, 501, 400, 500));   // stops above high
}

void test_charge_delta_bounds_match_what_the_parameter_is_registered_with() {
    // The bug, 2026-10-23: PersistentStorageTask registered wheater/tempChargeDelta as
    // 1.0-20.0 while the use site rejected anything below 5.0 and substituted the default.
    // A params/set of 1.0-4.9 was validated, acknowledged and persisted, then ignored -
    // stored value and running value disagreed with no error anywhere. Both sites now read
    // these constants, so this test fails if they are ever split apart again.
    TEST_ASSERT_EQUAL_FLOAT(5.0f,  WaterChargePolicy::CHARGE_DELTA_MIN);
    TEST_ASSERT_EQUAL_FLOAT(20.0f, WaterChargePolicy::CHARGE_DELTA_MAX);
    TEST_ASSERT_EQUAL_FLOAT(10.0f, WaterChargePolicy::CHARGE_DELTA_DEFAULT);
    // The default must itself be in range, or every fallback would be invalid.
    TEST_ASSERT_TRUE(WaterChargePolicy::chargeDeltaValid(
        WaterChargePolicy::CHARGE_DELTA_DEFAULT));
}

void test_charge_delta_valid_range_is_inclusive_at_both_ends() {
    using WaterChargePolicy::chargeDeltaValid;
    TEST_ASSERT_TRUE(chargeDeltaValid(5.0f));    // exactly the floor
    TEST_ASSERT_TRUE(chargeDeltaValid(10.0f));   // the default
    TEST_ASSERT_TRUE(chargeDeltaValid(15.0f));   // set live 2026-10-23
    TEST_ASSERT_TRUE(chargeDeltaValid(20.0f));   // exactly the ceiling
    TEST_ASSERT_FALSE(chargeDeltaValid(4.9f));   // the silently-ignored band
    TEST_ASSERT_FALSE(chargeDeltaValid(1.0f));   // the old registered minimum
    TEST_ASSERT_FALSE(chargeDeltaValid(0.0f));
    TEST_ASSERT_FALSE(chargeDeltaValid(-10.0f));
    TEST_ASSERT_FALSE(chargeDeltaValid(20.1f));
}

void test_charge_delta_rejects_nan_and_inf() {
    using WaterChargePolicy::chargeDeltaValid;
    using WaterChargePolicy::chargeDeltaOrDefault;
    // NaN passes BOTH range comparisons, so without the isfinite guard it would be
    // accepted and turned into a NaN setpoint (same class as the F24 storage bug).
    TEST_ASSERT_FALSE(chargeDeltaValid(NAN));
    TEST_ASSERT_FALSE(chargeDeltaValid(INFINITY));
    TEST_ASSERT_FALSE(chargeDeltaValid(-INFINITY));
    TEST_ASSERT_EQUAL_FLOAT(10.0f, chargeDeltaOrDefault(NAN));
    TEST_ASSERT_EQUAL_FLOAT(10.0f, chargeDeltaOrDefault(INFINITY));
}

void test_charge_delta_or_default_passes_valid_values_through_unchanged() {
    using WaterChargePolicy::chargeDeltaOrDefault;
    TEST_ASSERT_EQUAL_FLOAT(15.0f, chargeDeltaOrDefault(15.0f));
    TEST_ASSERT_EQUAL_FLOAT(5.0f,  chargeDeltaOrDefault(5.0f));
    TEST_ASSERT_EQUAL_FLOAT(20.0f, chargeDeltaOrDefault(20.0f));
    // Out of range falls back rather than clamping: a 25.0 request is a mistake, and
    // clamping to 20.0 would hide it behind a plausible-looking value.
    TEST_ASSERT_EQUAL_FLOAT(10.0f, chargeDeltaOrDefault(25.0f));
    TEST_ASSERT_EQUAL_FLOAT(10.0f, chargeDeltaOrDefault(1.0f));
}

