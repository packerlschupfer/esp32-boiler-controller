// include/modules/control/SpaceHeatingPolicy.h
#ifndef SPACE_HEATING_POLICY_H
#define SPACE_HEATING_POLICY_H

#include <cstdint>

/**
 * @brief Space heating decision rules (header-only, native-testable).
 */
namespace SpaceHeatingPolicy {

    // Weather mode stops heating only this far above heating/outsideThreshold (tenths °C)
    constexpr int16_t OUTSIDE_THRESHOLD_HYSTERESIS = 10;  // 1.0 °C

    /**
     * @brief Weather mode: is it cold enough outside for heating?
     *
     * Starts below the threshold and stops only at threshold + hysteresis. Without the
     * hysteresis, outside readings hovering around the threshold switched heating (and the
     * heating pump and burner request) on and off every 5 s cycle (2026-09-15).
     * BurnerTransitionPolicy::heatingLikelyWanted() uses the start rule.
     */
    inline bool outsideColdForHeating(int16_t outsideTenths, int16_t thresholdTenths, bool currentlyHeating) {
        const int32_t limit = currentlyHeating
                                  ? static_cast<int32_t>(thresholdTenths) + OUTSIDE_THRESHOLD_HYSTERESIS
                                  : static_cast<int32_t>(thresholdTenths);
        return static_cast<int32_t>(outsideTenths) < limit;
    }

} // namespace SpaceHeatingPolicy

#endif // SPACE_HEATING_POLICY_H
