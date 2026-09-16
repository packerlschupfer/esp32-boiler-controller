/**
 * @file test_autotune_gain_target.cpp
 * @brief Which gain set an autotune result is persisted to (review 2026-09-14 pid-4)
 *
 * BoilerTempController::updateMode() does not run while tuning, so isWaterMode_ still
 * held the mode of the last normal control cycle when the result was saved. The mode is
 * captured at startAutoTuning() and re-checked against the active burner request instead.
 */

#include <unity.h>
#include <cstring>

#include "../../include/modules/control/AutotuneGainTarget.h"

// setUp and tearDown are defined in test_main.cpp

using AutotuneGainTarget::Target;

void test_autotune_gains_go_to_the_gain_set_of_the_run() {
    // Water request at start and still at the end: wHeaterK*
    TEST_ASSERT_TRUE(AutotuneGainTarget::select(true, true) == Target::WATER);
    // Space heating run: spaceHeatingK*
    TEST_ASSERT_TRUE(AutotuneGainTarget::select(false, false) == Target::SPACE);

    // MQTT result payload / log text
    TEST_ASSERT_EQUAL_STRING("water", AutotuneGainTarget::toString(Target::WATER));
    TEST_ASSERT_EQUAL_STRING("space", AutotuneGainTarget::toString(Target::SPACE));
}

void test_autotune_result_rejected_when_mode_changed() {
    // Started in space heating, water charge running at the end (and the other way round):
    // part of the oscillation was measured on the radiators and part on the tank, so the
    // result belongs to neither gain set. Rejecting it is safer than writing the wrong one
    // - the live water gains are hand-corrected values.
    TEST_ASSERT_TRUE(AutotuneGainTarget::select(false, true) == Target::REJECT_MODE_CHANGED);
    TEST_ASSERT_TRUE(AutotuneGainTarget::select(true, false) == Target::REJECT_MODE_CHANGED);
    TEST_ASSERT_EQUAL_STRING("rejected", AutotuneGainTarget::toString(Target::REJECT_MODE_CHANGED));
}
