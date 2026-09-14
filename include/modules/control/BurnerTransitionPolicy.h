// include/modules/control/BurnerTransitionPolicy.h
#ifndef BURNER_TRANSITION_POLICY_H
#define BURNER_TRANSITION_POLICY_H

#include <cstdint>

/**
 * @brief Pure decision rules for burner state transitions (Stage A, 2026-09-14).
 *
 * Header-only and FreeRTOS-free so they can be unit tested natively; the
 * BurnerStateMachine gathers the inputs and performs the actions.
 */
namespace BurnerTransitionPolicy {

    // HeatingControlTask evaluates every 5 s, so a water -> heating handover may
    // take up to ~5 s before HEATING_ON and the heating request appear.
    constexpr uint32_t MODE_SWITCH_MAX_WAIT_MS = 15000;

    /**
     * @brief Stop immediately (bypassing the minimum on-time) on explicit disable.
     *
     * The *_ENABLED bits are only cleared by explicit commands (MQTT/UI through
     * StateManager, ControlTask) and restored at boot - never transiently - so
     * this cannot cause short cycling by itself. Only the running mode counts:
     * disabling water while heating runs does not stop the burner.
     */
    inline bool stopForExplicitDisable(bool runningModeIsWater, bool boilerEnabled,
                                       bool heatingEnabled, bool waterEnabled) {
        if (!boilerEnabled) {
            return true;
        }
        return runningModeIsWater ? !waterEnabled : !heatingEnabled;
    }

    /**
     * @brief Will HeatingControlTask request the burner soon?
     *
     * Mirrors HeatingControlTask::checkIfSpaceHeatingNeededEvent() for the turn-on
     * case. Temperatures in tenths of °C.
     */
    inline bool heatingLikelyWanted(bool heatingEnabled, bool heatingOverrideOff,
                                    bool useWeatherCompensation,
                                    bool outsideValid, int16_t outsideTemp, int16_t outsideThreshold,
                                    bool roomValid, int16_t roomTemp, int16_t roomTarget,
                                    int16_t roomOverheatMargin) {
        if (!heatingEnabled || heatingOverrideOff) {
            return false;
        }
        if (useWeatherCompensation) {
            if (!outsideValid || outsideTemp >= outsideThreshold) {
                return false;
            }
            if (roomValid && roomTarget > 0 &&
                static_cast<int32_t>(roomTemp) > static_cast<int32_t>(roomTarget) + roomOverheatMargin) {
                return false;
            }
            return true;
        }
        return roomValid && roomTarget > 0 && roomTemp < roomTarget;
    }

    enum class ModeSwitchAction { WAIT, STOP };

    /**
     * @brief MODE_SWITCHING while the new mode has no burner request.
     *
     * Wait for the water -> heating handover only while heating is likely
     * wanted, and never longer than MODE_SWITCH_MAX_WAIT_MS. Previously the wait
     * only checked room < target, had no time limit, and kept the burner firing
     * (relays still in water mode) if heating never requested.
     */
    inline ModeSwitchAction onNoDemandForNewMode(bool newModeIsWater, bool heatingWanted,
                                                 uint32_t timeInStateMs) {
        if (newModeIsWater || !heatingWanted) {
            return ModeSwitchAction::STOP;
        }
        return (timeInStateMs < MODE_SWITCH_MAX_WAIT_MS) ? ModeSwitchAction::WAIT
                                                         : ModeSwitchAction::STOP;
    }

    enum class RevertAction { RESUME_RUNNING, WAIT, STOP };

    /**
     * @brief MODE_SWITCHING where the demand points back to the running mode.
     *
     * Resume only if the running mode's ON bit is set again. If it is not (e.g.
     * WATER_OFF_OVERRIDE cleared WATER_ON while the request is still set, or a
     * handover race), resuming would bounce RUNNING_LOW <-> MODE_SWITCHING every
     * tick; wait instead, bounded by MODE_SWITCH_MAX_WAIT_MS.
     */
    inline RevertAction onModeReverted(bool runningModeOnBitSet, uint32_t timeInStateMs) {
        if (runningModeOnBitSet) {
            return RevertAction::RESUME_RUNNING;
        }
        return (timeInStateMs < MODE_SWITCH_MAX_WAIT_MS) ? RevertAction::WAIT
                                                         : RevertAction::STOP;
    }

} // namespace BurnerTransitionPolicy

#endif // BURNER_TRANSITION_POLICY_H
