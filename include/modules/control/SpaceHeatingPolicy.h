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

    /**
     * @brief Weather mode: has the room reached the overheat backstop (Begrenzung)?
     *
     * ASYMMETRIC, and the asymmetry is the part that misleads readers. The stop test, used
     * while heating is ON, is `room > target + margin`. The restart test, used while heating
     * is OFF, is `room >= target + margin - hysteresis`. So while heating is off this
     * predicate does NOT mean "the room is too warm" - it means "the room has not yet fallen
     * far enough to restart", which is why a room merely sitting in the deadband satisfies it
     * (2026-10-18). Extracted from HeatingControlTask unchanged.
     */
    inline bool roomAtBackstop(int16_t roomTenths, int16_t targetTenths, int16_t marginTenths,
                               int16_t hysteresisTenths, bool currentlyHeating) {
        const int32_t limit = static_cast<int32_t>(targetTenths) + marginTenths;
        return currentlyHeating ? (static_cast<int32_t>(roomTenths) > limit)
                                : (static_cast<int32_t>(roomTenths) >= limit - hysteresisTenths);
    }

    /// Why weather mode is withholding demand. Mapped to HeatingControlDiagnostics::Block by
    /// the caller; kept local so this header stays dependency-free and natively testable.
    enum class WeatherBlock : uint8_t {
        NONE,           // heating is wanted
        OUTSIDE,        // the outside gate is shut
        ROOM_BACKSTOP,  // the room is at or above the backstop
        ROOM_UNKNOWN    // no usable room reading, so the backstop cannot be evaluated
    };

    struct WeatherDecision {
        bool heat;
        WeatherBlock block;
    };

    /**
     * @brief The whole weather-mode ON/OFF decision.
     *
     * WHY `roomIsMeasurement` IS A SEPARATE INPUT (2026-10-25)
     *
     * Before this existed, the backstop was computed only when the room reading was valid,
     * and `roomAtBackstop` defaulted to false otherwise - so losing the room sensor DELETED
     * the only term that was holding heating off and turned the burner ON. Demonstrated live:
     * swapping the room sensor at 00:44 UTC fired the burner for two minutes, with the room
     * sitting 0.1 K inside the backstop at the time. Meanwhile an invalid OUTSIDE reading
     * correctly disabled heating. A dead outside sensor failed safe; a dead room sensor
     * failed open.
     *
     * The caller must pass `roomIsMeasurement` from RoomTempFallbackPolicy::isMeasurement(),
     * i.e. the reading is fresh OR inside the hold window - NOT merely "we have a number".
     * The curve's TARGET substitution is deliberately not good enough here: substituting
     * "room = target" puts the room below the restart limit, which licenses heating, which is
     * the very fail-open this function closes.
     *
     * Blocking is the safe direction because frost protection is a separate path that
     * overrides the heating switch, and because TemperatureSensorFallback already escalates a
     * persistently invalid room sensor to SHUTDOWN - so this aligns the weather decision with
     * what the layer below it was going to do anyway.
     *
     * Reason order is deliberate: the outside gate is reported FIRST, because it is the
     * primary weather-mode decision and reporting a room limit at 20 C outside invites the
     * user to adjust a margin that is not the cause (2026-10-18).
     */
    inline WeatherDecision weatherDecision(bool outsideCold, bool roomTargetConfigured,
                                           bool roomIsMeasurement, bool roomAtBackstopNow) {
        if (!outsideCold) {
            return WeatherDecision{false, WeatherBlock::OUTSIDE};
        }
        // With no room target configured there is no backstop to evaluate and weather-only
        // control is the intended mode, so a missing room reading is not a reason to stop.
        if (roomTargetConfigured) {
            if (!roomIsMeasurement) {
                return WeatherDecision{false, WeatherBlock::ROOM_UNKNOWN};
            }
            if (roomAtBackstopNow) {
                return WeatherDecision{false, WeatherBlock::ROOM_BACKSTOP};
            }
        }
        return WeatherDecision{true, WeatherBlock::NONE};
    }

} // namespace SpaceHeatingPolicy

#endif // SPACE_HEATING_POLICY_H
