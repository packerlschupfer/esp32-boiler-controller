// include/modules/control/BurnerHysteresisPolicy.h
#ifndef BURNER_HYSTERESIS_POLICY_H
#define BURNER_HYSTERESIS_POLICY_H

#include "shared/Temperature.h"

/**
 * @brief Flow-temperature dead band: may the burner be lit for this flow error?
 *
 * Header-only and natively testable, like BurnerBoostPolicy and SpaceHeatingPolicy.
 *
 * WHY THIS EXISTS (2026-10-23)
 *
 * This controller has two nested loops. The room loop decides whether heat is requested at
 * all; inside one request, the flow loop switches the burner on and off repeatedly. The
 * flow loop's hysteresis is what sets the burner start rate, and before this policy it was
 * specified in **percent of PID output** rather than in degrees:
 *
 *   output% = 50 + adjustment/10,  adjustment = Kp*error + Ki*integral  (clamped +/-500)
 *   < offThreshold (35 %) -> OFF,  > halfThreshold + hysteresis (55 %) -> stage 1
 *
 * A percent threshold only becomes a temperature after dividing by Kp, so with the live
 * space-heating gains (Kp 13.222, Ki 0.045334, Kd 0) pure proportional action gives:
 *
 *   OFF  at target +1.13 °C      (adjustment -150)
 *   ON   at target -0.38 °C      (adjustment  +50)
 *
 * a 1.51 K band. Measured on hardware it was 2.9-3.2 K: the burner shut off at target
 * +2.3, +2.4 and +2.4 on three independent cycles and restarted at target -0.5 to -0.8.
 * Kd is zero, so the extra width is the integral - each +100 of integral contribution
 * pushes the OFF point out by 100/Kp/10 = 0.76 °C, and the +167 needed to explain +2.40 is
 * only ~370 s of 1.0 °C error. Anti-windup bounds it (the integral freezes whenever output
 * pins at 0 or 100 %, PIDControlModuleFixedPoint's output limits being +/-500, exactly
 * where the power mapping saturates), but within those bounds it moves freely, and the PID
 * is NOT reset between burner cycles inside one request - only when a request ends
 * (BoilerPowerLevel::pidPaused). So the band's width and position depended on how long the
 * burner had been running. It was not a fixed band but a trajectory-dependent one.
 *
 * WHAT IT COST. 2026-10-22 17:39-18:19, outside 12.2-13.0, target pinned at the 38.0 floor:
 * a 32m20s burn ended at flow 40.4 (target +2.4), flow decayed 3.2 K in 3m50s, the burner
 * relit at 37.2 (target -0.8) and ran 3m20s before the ROOM loop ended the request at
 * 18:20. 0.11 m3 of gas and one extra ignition for a burn the house had not asked for -
 * the flow loop cycles far faster than the room loop that actually knows whether the room
 * is satisfied. The same pattern was measured on 2026-10-19 (4 starts in 46 min) and
 * 2026-10-20 (a 2m00s burn the room cut at 115 s, held to 120 s by MIN_ON_TIME_MS).
 *
 * WHY THE BAND AND NOT SOMETHING ELSE. For hysteresis control on a thermal system, on-time
 * is H*C/(P-L) and off-time H*C/L, so the cycle PERIOD is proportional to the band width H
 * while the DUTY is L/P and independent of it. Widening the band therefore trades burner
 * starts for flow swing at constant gas consumption - 2.9 K -> 8.0 K is about 2.7x fewer
 * starts for the same heat. The dials that do not work: MIN_ON_TIME_MS lengthens unwanted
 * burns rather than preventing them (2026-10-20, it held the burner on for the last 5 s of
 * a burn the room had already finished with); a minimum off-time only postpones the burn
 * while the house keeps cooling; h/sLo moves where the band sits, not how wide it is; and
 * lowering Kp widens the band only by detuning the loop's response.
 *
 * MODE AWARENESS. Space heating and a tank charge want different bands, so they get
 * separate parameters:
 *   - space heating: wide. The room is governed by the outer loop and the building time
 *     constant is hours against a band period of tens of minutes, so the room barely sees
 *     the flow swing. Default +5.0 / -3.0, the values that were already sitting unused in
 *     BoilerTempController::Config.
 *   - water: default +4.0 / -3.0, chosen to REPRODUCE measured behaviour rather than to
 *     change it. A DHW charge on 2026-10-22 stopped at target +3.3 and +5.2 and relit at
 *     target -2.9. Widening it would push peak flow higher, and the boiler's internal
 *     tandem thermostat dial is still unread (flow has reached 66.7 with no cut-out, so it
 *     is above ~67, but nothing bounds it above that). The DHW burst structure needs the
 *     charge target to stop chasing the tank, which is a separate change; do not try to fix
 *     it by widening this band until the dial is known.
 *
 * WHAT THIS DOES NOT CHANGE. The band decides only the ON/OFF bit. Stage selection (1 vs 2)
 * is still the PID's percent output through BoilerPowerLevel::fromPidOutput, still gated for
 * space heating by BurnerBoostPolicy (h/boostMin). The band is applied upstream of
 * BurnerAntiFlapping, so MIN_ON_TIME_MS and MIN_OFF_TIME_MS still bound every transition.
 * The 110 °C MAX_BOILER_TEMP_C interlock is untouched and remains the absolute backstop;
 * with heating_high_limit 75.0 and the largest permitted offAbove, commanded flow stays
 * well below it.
 */
