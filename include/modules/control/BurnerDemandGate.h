// include/modules/control/BurnerDemandGate.h
#ifndef BURNER_DEMAND_GATE_H
#define BURNER_DEMAND_GATE_H

#include <cstdint>
#include "shared/Temperature.h"  // tempSub/tempAbs for applyDemandWrite()

/**
 * @brief Rules for arming the burner heat demand (header-only, native-testable).
 *
 * Two tasks write BurnerStateMachine::setHeatDemand(): BurnerControlTask (request
 * start/change, safety blocks) and BoilerTempControlTask (PID power level).
 * BurnerControlTask armed ON on every request start without looking at the boiler
 * temperature, and BoilerTempControlTask only re-asserted OFF on its next cycle -
 * on 2026-09-14 17:12 that was 4 s later, after the 2 s pre-purge, so the burner
 * ignited at 64.4 °C against a 47.0 °C heating target and then ran the minimum
 * on-time.
 *
 * - BurnerControlTask publishes a Permission (request present, all of its blocks
 *   passed) and writes OFF itself when it withdraws it. The permission lives in
 *   BurnerStateMachine and setHeatDemand(true) checks it under demandMutex, so an
 *   arm that raced a revoke is refused or disarmed by the following OFF (review
 *   2026-09-14 bsm-6: a stale snapshot re-armed a revoked demand for one cycle). It arms ON for a new or
 *   changed request only when BoilerTempControlTask would (controlTaskMayArm), so
 *   a water <-> heating handover still gets its demand without waiting a PID cycle.
 * - BoilerTempControlTask keeps the demand equal to "permitted and PID wants heat"
 *   on every cycle (decide), not only when its output changes: a demand
 *   BurnerControlTask did not arm is armed, a re-armed demand is dropped.
 *
 * Temperatures in tenths of °C.
 */
namespace BurnerDemandGate {

    // BoilerTempControlTask runs on every boiler output sensor update (~2.5 s)
    constexpr uint32_t DECISION_MAX_AGE_MS = 6000;
    // BurnerControlTask re-evaluates when the request target moves by more than 1 °C
    constexpr int16_t DECISION_TARGET_TOLERANCE = 10;

    struct Permission {
        bool permitted;          // request present, sensors fresh, safety validation passed, no preheating
        bool highPowerAllowed;   // false while sensor fallback reduces the power factor
        int16_t maxTargetTemp;   // sensor fallback target cap, 0 = none
    };

    inline int16_t cappedTarget(int16_t requestTarget, int16_t maxTargetTemp) {
        return (maxTargetTemp > 0 && requestTarget > maxTargetTemp) ? maxTargetTemp : requestTarget;
    }

    // BoilerTempController::predictHeatDemand() result for the request's target
    enum class Prediction { UNKNOWN, OFF, HEAT };

    /**
     * @brief May BurnerControlTask arm the demand for a new or changed request?
     *
     * Follows BoilerTempControlTask's latest decision when it is fresh and was made
     * for (about) this target; otherwise the controller's prediction for its next
     * cycle (PID step and power mapping with hysteresis). "Boiler below target" is
     * only the fallback when no prediction is available: from OFF the PID turns on
     * only above 55 %, so in a band below target it armed while the PID stayed OFF
     * and the next cycle disarmed after ignition - a short start (review 2026-09-14
     * tests-4). Without a valid boiler temperature the PID does not run, so
     * BurnerControlTask keeps arming as before (sensor fallback operation).
     */
    inline bool controlTaskMayArm(bool boilerTempValid, int16_t boilerTemp, int16_t target,
                                  bool decisionFresh, int16_t decisionTarget, bool decisionOn,
                                  Prediction controllerPrediction) {
        if (!boilerTempValid) {
            return true;
        }
        if (decisionFresh) {
            int32_t diff = static_cast<int32_t>(decisionTarget) - static_cast<int32_t>(target);
            if (diff < 0) {
                diff = -diff;
            }
            if (diff <= DECISION_TARGET_TOLERANCE) {
                return decisionOn;
            }
        }
        if (controllerPrediction != Prediction::UNKNOWN) {
            return controllerPrediction == Prediction::HEAT;
        }
        return boilerTemp < target;
    }

    /**
     * @brief BurnerStateMachine::setHeatDemand(): may this demand be written now?
     *
     * Checked under demandMutex against the current permission, not a caller's
     * snapshot. OFF is always allowed.
     */
    inline bool demandWriteAllowed(bool demand, bool permitted) {
        return !demand || permitted;
    }

