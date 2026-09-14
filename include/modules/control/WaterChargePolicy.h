// include/modules/control/WaterChargePolicy.h
#ifndef WATER_CHARGE_POLICY_H
#define WATER_CHARGE_POLICY_H

#include <cstdint>

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

} // namespace WaterChargePolicy

#endif // WATER_CHARGE_POLICY_H