namespace BurnerHysteresisPolicy {

    // Space heating: the values that were already in BoilerTempController::Config but were
    // read only by the bang-bang path and a boot log line.
    constexpr Temperature_t DEFAULT_HEATING_OFF_ABOVE = 50;   // +5.0 °C above target -> OFF
    constexpr Temperature_t DEFAULT_HEATING_ON_BELOW  = 30;   // -3.0 °C below target -> ON

    // Water: deliberately close to the measured behaviour of the percent band.
    constexpr Temperature_t DEFAULT_WATER_OFF_ABOVE   = 40;   // +4.0 °C above target -> OFF
    constexpr Temperature_t DEFAULT_WATER_ON_BELOW    = 30;   // -3.0 °C below target -> ON

    // Parameter bounds. The floor keeps a band from collapsing to zero (which would chatter
    // at every sensor tick); the ceiling keeps commanded flow clear of the safety interlock
    // even at heating_high_limit 75.0.
    constexpr Temperature_t MIN_BAND_EDGE = 5;    // 0.5 °C
    constexpr Temperature_t MAX_BAND_EDGE = 150;  // 15.0 °C

    struct Band {
        Temperature_t offAbove;  // turn OFF once flow reaches target + this
        Temperature_t onBelow;   // turn ON only once flow falls to target - this
    };

    inline Band heatingDefault() {
        Band b = {DEFAULT_HEATING_OFF_ABOVE, DEFAULT_HEATING_ON_BELOW};
        return b;
    }

    inline Band waterDefault() {
        Band b = {DEFAULT_WATER_OFF_ABOVE, DEFAULT_WATER_ON_BELOW};
        return b;
    }

    inline bool edgeValid(Temperature_t edge) {
        return edge >= MIN_BAND_EDGE && edge <= MAX_BAND_EDGE;
    }

    inline bool bandValid(const Band& band) {
        return edgeValid(band.offAbove) && edgeValid(band.onBelow);
    }

    /**
     * @brief Fall back to the mode's default if either edge is out of range.
     *
     * Falls back as a pair rather than clamping each edge: a half-clamped band is a band
     * nobody chose, and an out-of-range value is a mistake worth losing rather than
     * silently reinterpreting (same reasoning as WaterChargePolicy::chargeDeltaOrDefault).
     */
    inline Band bandOrDefault(const Band& band, bool isWaterMode) {
        if (bandValid(band)) {
            return band;
        }
        return isWaterMode ? waterDefault() : heatingDefault();
    }

    /**
     * @param currentlyOn  burner is lit right now. After a request pause the caller must
     *                     pass false: BoilerPowerLevel::modulatingCycleStart() restarts
     *                     every request from OFF, so the band must too, otherwise a new
     *                     request would inherit the previous one's "keep firing" branch.
     * @param flowTarget   commanded flow setpoint, tenths °C
     * @param flowActual   measured flow temperature, tenths °C
     * @param band         the active mode's band, already passed through bandOrDefault()
     * @return true if the burner may be lit
     *
     * Asymmetric by construction - that IS the hysteresis:
     *   lit     -> keep firing until flow reaches target + offAbove
     *   not lit -> relight only once flow has fallen to target - onBelow
     *
     * Arithmetic in int32: flowTarget can be 1100 (MAX_BOILER_TEMP_C) and MAX_BAND_EDGE
     * adds 150, which still fits int16, but the sum is computed wide so a future wider
     * bound cannot silently wrap.
     */
    inline bool burnerWanted(bool currentlyOn, Temperature_t flowTarget,
                             Temperature_t flowActual, const Band& band) {
        const int32_t target = static_cast<int32_t>(flowTarget);
        const int32_t actual = static_cast<int32_t>(flowActual);
        if (currentlyOn) {
            return actual < target + band.offAbove;
        }
        return actual <= target - band.onBelow;
    }

} // namespace BurnerHysteresisPolicy

#endif // BURNER_HYSTERESIS_POLICY_H