    // BurnerStateMachine's demand fields (heatDemand, targetTemperature, requestedHighPower)
    struct DemandSlot {
        bool demand;
        int16_t target;      // tenths of °C
        bool highPower;
    };

    enum class WriteResult { REFUSED, UNCHANGED, UPDATED };

    /**
     * @brief BurnerStateMachine::setHeatDemand() body, called under demandMutex
     *
     * The permission check and the write are one step (review 2026-09-14 bsm-6): with
     * `permitted` read under the same lock as the write, an arm that races a revoke is
     * refused here or overwritten by BurnerControlTask's following OFF. A target of 0
     * keeps the stored target; a change of 0.1 °C or less is not an update.
     */
    inline WriteResult applyDemandWrite(DemandSlot& slot, bool demand, int16_t target, bool highPower,
                                        bool permitted) {
        if (!demandWriteAllowed(demand, permitted)) {
            return WriteResult::REFUSED;
        }
        const bool demandChanged = (slot.demand != demand);
        const bool targetChanged = (target > 0 && tempAbs(tempSub(slot.target, target)) > 1);
        const bool powerChanged = (slot.highPower != highPower);
        if (!demandChanged && !targetChanged && !powerChanged) {
            return WriteResult::UNCHANGED;
        }
        slot.demand = demand;
        slot.highPower = highPower;
        if (target > 0) {
            slot.target = target;
        }
        return WriteResult::UPDATED;
    }

    /**
     * @brief BurnerControlTask::processTemperatureUpdate(): evaluate the sensor fallback on this read?
     *
     * Only while a mode request is active or the demand is armed (pass true for an unknown
     * demand state). In idle every single invalid boiler output reading flipped the
     * fallback NORMAL -> SHUTDOWN -> NORMAL with retained publishes and error bits (review
     * R4-3); a new request is checked by processBurnerRequest() before it can arm.
     */
    inline bool sensorFallbackCheckNeeded(bool modeRequested, bool demandArmed) {
        return modeRequested || demandArmed;
    }

    /**
     * @brief Heat demand for SensorFailureConfirm::shouldStop()
     *
     * The demand armed in the state machine AND a mode request: BurnerTransitions does not
     * start or keep running on an armed demand without a mode request. The former
     * burnerState.lastHeatDemand stayed true after a normal request end while the sensors
     * were failing and forced ERROR 10 s later (review R4-1).
     */
    inline bool sensorStopDemand(bool modeRequested, bool demandArmed) {
        return modeRequested && demandArmed;
    }

    enum class AutotuneAction { NONE, ASSERT_OFF, ASSERT_FULL };

    /**
     * @brief BoilerTempControlTask during autotune: bring the demand in line with the tuner
     *
     * Level-triggered like decide(), not only on tuner edges (review 2026-09-14 pid-3): a
     * demand armed elsewhere during an OFF phase is dropped, an ON edge that was refused
     * (permission revoked, safety validation) is retried, and an armed demand without high
     * power is raised to FULL. An unknown demand state (mutex timeout) writes the desired
     * state. ASSERT_FULL still requires the task's safety validation.
     */
    inline AutotuneAction decideAutotune(bool tunerWantsHeat, bool permitted, bool tunerOutputChanged,
                                         bool demandKnown, bool demandArmed, bool demandHighPower) {
        if (!tunerWantsHeat || !permitted) {
            return (tunerOutputChanged || !demandKnown || demandArmed) ? AutotuneAction::ASSERT_OFF
                                                                       : AutotuneAction::NONE;
        }
        return (tunerOutputChanged || !demandKnown || !demandArmed || !demandHighPower)
                   ? AutotuneAction::ASSERT_FULL
                   : AutotuneAction::NONE;
    }

    enum class Action { NONE, ARM, SET_POWER, DISARM };

    /**
     * @brief BoilerTempControlTask: bring the state machine demand in line.
     *
     * ARM and SET_POWER still require BoilerTempControlTask's safety validation.
     */
    inline Action decide(bool permitted, bool pidWantsHeat, bool pidOutputChanged, bool demandArmed) {
        if (permitted && pidWantsHeat) {
            if (!demandArmed) {
                return Action::ARM;
            }
            return pidOutputChanged ? Action::SET_POWER : Action::NONE;
        }
        return demandArmed ? Action::DISARM : Action::NONE;
    }

} // namespace BurnerDemandGate

#endif // BURNER_DEMAND_GATE_H
