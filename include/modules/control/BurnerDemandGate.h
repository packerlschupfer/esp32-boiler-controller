// include/modules/control/BurnerDemandGate.h
#ifndef BURNER_DEMAND_GATE_H
#define BURNER_DEMAND_GATE_H

#include <cstdint>

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
 *   passed) and writes OFF itself when it withdraws it. It arms ON for a new or
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

    /**
     * @brief May BurnerControlTask arm the demand for a new or changed request?
     *
     * Follows BoilerTempControlTask's latest decision when it is fresh and was made
     * for (about) this target; otherwise the boiler must be below target, which is
     * also what the PID decides after a reset. Without a valid boiler temperature
     * the PID does not run, so BurnerControlTask keeps arming as before (sensor
     * fallback operation).
     */
    inline bool controlTaskMayArm(bool boilerTempValid, int16_t boilerTemp, int16_t target,
                                  bool decisionFresh, int16_t decisionTarget, bool decisionOn) {
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
        return boilerTemp < target;
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
