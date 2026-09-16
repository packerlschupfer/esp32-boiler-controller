/**
 * @file test_burner_demand_gate.cpp
 * @brief Unit tests for BurnerDemandGate (single effective source for arming heat demand)
 */

#include <unity.h>
#include <cstdint>

#include "../../include/modules/control/BurnerDemandGate.h"
#include "../../include/modules/control/BoilerPowerLevel.h"
#include "../../include/modules/control/SensorFailureConfirm.h"

// setUp and tearDown are defined in test_main.cpp

using namespace BurnerDemandGate;
using BoilerPowerLevel::Level;

namespace {

// Space heating gains measured 2026-09-14 (Kp 13.22, Ki 0.0453)
const int32_t GATE_KP = PIDGainFixedPoint::fromFloat(13.22f);
const int32_t GATE_KI = PIDGainFixedPoint::fromFloat(0.0453f);
// Default water heating gains (Kp 1.0, Ki 0.02)
const int32_t WATER_KP = PIDGainFixedPoint::fromFloat(1.0f);
const int32_t WATER_KI = PIDGainFixedPoint::fromFloat(0.02f);

FixedPointPIDStep::Limits gatePidLimits() {
    FixedPointPIDStep::Limits limits = {
        -100000, 100000,
        static_cast<int16_t>(-PIDGainFixedPoint::POWER_ADJUSTMENT_LIMIT),
        PIDGainFixedPoint::POWER_ADJUSTMENT_LIMIT
    };
    return limits;
}

// PID state left by a previous request: integral at its clamp
FixedPointPIDStep::State woundPidState() {
    FixedPointPIDStep::State state = {100000, 480, false};
    return state;
}

// BoilerTempController::predictHeatDemand(): BoilerPowerLevel::predictModulatingLevel(),
// the firmware's reset / start level decision and one PID cycle on a copy of the state
Prediction predictNext(Level last, const FixedPointPIDStep::State& pid, bool paused, bool modeOrGainsChanged,
                       int16_t boiler, int16_t target, int32_t kp = GATE_KP, int32_t ki = GATE_KI) {
    const Level next = BoilerPowerLevel::predictModulatingLevel(
        last, pid, gatePidLimits(), paused, modeOrGainsChanged, target, boiler, kp, ki, 0,
        BoilerPowerLevel::defaultThresholds());
    return (next != Level::OFF) ? Prediction::HEAT : Prediction::OFF;
}

// Running PID, no pause and no mode or gain change
Prediction predict(Level last, const FixedPointPIDStep::State& pid, int16_t boiler, int16_t target,
                   int32_t kp = GATE_KP, int32_t ki = GATE_KI) {
    return predictNext(last, pid, false, false, boiler, target, kp, ki);
}

// First request after a pause: the stale level and PID state must not matter
Prediction predictAfterPause(int16_t boiler, int16_t target, int32_t kp = GATE_KP, int32_t ki = GATE_KI) {
    return predictNext(Level::FULL, woundPidState(), true, false, boiler, target, kp, ki);
}

// Two-task replay: BurnerControlTask handles the request start once, then
// BoilerTempControlTask cycles every 2.5 s with the firmware PID and power mapping.
// The previous request ended at HALF and the PID was paused since, so the first
// cycle starts through BoilerPowerLevel::modulatingCycleStart() like calculateModulating().
struct DemandReplay {
    bool demand = false;
    Level pidLevel = Level::HALF;
    FixedPointPIDStep::State pid = woundPidState();
    bool paused = true;
    int arms = 0;
    int disarms = 0;

    void requestStart(bool permitted, int16_t boiler, int16_t target, bool decisionFresh,
                      int16_t decisionTarget, bool decisionOn) {
        if (!permitted) {
            demand = false;
        } else if (controlTaskMayArm(true, boiler, target, decisionFresh, decisionTarget, decisionOn,
                                     predictNext(pidLevel, pid, paused, false, boiler, target))) {
            if (!demand) {
                arms++;
            }
            demand = true;
        }
    }

