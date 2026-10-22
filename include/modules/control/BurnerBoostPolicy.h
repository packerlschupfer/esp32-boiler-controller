// include/modules/control/BurnerBoostPolicy.h
#ifndef BURNER_BOOST_POLICY_H
#define BURNER_BOOST_POLICY_H

#include "shared/Temperature.h"

/**
 * @brief May stage 2 (POWER_BOOST) be used for this demand?
 *
 * Header-only and natively testable, like SpaceHeatingPolicy and RelayCommandPolicy.
 *
 * WHY THIS EXISTS (2026-10-19; rationale corrected 2026-10-20 against measured data - see
 * "WHAT THIS DOES NOT DO" below, which replaces an earlier claim in this comment).
 *
 * On a space-heating cold start the radiator circuit sits at room temperature, so the first
 * flow reading after the pump starts is 20-27 °C against a target that in the shoulder
 * season is only 38-46 °C. That 10-20 °C deficit saturates the power selector, and it does
 * so on either of the two selectors this controller can run:
 *   - boiler/pidEnabled true - the live configuration - gives BurnerType::MODULATING
 *     (BoilerTempController.cpp:94), so calculateModulating() -> fromPidOutput() asks for
 *     FULL whenever the PID output exceeds fullThreshold (75 %);
 *   - boiler/pidEnabled false gives BurnerType::TWO_STAGE, so calculateBangBang() ->
 *     fromBangBangError() asks for FULL above fullPowerThreshold (10.0 °C).
 * This policy is deliberately indifferent to which one is active: it is applied in
 * BoilerTempControlTask at the single point where ControlOutput::powerLevel == FULL becomes
 * a burner request, downstream of both.
 *
 * Measured 2026-10-19, outside 11.5 °C, flow target 38.0 (clamped at h/sLo):
 *
 *   19:55:57  RUNNING_LOW   ignition, flow 35.7 and falling as the cold circuit mixes in
 *   19:56:17  RUNNING_HIGH  <- stage 2, 20 s after ignition, flow 29.3
 *   20:07:17  RUNNING_LOW   11 minutes later, flow 38.9
 *
 * 11 minutes of full power to chase a 38 °C setpoint on a mild autumn evening, plus a
 * second 1.5-minute burst at 20:35. The house needs single-digit kW at 11 °C outside and
 * stage 1 alone is ~19 kW (measured; see POWER FIGURES below).
 *
 * WHAT THIS DOES NOT DO. An earlier version of this comment said stage 2 overshoots the
 * setpoint, trips the -5.0 °C off-hysteresis and ends the cycle early, producing that
 * evening's 4 starts in 46 minutes. The recorder data refutes all three parts. Boost was
 * released at flow 38.9, i.e. 0.9 °C *past* a 38.0 target, and peak flow for the whole
 * cycle was 40.3 - the -5.0 °C band would need 43.0 and was never approached. On the live
 * PID path that band is not even in use. The four burner-offs happened at flow 40.3, 39.4,
 * 39.3 and 40.9, which is the PID's own off threshold (35 %, empirically target +2.3 to
 * +2.9), against a target pinned on the 38.0 floor; the burns were held to about 3 minutes
 * by SystemConstants::Burner::MIN_ON_TIME_MS (120 s). None of that is a power-stage effect
 * and the lock does not change any of it. Confirmed 2026-10-20 09:46-10:30 with the lock
 * live: a 37m10s burn still ended flow-driven at 43.4 against a 41.0 target, a 3m40s stop
 * followed, and the 2m00s restart that followed that was itself cut short - the room
 * crossed its limit at 10:29:50 and MIN_ON_TIME_MS held the burner on to 10:29:55.
 *
 * WHAT IT IS WORTH, then: ~34 kW fired into 20.8 °C return water is the harshest thermal
 * moment of the day, and full fire on an atmospheric boiler is audible. The lock buys
 * heat-exchanger stress and noise. It does not buy gas - the house needs the same heat
 * either way - and it does not buy fewer starts, which are set by the PID band, MIN_ON_TIME
 * and the h/sLo floor.
 *
 * POWER FIGURES (measured 2026-10-23, corrected from nameplate). This boiler is set to the
 * lowest gas pressure its valve allows, so the nameplate 23.3 / 42.2 kW are upper bounds.
 * Ten days of house gas meter against burner power level, 22.3 h of firing:
 * stage 1 = 2.065 m3/h, stage 2 = 3.678 m3/h, burner-off = 0.001 m3/h (no standing pilot).
 * At Hi ~10.2 kWh/m3 that is roughly 19 kW and 34 kW of output, both ~82 % of nameplate.
 * Earlier revisions of this comment quoted 42.2 and 23.3 as delivered power; they are not.
 * See docs/EQUIPMENT_SPECS.md "Measured Gas Throughput".
 *
 * MEASURED COST, 2026-10-20 00:00-06:35 with the lock live, outside 5.9-7.6 °C, targets
 * 44.0-46.0: two burns of 40m50s and 49m00s, stage 1 only, both ending on "Room warm
 * enough" rather than on flow, ramping at 0.42 °C/min. Burn 1 never reached its 46.0
 * setpoint at all - it stopped at 43.3. So between h/sLo and h/boostMin stage 1 can
 * under-deliver on the flow setpoint. That is harmless while the room is still satisfied,
 * and above h/boostMin stage 2 returns on its own.
 *
 * Stage 2 is NOT gated away where it earns its keep:
 *   - water (tank charge): the tank is a real heat sink, a fast charge is the point, and
 *     the charge target is tank + 10 °C rather than a weather curve. Verified live with the
 *     lock active, 2026-10-20: RUNNING_HIGH in Water mode at 07:08:45-07:10:35 and again
 *     from 07:21:45, while every Heating burn that day stayed on stage 1;
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
     * Denying is the fail-safe direction: the burner still runs at stage 1 (~19 kW measured),
     * so an invalid or zero target loses boost rather than gaining it.
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
