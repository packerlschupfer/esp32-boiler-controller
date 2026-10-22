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
