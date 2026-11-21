// include/modules/control/CommandedSetpoint.h
#ifndef COMMANDED_SETPOINT_H
#define COMMANDED_SETPOINT_H

#include "shared/Temperature.h"
#include <cstdint>

/**
 * @brief What boiler/status/sensors publishes as t.bt ("Boiler Commanded Setpoint" in HA).
 *
 * Header-only and free of ESP headers so it can be tested natively.
 *
 * WHY THIS EXISTS (2026-11-21). t.bt used to be BurnerStateMachine's targetTemperature,
 * which is written only when the burner is ARMED or disarmed. During the 2026-11-20 06:55
 * tank charge the charge target rose 61 -> 63 -> 65 °C while the burner coasted, and t.bt
 * stayed at 61 - then stayed at 61 for another seven minutes after the charge had ended,
 * until space heating next armed the burner at 07:32:43. The chart showed a setpoint the
 * controller was not regulating to.
 *
 * The source is now the control loop's own decision (BoilerTempControlTask
 * publishDecision): the request target AFTER the sensor-fallback cap, recorded every cycle
 * whether the burner is firing or coasting. That is the number the dead band is applied to.
 *
 * When there is no request the last value is HELD rather than dropped. User decision: a
 * missing key makes the HA template's `| default(0)` chart 0 °C, which wrecks the chart's
 * scale; a held value is the lesser distortion and is how the old behaviour looked at idle.
 */
namespace CommandedSetpoint {

    /// A decision older than this is not "the current target" any more (the loop runs on
    /// the 2.5 s MB8ART cadence; this allows several missed cycles).
    constexpr uint32_t MAX_DECISION_AGE_MS = 15000;

    inline bool isUsableTarget(Temperature_t t) {
        return t > 0 && t != TEMP_INVALID;
    }

    /**
     * @param haveDecision   getBoilerTempDecision() returned true
     * @param decisionTarget its target (0 when no burner request is active)
     * @param decisionAgeMs  its age
     * @param held           in/out: the last value published; updated when a live target exists
     * @param haveHeld       in/out: whether `held` has ever been set
     * @return true if t.bt should be published, with the value in `held`
     */
    inline bool select(bool haveDecision, Temperature_t decisionTarget, uint32_t decisionAgeMs,
                       Temperature_t& held, bool& haveHeld) {
        if (haveDecision && decisionAgeMs <= MAX_DECISION_AGE_MS && isUsableTarget(decisionTarget)) {
            held = decisionTarget;
            haveHeld = true;
        }
        return haveHeld;
    }

}  // namespace CommandedSetpoint

#endif  // COMMANDED_SETPOINT_H
