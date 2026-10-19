// include/modules/control/BurnerBoostPolicy.h
#ifndef BURNER_BOOST_POLICY_H
#define BURNER_BOOST_POLICY_H

#include "shared/Temperature.h"

/**
 * @brief May stage 2 (POWER_BOOST, full 42.2 kW) be used for this demand?
 *
 * Header-only and natively testable, like SpaceHeatingPolicy and RelayCommandPolicy.
 *
 * WHY THIS EXISTS (2026-10-19). BoilerTempController runs a three-point bang-bang on
 * (flow target - flow actual) and asks for FULL power whenever that error exceeds
 * fullPowerThreshold (10.0 °C, BoilerPowerLevel.h). On a space-heating cold start the
 * error always exceeds it: the radiator circuit sits at room temperature, so the first
 * reading after the pump starts is 20-26 °C against a target that in the shoulder season
 * is only 38-40 °C. Measured on 2026-10-19, outside 11.5 °C, flow target 38.0:
 *
 *   19:55:57  RUNNING_LOW   ignition
 *   19:56:17  RUNNING_HIGH  <- 42.2 kW, 20 s after ignition
 *   20:07:17  RUNNING_LOW   11 minutes later
 *
 * 11 minutes of full power to chase a 38 °C setpoint on a mild autumn evening. The house
 * needs single-digit kW at 11 °C outside; stage 1 alone is 23.3 kW. The extra 19 kW cannot
 * leave through the emitters at that flow temperature, so it goes into the boiler's 12 L of
 * water instead, overshoots the setpoint, trips the -5.0 °C off-hysteresis, and the cycle
 * ends early. That is the 4-starts-in-46-minutes pattern seen the same evening.
 *
 * Stage 2 is NOT gated away where it earns its keep:
 *   - water (tank charge): the tank is a real heat sink, a fast charge is the point, and
 *     the charge target is tank + 10 °C rather than a weather curve;
 *   - autotuning: PIDAutoTuner needs a full-power step to produce usable oscillations
 *     (BoilerTempController.cpp:821 sets FULL deliberately);
 *   - deep winter: a high flow target means the emitters can actually absorb stage 2.
 *
 * The gate is the flow TARGET rather than the outside temperature, although the user's
 * framing was "depending on outside temp". The target is the better discriminator and
 * needs no extra input:
 *   - it already contains the outside temperature, through the heating curve;
 *   - it already contains the room deficit, through the room-deviation shift (2.0 °C of
 *     flow per °C of room error), so a cold-morning recovery raises the target and earns
 *     boost back automatically - an outside-temperature gate would deny it;
 *   - it survives sensor fallback, which clamps the target, instead of depending on the
 *     outside reading that fallback has just declared untrustworthy.
 * With the curve as of 2026-10-19 (coeff 1.40, shift 4.4) a 50.0 °C target corresponds to
 * roughly +4 °C outside with the room at target, so the default really is a shoulder-season
 * lock, expressed in the controller's own currency.
 */
namespace BurnerBoostPolicy {

    /**
     * @param isWaterMode     this demand is a hot-water charge
     * @param isAutoTuning    the PID autotuner is driving the burner
     * @param flowTarget      commanded flow setpoint, tenths °C
     * @param minTargetForFull  h/boostMin, tenths °C: the lowest flow target that may use
     *                        stage 2. At or below h/sLo every heating target qualifies, so
     *                        the gate is effectively off; at h/sHi it is effectively always
     *                        on. No magic sentinel - both degenerate cases are just ends of
     *                        the parameter's range.
     * @return true if stage 2 may be requested
     *
     * Denying is the fail-safe direction: the burner still runs at stage 1 (23.3 kW), so an
     * invalid or zero target loses boost rather than gaining it.
     */
    inline bool fullPowerAllowed(bool isWaterMode, bool isAutoTuning,
                                 Temperature_t flowTarget, Temperature_t minTargetForFull) {
        if (isWaterMode) {
            return true;
        }
        if (isAutoTuning) {
            return true;
        }
        return flowTarget >= minTargetForFull;
    }

} // namespace BurnerBoostPolicy

#endif // BURNER_BOOST_POLICY_H