    void pidCycle(bool permitted, int16_t boiler, int16_t target) {
        const BoilerPowerLevel::CycleStart start = BoilerPowerLevel::modulatingCycleStart(pidLevel, paused, false);
        paused = false;
        if (start.resetPid) {
            pid = FixedPointPIDStep::resetState();
        }
        pidLevel = start.level;
        const Level next = BoilerPowerLevel::nextModulatingLevel(
            pidLevel, pid, gatePidLimits(), target, boiler, GATE_KP, GATE_KI, 0,
            BoilerPowerLevel::PID_NOMINAL_DT_MS, BoilerPowerLevel::defaultThresholds());
        const bool on = (next != Level::OFF);
        const bool changed = (next != pidLevel);
        pidLevel = next;
        switch (decide(permitted, on, changed, demand)) {
            case Action::ARM:
                arms++;
                demand = true;
                break;
            case Action::DISARM:
                disarms++;
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
    // Boiler cools through the heating circuit; demand stays off above target and
    // is armed by the PID once it crosses its on-threshold below target
    int16_t boiler = 644;
    while (!r.demand && boiler > 380) {
        r.pidCycle(true, boiler, 470);
        if (r.demand) {
            break;
        }
        boiler--;
    }
    TEST_ASSERT_TRUE(r.demand);
    TEST_ASSERT_TRUE(boiler < 470);
    TEST_ASSERT_EQUAL_INT(1, r.arms);
    // Still cooling slowly after the start: no disarm (no short start)
    for (int cycle = 0; cycle < 20; cycle++) {
        r.pidCycle(true, boiler, 470);
    }
    TEST_ASSERT_TRUE(r.demand);
    TEST_ASSERT_EQUAL_INT(0, r.disarms);
}

void test_gate_cold_boiler_request_arms_immediately() {
    DemandReplay r;
    r.requestStart(true, 400, 470, false, 0, false);
    TEST_ASSERT_TRUE(r.demand);
    r.pidCycle(true, 400, 470);
    TEST_ASSERT_TRUE(r.demand);
    TEST_ASSERT_EQUAL_INT(1, r.arms);
    TEST_ASSERT_EQUAL_INT(0, r.disarms);
}

void test_gate_no_decision_follows_pid_on_threshold() {
    // Review 2026-09-14 tests-4: from OFF the PID turns on only above 55 %, the first
    // cycle after the resume reset is almost P-only. Kp 13.22: 0.5 C below target.
    TEST_ASSERT_TRUE(predictAfterPause(467, 470) == Prediction::OFF);
    TEST_ASSERT_FALSE(controlTaskMayArm(true, 467, 470, false, 0, false, predictAfterPause(467, 470)));
    TEST_ASSERT_TRUE(controlTaskMayArm(true, 465, 470, false, 0, false, predictAfterPause(465, 470)));
    // Kp 1.0 / Ki 0.02: several degrees below target
    TEST_ASSERT_TRUE(predictAfterPause(430, 470, WATER_KP, WATER_KI) == Prediction::OFF);
    TEST_ASSERT_TRUE(predictAfterPause(410, 470, WATER_KP, WATER_KI) == Prediction::HEAT);
    // Only without a prediction (controller unavailable) the temperature decides
    TEST_ASSERT_TRUE(controlTaskMayArm(true, 467, 470, false, 0, false, Prediction::UNKNOWN));
}

void test_gate_band_below_target_no_short_start() {
    // Boiler 0.3 C below target: the former "boiler < target" rule armed here, the
    // PID stayed OFF and disarmed on its next cycle - after ignition. The stale HALF
    // of the previous request would have held here as well (review R1-1).
    DemandReplay r;
    r.requestStart(true, 467, 470, false, 0, false);
    TEST_ASSERT_FALSE(r.demand);
    for (int cycle = 0; cycle < 4; cycle++) {
        r.pidCycle(true, 467, 470);
    }
    TEST_ASSERT_FALSE(r.demand);
    TEST_ASSERT_EQUAL_INT(0, r.arms);
    TEST_ASSERT_EQUAL_INT(0, r.disarms);
}

void test_gate_prediction_follows_current_level() {
    // Burner at HALF with a running PID: still heating 1.0 C above target (HALF holds
    // down to 35 %), so a re-processed request arms like the next PID cycle would
    FixedPointPIDStep::State pid = FixedPointPIDStep::resetState();
    TEST_ASSERT_TRUE(predict(Level::HALF, pid, 480, 470) == Prediction::HEAT);
    TEST_ASSERT_TRUE(predict(Level::OFF, pid, 480, 470) == Prediction::OFF);
}

void test_gate_handover_uses_prediction_when_decision_was_for_other_target() {
    // Water (target 66.0, PID on) -> heating (target 47.0): the fresh decision was
    // for the water target, so the controller's prediction for 47.0 decides
    TEST_ASSERT_TRUE(controlTaskMayArm(true, 400, 470, true, 660, true, predictAfterPause(400, 470)));
    TEST_ASSERT_FALSE(controlTaskMayArm(true, 530, 470, true, 660, true, predictAfterPause(530, 470)));
}

void test_gate_fresh_matching_decision_wins_over_temperature() {
    // PID still coasting after an overshoot although the boiler is just below target
    TEST_ASSERT_FALSE(controlTaskMayArm(true, 465, 470, true, 470, false, Prediction::HEAT));
    // PID holding HALF slightly above target
    TEST_ASSERT_TRUE(controlTaskMayArm(true, 478, 470, true, 475, true, Prediction::OFF));
    // Stale decision: the prediction decides, without one the temperature
    TEST_ASSERT_FALSE(controlTaskMayArm(true, 465, 470, false, 470, false, Prediction::OFF));
    TEST_ASSERT_TRUE(controlTaskMayArm(true, 465, 470, false, 470, false, Prediction::UNKNOWN));
    // Boundary: at target is not below target
    TEST_ASSERT_FALSE(controlTaskMayArm(true, 470, 470, false, 0, false, Prediction::UNKNOWN));
}

void test_gate_without_boiler_temperature_control_task_arms() {
    // PID cannot run without a valid boiler output temperature (sensor fallback)
    TEST_ASSERT_TRUE(controlTaskMayArm(false, 0, 470, true, 470, false, Prediction::UNKNOWN));
    TEST_ASSERT_TRUE(controlTaskMayArm(false, 0, 470, false, 0, false, Prediction::OFF));
}

void test_gate_revoke_race_cannot_leave_demand_armed() {
    // Review 2026-09-14 bsm-6, through applyDemandWrite(), the body BurnerStateMachine::
    // setHeatDemand() runs under demandMutex. BoilerTempControlTask: S = permission
    // snapshot (granted), A = setHeatDemand(true). BurnerControlTask: R = revoke
    // (setDemandPermission), D = setHeatDemand(false). Every interleaving with S before A,
    // R before D and S before R (the snapshot still saw the grant).
    static const char* const ORDERS[] = {"SARD", "SRAD", "SRDA"};
    for (int i = 0; i < 3; i++) {
        bool permitted = true;
        bool snapshot = false;
        DemandSlot checked = {false, 470, false};
        DemandSlot snapshotOnly = {false, 470, false};  // former behaviour: arm from the snapshot
        WriteResult armResult = WriteResult::UNCHANGED;
        for (const char* step = ORDERS[i]; *step != '\0'; step++) {
            switch (*step) {
                case 'S':
                    snapshot = permitted;
                    break;
                case 'R':
                    permitted = false;
                    break;
                case 'D':
                    TEST_ASSERT_TRUE(applyDemandWrite(checked, false, 0, false, permitted) != WriteResult::REFUSED);
                    applyDemandWrite(snapshotOnly, false, 0, false, true);
                    break;
                case 'A':
                    armResult = applyDemandWrite(checked, true, 470, false, permitted);
                    applyDemandWrite(snapshotOnly, true, 470, false, snapshot);
                    break;
                default:
                    break;
            }
        }
        TEST_ASSERT_FALSE(checked.demand);
        TEST_ASSERT_TRUE(armResult == ((i == 0) ? WriteResult::UPDATED : WriteResult::REFUSED));
        if (i == 2) {
            TEST_ASSERT_TRUE(snapshotOnly.demand);  // the re-armed revoke
        }
    }
    // Writing OFF is always allowed
    TEST_ASSERT_TRUE(demandWriteAllowed(false, false));
    TEST_ASSERT_TRUE(demandWriteAllowed(true, true));
}

void test_gate_demand_write_change_detection() {
    DemandSlot slot = {false, 470, false};
    // Refused ON leaves the slot untouched
    TEST_ASSERT_TRUE(applyDemandWrite(slot, true, 600, true, false) == WriteResult::REFUSED);
    TEST_ASSERT_FALSE(slot.demand);
    TEST_ASSERT_EQUAL_INT(470, slot.target);
    TEST_ASSERT_FALSE(slot.highPower);
    // OFF without a target keeps the stored target
    TEST_ASSERT_TRUE(applyDemandWrite(slot, false, 0, false, false) == WriteResult::UNCHANGED);
    TEST_ASSERT_TRUE(applyDemandWrite(slot, true, 0, false, true) == WriteResult::UPDATED);
    TEST_ASSERT_EQUAL_INT(470, slot.target);
    // A target change of 0.1 C is not an update, 0.2 C is
    TEST_ASSERT_TRUE(applyDemandWrite(slot, true, 471, false, true) == WriteResult::UNCHANGED);
    TEST_ASSERT_EQUAL_INT(470, slot.target);
    TEST_ASSERT_TRUE(applyDemandWrite(slot, true, 472, false, true) == WriteResult::UPDATED);
    TEST_ASSERT_EQUAL_INT(472, slot.target);
    // Power change alone is an update
    TEST_ASSERT_TRUE(applyDemandWrite(slot, true, 472, true, true) == WriteResult::UPDATED);
    TEST_ASSERT_TRUE(slot.highPower);
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

// ===== Cycle start after a pause (review R1-1, R1-2) =====

void test_cycle_start_pause_resets_pid_and_starts_off() {
    BoilerPowerLevel::CycleStart start = BoilerPowerLevel::modulatingCycleStart(Level::HALF, true, false);
    TEST_ASSERT_TRUE(start.resetPid);
    TEST_ASSERT_TRUE(start.level == Level::OFF);
    start = BoilerPowerLevel::modulatingCycleStart(Level::FULL, true, true);
    TEST_ASSERT_TRUE(start.resetPid);
    TEST_ASSERT_TRUE(start.level == Level::OFF);
}

void test_cycle_start_mode_or_gain_change_keeps_level() {
    // Seamless water <-> heating handover: PID reset, hysteresis from the running level
    BoilerPowerLevel::CycleStart start = BoilerPowerLevel::modulatingCycleStart(Level::HALF, false, true);
    TEST_ASSERT_TRUE(start.resetPid);
    TEST_ASSERT_TRUE(start.level == Level::HALF);
    start = BoilerPowerLevel::modulatingCycleStart(Level::FULL, false, false);
    TEST_ASSERT_FALSE(start.resetPid);
    TEST_ASSERT_TRUE(start.level == Level::FULL);
}

void test_cycle_start_pause_detection_and_read_order() {
    TEST_ASSERT_FALSE(BoilerPowerLevel::pidPaused(12500u, 2500u));   // exactly PID_MAX_DT_MS
    TEST_ASSERT_TRUE(BoilerPowerLevel::pidPaused(12501u, 2500u));
    TEST_ASSERT_FALSE(BoilerPowerLevel::pidPaused(0x00000FFFu, 0xFFFFF000u));  // across the millis() wrap
    // Review R1-2: millis() taken before a newer cycle time was stored wraps into a
    // false pause - why predictHeatDemand() reads lastPIDTime_ before millis()
    TEST_ASSERT_TRUE(BoilerPowerLevel::pidPaused(100000u, 100001u));
}

void test_prediction_stale_half_after_pause_does_not_arm_above_target() {
    // Review R1-1: a water request ended at HALF; the next one starts 10 C and 14 C above
    // target with the default water gains. The stale HALF holds (PID 35-40 %, HALF only
    // drops below 35 %), so the former start level armed and ignited here.
    const FixedPointPIDStep::State fresh = FixedPointPIDStep::resetState();
    TEST_ASSERT_TRUE(predictNext(Level::HALF, fresh, false, true, 560, 460, WATER_KP, WATER_KI) == Prediction::HEAT);
    TEST_ASSERT_TRUE(predictNext(Level::HALF, fresh, false, true, 600, 460, WATER_KP, WATER_KI) == Prediction::HEAT);
    TEST_ASSERT_TRUE(predictNext(Level::HALF, fresh, true, false, 560, 460, WATER_KP, WATER_KI) == Prediction::OFF);
    TEST_ASSERT_TRUE(predictNext(Level::HALF, woundPidState(), true, false, 600, 460, WATER_KP, WATER_KI) == Prediction::OFF);
    // FULL left by an autotune stop, 3 C above target
    TEST_ASSERT_TRUE(predictNext(Level::FULL, fresh, false, true, 490, 460, WATER_KP, WATER_KI) == Prediction::HEAT);
    TEST_ASSERT_TRUE(predictNext(Level::FULL, woundPidState(), true, false, 490, 460, WATER_KP, WATER_KI) == Prediction::OFF);
    // Well below target a paused start still heats
    TEST_ASSERT_TRUE(predictNext(Level::HALF, woundPidState(), true, false, 380, 460, WATER_KP, WATER_KI) == Prediction::HEAT);
}

// ===== Autotune demand (review 2026-09-14 pid-3, R1-4) =====

void test_autotune_off_phase_drops_demand_armed_elsewhere() {
    // Tuner OFF, unchanged, but BurnerControlTask armed the request's demand: level-triggered OFF
    TEST_ASSERT_TRUE(decideAutotune(false, true, false, true, true, false) == AutotuneAction::ASSERT_OFF);
    TEST_ASSERT_TRUE(decideAutotune(false, true, false, true, false, false) == AutotuneAction::NONE);
    TEST_ASSERT_TRUE(decideAutotune(false, true, true, true, false, false) == AutotuneAction::ASSERT_OFF);
    // Unknown demand state: write the desired state
    TEST_ASSERT_TRUE(decideAutotune(false, true, false, false, false, false) == AutotuneAction::ASSERT_OFF);
    TEST_ASSERT_TRUE(decideAutotune(true, true, false, false, true, true) == AutotuneAction::ASSERT_FULL);
}

void test_autotune_blocked_on_edge_is_retried() {
    DemandSlot slot = {false, 550, false};
    bool permitted = false;
    // Cycle 1: tuner ON edge while BurnerControlTask has revoked the permission
    AutotuneAction action = decideAutotune(true, permitted, true, true, slot.demand, slot.highPower);
    TEST_ASSERT_TRUE(action == AutotuneAction::ASSERT_OFF);
    TEST_ASSERT_TRUE(applyDemandWrite(slot, false, 550, false, permitted) != WriteResult::REFUSED);
    // Cycle 2: permitted again, decide says FULL but the permission is revoked before the write
    permitted = true;
    action = decideAutotune(true, permitted, false, true, slot.demand, slot.highPower);
    TEST_ASSERT_TRUE(action == AutotuneAction::ASSERT_FULL);
    TEST_ASSERT_TRUE(applyDemandWrite(slot, true, 550, true, false) == WriteResult::REFUSED);
    TEST_ASSERT_FALSE(slot.demand);
    // Cycle 3: no tuner edge, still retried
    action = decideAutotune(true, permitted, false, true, slot.demand, slot.highPower);
    TEST_ASSERT_TRUE(action == AutotuneAction::ASSERT_FULL);
    TEST_ASSERT_TRUE(applyDemandWrite(slot, true, 550, true, permitted) == WriteResult::UPDATED);
    TEST_ASSERT_TRUE(slot.demand);
    TEST_ASSERT_TRUE(slot.highPower);
    // Cycle 4: in line, nothing to do
    TEST_ASSERT_TRUE(decideAutotune(true, permitted, false, true, slot.demand, slot.highPower) == AutotuneAction::NONE);
}

void test_autotune_armed_but_not_high_reasserts_full() {
    // BurnerControlTask armed the request's (low) power while the tuner wants FULL
    TEST_ASSERT_TRUE(decideAutotune(true, true, false, true, true, false) == AutotuneAction::ASSERT_FULL);
    TEST_ASSERT_TRUE(decideAutotune(true, true, false, true, true, true) == AutotuneAction::NONE);
}

void test_autotune_not_permitted_disarms() {
    TEST_ASSERT_TRUE(decideAutotune(true, false, false, true, true, true) == AutotuneAction::ASSERT_OFF);
    TEST_ASSERT_TRUE(decideAutotune(true, false, false, true, false, false) == AutotuneAction::NONE);
}

// ===== Sensor fallback check and confirmation feed (review R4-1, R4-3) =====

void test_sensor_check_only_with_request_or_armed_demand() {
    TEST_ASSERT_FALSE(sensorFallbackCheckNeeded(false, false));
    TEST_ASSERT_TRUE(sensorFallbackCheckNeeded(true, false));
    TEST_ASSERT_TRUE(sensorFallbackCheckNeeded(false, true));
    TEST_ASSERT_FALSE(sensorStopDemand(false, true));   // stale armed demand, no mode request
    TEST_ASSERT_FALSE(sensorStopDemand(true, false));   // request not armed (e.g. coasting)
    TEST_ASSERT_TRUE(sensorStopDemand(true, true));
}

void test_sensor_stop_not_forced_after_normal_request_end() {
    // Review R4-1: the room sensor drops out while heating. HeatingControlTask drops its
    // request on the next read, BurnerControlTask disarms, the burner post-purges. The
    // fallback stays SHUTDOWN. The former feed (lastHeatDemand, never cleared on the
    // fallback early return) stopped the burner with ERROR 10 s later.
    SensorFailureConfirm::State fixedFeed;
    SensorFailureConfirm::State formerFeed;
    bool fixedStopped = false;
    bool formerStopped = false;
    for (uint32_t t = 1000; t <= 1000 + SensorFailureConfirm::CONFIRM_MS * 2; t += 2500) {
        const bool first = (t == 1000);
        const bool modeRequested = first;
        const bool demandArmed = first;
        if (sensorFallbackCheckNeeded(modeRequested, demandArmed)) {
            fixedStopped |= SensorFailureConfirm::shouldStop(fixedFeed, false,
                                                             sensorStopDemand(modeRequested, demandArmed), t);
        } else {
            fixedFeed = SensorFailureConfirm::State();
        }
        formerStopped |= SensorFailureConfirm::shouldStop(formerFeed, false, true, t);
    }
    TEST_ASSERT_FALSE(fixedStopped);
    TEST_ASSERT_TRUE(formerStopped);
}

void test_sensor_stop_still_fires_with_persistent_demand() {
    // Request and armed demand stay while the required sensors stay missing
    SensorFailureConfirm::State feed;
    bool stoppedAt = false;
    uint32_t t = 1000;
    for (; t <= 1000 + SensorFailureConfirm::CONFIRM_MS; t += 2500) {
        TEST_ASSERT_TRUE(sensorFallbackCheckNeeded(true, true));
        if (SensorFailureConfirm::shouldStop(feed, false, sensorStopDemand(true, true), t)) {
            stoppedAt = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(stoppedAt);
    TEST_ASSERT_EQUAL_UINT32(1000 + SensorFailureConfirm::CONFIRM_MS, t);
}
