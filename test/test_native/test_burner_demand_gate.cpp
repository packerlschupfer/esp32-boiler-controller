/**
 * @file test_burner_demand_gate.cpp
 * @brief Unit tests for BurnerDemandGate (single effective source for arming heat demand)
 */

#include <unity.h>
#include <cstdint>

#include "../../include/modules/control/BurnerDemandGate.h"

// setUp and tearDown are defined in test_main.cpp

using namespace BurnerDemandGate;

namespace {

// Two-task replay: BurnerControlTask handles the request start once, then
// BoilerTempControlTask cycles every 2.5 s. The PID is modelled as "heat while the
// boiler is below target" (what the fixed-point PID does after its resume reset).
struct DemandReplay {
    bool demand = false;
    bool pidOn = false;
    int arms = 0;

    void requestStart(bool permitted, int16_t boiler, int16_t target, bool decisionFresh,
                      int16_t decisionTarget, bool decisionOn) {
        if (!permitted) {
            demand = false;
        } else if (controlTaskMayArm(true, boiler, target, decisionFresh, decisionTarget, decisionOn)) {
            if (!demand) {
                arms++;
            }
            demand = true;
        }
    }

    void pidCycle(bool permitted, int16_t boiler, int16_t target) {
        const bool on = boiler < target;
        const bool changed = on != pidOn;
        pidOn = on;
        switch (decide(permitted, on, changed, demand)) {
            case Action::ARM:
                arms++;
                demand = true;
                break;
            case Action::DISARM:
                demand = false;
                break;
            default:
                break;
        }
    }
};

} // namespace

void test_gate_hot_boiler_request_does_not_arm() {
    // 2026-09-14 17:12:42: heating enabled, boiler 64.4 C, heating target 47.0 C,
    // no recent PID decision (PID paused for 490 s)
    DemandReplay r;
    r.requestStart(true, 644, 470, false, 0, false);
    TEST_ASSERT_FALSE(r.demand);
    // Boiler cools through the heating circuit; demand stays off above target
    int16_t boiler = 644;
    for (int cycle = 0; cycle < 200 && boiler >= 470; cycle++) {
        r.pidCycle(true, boiler, 470);
        TEST_ASSERT_FALSE(r.demand);
        boiler -= 1;
    }
    // Below target the PID arms exactly once
    r.pidCycle(true, 469, 470);
    TEST_ASSERT_TRUE(r.demand);
    TEST_ASSERT_EQUAL_INT(1, r.arms);
}

void test_gate_cold_boiler_request_arms_immediately() {
    DemandReplay r;
    r.requestStart(true, 400, 470, false, 0, false);
    TEST_ASSERT_TRUE(r.demand);
    r.pidCycle(true, 400, 470);
    TEST_ASSERT_TRUE(r.demand);
    TEST_ASSERT_EQUAL_INT(1, r.arms);
}

void test_gate_handover_uses_temperature_when_decision_was_for_other_target() {
    // Water (target 66.0, PID on) -> heating (target 47.0): the fresh decision was
    // for the water target, so the boiler temperature decides
    TEST_ASSERT_TRUE(controlTaskMayArm(true, 400, 470, true, 660, true));
    TEST_ASSERT_FALSE(controlTaskMayArm(true, 530, 470, true, 660, true));
}

void test_gate_fresh_matching_decision_wins_over_temperature() {
    // PID still coasting after an overshoot although the boiler is just below target
    TEST_ASSERT_FALSE(controlTaskMayArm(true, 465, 470, true, 470, false));
    // PID holding HALF slightly above target
    TEST_ASSERT_TRUE(controlTaskMayArm(true, 478, 470, true, 475, true));
    // Stale decision: temperature decides
    TEST_ASSERT_TRUE(controlTaskMayArm(true, 465, 470, false, 470, false));
    // Boundary: at target is not below target
    TEST_ASSERT_FALSE(controlTaskMayArm(true, 470, 470, false, 0, false));
}

void test_gate_without_boiler_temperature_control_task_arms() {
    // PID cannot run without a valid boiler output temperature (sensor fallback)
    TEST_ASSERT_TRUE(controlTaskMayArm(false, 0, 470, true, 470, false));
}

void test_gate_pid_arms_demand_it_did_not_see_armed() {
    // Output stays HALF (no change) but demand is off, e.g. after a preheating
    // block was lifted - must still arm, otherwise the burner never starts
    TEST_ASSERT_TRUE(decide(true, true, false, false) == Action::ARM);
    TEST_ASSERT_TRUE(decide(true, true, true, false) == Action::ARM);
}

void test_gate_drops_demand_rearmed_while_coasting() {
    TEST_ASSERT_TRUE(decide(true, false, false, true) == Action::DISARM);
    TEST_ASSERT_TRUE(decide(true, false, true, true) == Action::DISARM);
    TEST_ASSERT_TRUE(decide(true, false, false, false) == Action::NONE);
}

void test_gate_not_permitted_never_arms() {
    TEST_ASSERT_TRUE(decide(false, true, true, false) == Action::NONE);
    TEST_ASSERT_TRUE(decide(false, true, false, true) == Action::DISARM);
}

void test_gate_power_update_only_on_pid_change() {
    TEST_ASSERT_TRUE(decide(true, true, true, true) == Action::SET_POWER);
    TEST_ASSERT_TRUE(decide(true, true, false, true) == Action::NONE);
}

void test_gate_fallback_target_cap() {
    TEST_ASSERT_EQUAL_INT(600, cappedTarget(700, 600));
    TEST_ASSERT_EQUAL_INT(500, cappedTarget(500, 600));
    TEST_ASSERT_EQUAL_INT(700, cappedTarget(700, 0));
}
