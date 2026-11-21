/**
 * @file test_commanded_setpoint.cpp
 * @brief t.bt source and idle hold (CommandedSetpoint.h, 2026-11-21).
 */
#include <unity.h>
#include "../../include/modules/control/CommandedSetpoint.h"

using CommandedSetpoint::select;

void test_setpoint_follows_live_target_while_coasting() {
    // 2026-11-20: the charge target rose 61 -> 63 -> 65 while the burner coasted and t.bt
    // stayed at 61. Each fresh decision must replace the published value.
    Temperature_t held = 0; bool have = false;
    TEST_ASSERT_TRUE(select(true, 610, 1000, held, have)); TEST_ASSERT_EQUAL(610, held);
    TEST_ASSERT_TRUE(select(true, 630, 1000, held, have)); TEST_ASSERT_EQUAL(630, held);
    TEST_ASSERT_TRUE(select(true, 650, 1000, held, have)); TEST_ASSERT_EQUAL(650, held);
}

void test_setpoint_holds_last_value_when_no_request() {
    // User decision: a held value, never a missing key (HA charts that as 0 °C).
    Temperature_t held = 0; bool have = false;
    select(true, 650, 1000, held, have);
    TEST_ASSERT_TRUE(select(true, 0, 1000, held, have));             // request ended
    TEST_ASSERT_EQUAL(650, held);
    TEST_ASSERT_TRUE(select(true, TEMP_INVALID, 1000, held, have));
    TEST_ASSERT_EQUAL(650, held);
}

void test_setpoint_ignores_a_stale_decision() {
    Temperature_t held = 0; bool have = false;
    select(true, 400, 1000, held, have);
    TEST_ASSERT_TRUE(select(true, 550, CommandedSetpoint::MAX_DECISION_AGE_MS + 1, held, have));
    TEST_ASSERT_EQUAL(400, held);
}

void test_setpoint_absent_until_first_usable_value() {
    Temperature_t held = 0; bool have = false;
    TEST_ASSERT_FALSE(select(false, 0, 0, held, have));
    TEST_ASSERT_FALSE(select(true, 0, 100, held, have));
    TEST_ASSERT_TRUE(select(true, 380, 100, held, have));
}
