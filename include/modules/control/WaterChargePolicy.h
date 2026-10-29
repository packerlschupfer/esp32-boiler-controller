// include/modules/control/WaterChargePolicy.h
#ifndef WATER_CHARGE_POLICY_H
#define WATER_CHARGE_POLICY_H

#include <cstdint>
#include <cmath>

/**
 * @brief Tank charge limits (header-only, native-testable). Tenths of °C.
 *
 * WheaterControlTask starts a charge below tempLimitLow and stops above
 * tempLimitHigh. Both parameters are range-checked one at a time, so the pair can
 * be inverted - raising both limits sends low before high. On 2026-09-14 an
 * inverted pair (low 60.0, high 50.0) toggled water/heating every control cycle.
 * Such a pair is treated as invalid until it is consistent again.
 */
namespace WaterChargePolicy {

    inline bool limitsValid(int16_t tempLimitLow, int16_t tempLimitHigh) {
        return tempLimitLow > 0 && tempLimitHigh > 0 && tempLimitLow < tempLimitHigh;
    }

    /**
     * @brief Two-threshold charge latch: start below tempLimitLow, stop above tempLimitHigh.
     *
     * WheaterControlTask clears the latch whenever water heating is switched off
     * (water/boiler disabled, water OFF override). Otherwise re-enabling resumed an
     * interrupted charge up to tempLimitHigh although the tank was above
     * tempLimitLow (2026-09-14 18:12).
     */
    inline bool nextChargeNeeded(bool charging, int16_t tankTemp,
                                 int16_t tempLimitLow, int16_t tempLimitHigh) {
        return charging ? !(tankTemp > tempLimitHigh) : (tankTemp < tempLimitLow);
    }

    /**
     * @brief Can a tank stop limit be reached at all, given the water flow ceiling?
     *
     * A running charge has FIVE exits and not one of them is time- or convergence-based:
     * tank above tempLimitHigh, the water OFF override, water disabled, boiler disabled, or
     * sensor loss. The 10-minute burner request watchdog cannot end it either, because
     * WheaterControlTask re-asserts the request every 5 minutes.
     *
     * So an unreachable stop limit is an INDEFINITE charge - and with WATER_PRIORITY on (the
     * live configuration) HeatingControlTask returns before frost demand or the room backstop
     * is acted on, and a frost burn already running is stopped to yield to water. An
     * unreachable tank limit therefore locks out space heating AND frost protection for as
     * long as it stands.
     *
     * No hardware fault is needed to arrange this. There is no cross-field check between
     * wheater/tempLimitHigh (registered 500-850) and w/hHi (600-1000), and the flow is capped
     * by calculateBoilerTarget's clamp to w/hHi plus the w/bandOff OFF band - so w/hHi 60.0
     * with tempLimitHigh 65.0 is accepted today and simply never finishes. Found by the
     * 2026-10-28 review.
     *
     * The tank is heated by a coil, so the flow must sit ABOVE the tank temperature for
     * transfer to happen at all - by the same physical margin the delta bounds describe
     * below. Requiring the flow ceiling to clear the tank stop limit by that margin is the
     * weakest check that still rules out "cannot converge".
     *
     * @param tempLimitHigh tank stop limit, tenths
     * @param waterFlowHigh w/hHi, the water flow ceiling, tenths
     * @param minDelta      the smallest useful flow-above-tank head, tenths
     */
    inline bool tankLimitReachable(int16_t tempLimitHigh, int16_t waterFlowHigh,
                                   int16_t minDelta) {
        return static_cast<int32_t>(waterFlowHigh) >=
               static_cast<int32_t>(tempLimitHigh) + minDelta;
    }

    /**
     * @brief Hard cap on a single charge, so an unconvergeable one cannot lock out frost.
     *
     * The reachability check above rules out the CONFIGURATION case. This bounds the physical
     * one, which cannot be ruled out from parameters: a coil fouled with scale, a failed tank
     * sensor reading low, or the boiler's own thermostat cutting the flame below the flow the
     * curve asked for (suspected at 62-63 C on this boiler but NOT established - two burns,
     * two undistinguished hypotheses, and a 2026-09-12/13 run to 90 C with the flame on
     * argues against it).
     *
     * Generous rather than tight: a full charge from the low limit measured about 40 minutes,
     * so three hours is far beyond any legitimate charge and still bounds the lockout. The
     * point is that space heating and frost get the burner back, and that the operator is told,
     * rather than the number being exact.
     */
    constexpr uint32_t MAX_CHARGE_MS = 10800000;  // 3 h

    inline bool chargeOverran(uint32_t chargeElapsedMs,
                              uint32_t maxMs = MAX_CHARGE_MS) {
        return chargeElapsedMs >= maxMs;
    }

    /**
     * @brief Charge delta bounds: the head the boiler holds above the tank.
     *
     * The water burner target is tank + delta (WheaterControlTask::calculateBoilerTarget),
     * recomputed as the tank warms. The delta is what drives the tank coil, so it has a
     * real physical floor: coil transfer is UA x (flow - tank), and a 10.0 delta already
     * produces only a 3.0-3.5 K flow-return gap on this boiler (measured 2026-10-22 during
     * a full charge). Below ~5.0 the boiler would command tank + a few tenths and could not
     * charge the tank at all.
     *
     * These are the single source of truth. Before 2026-10-23 the bounds were written out
     * twice and disagreed: PersistentStorageTask registered the parameter as 1.0-20.0 while
     * this check rejected anything below 5.0 and silently substituted the default. A
     * params/set of 1.0-4.9 was therefore validated, acknowledged, persisted to NVS and
     * then ignored at the use site - the stored value and the running value disagreed with
     * no error anywhere. Registering with CHARGE_DELTA_MIN makes the storage layer reject
     * it instead (PersistentStorage.cpp returns ERROR_VALIDATION_FAILED out of range).
     */
    constexpr float CHARGE_DELTA_MIN     = 5.0f;
    constexpr float CHARGE_DELTA_MAX     = 20.0f;
    constexpr float CHARGE_DELTA_DEFAULT = 10.0f;

    inline bool chargeDeltaValid(float delta) {
        return std::isfinite(delta) &&
               delta >= CHARGE_DELTA_MIN && delta <= CHARGE_DELTA_MAX;
    }

    /**
     * @brief The delta to actually use. Falls back to the default when out of range.
     *
     * Defence in depth: the storage layer already rejects out-of-range writes, but
     * settings can also arrive from an NVS blob written by older firmware, so the use
     * sites still validate. Callers that want to log the substitution should test with
     * chargeDeltaValid() first.
     */
    inline float chargeDeltaOrDefault(float delta) {
        return chargeDeltaValid(delta) ? delta : CHARGE_DELTA_DEFAULT;
    }

} // namespace WaterChargePolicy

#endif // WATER_CHARGE_POLICY_H
