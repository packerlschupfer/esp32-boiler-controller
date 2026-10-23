// include/modules/control/RoomTempFallbackPolicy.h
#ifndef ROOM_TEMP_FALLBACK_POLICY_H
#define ROOM_TEMP_FALLBACK_POLICY_H

#include "shared/Temperature.h"
#include "modules/control/HeatingCurve.h"

/**
 * @brief What room temperature does the heating curve use when the room sensor is invalid?
 *
 * Header-only and natively testable, like BurnerHysteresisPolicy and SpaceHeatingPolicy.
 *
 * WHY THIS EXISTS (2026-10-23)
 *
 * The curve needs a room temperature as its base, and before 2026-10-23 it was handed
 * TEMP_INVALID (INT16_MIN) straight through whenever the sensor was marked invalid, which
 * overflowed the polynomial - see HeatingCurve.h. The first fix substituted
 * targetTemperatureInside, falling back to a nominal 20.0. That is better than garbage but
 * it is still a guess: the sensor fails for a few seconds at a time (a Modbus device on a
 * shared bus), and in those seconds the room temperature is known almost exactly, because
 * it was just measured.
 *
 * So the substitution order is now:
 *
 *   1. MEASURED - the reading is valid. Use it, and remember it.
 *   2. HELD     - invalid, but a reading from within MAX_HOLD_MS is remembered. Use that.
 *                 For a dropout of a few seconds this reproduces the previous cycle's
 *                 curve output exactly, so a glitch perturbs the flow setpoint by nothing
 *                 at all rather than by the 0.2 C the target substitution cost.
 *   3. TARGET   - nothing fresh to hold. Assume the room is AT target, which is the same
 *                 assumption the skipped room-deviation shift makes, so the curve falls
 *                 back to its nominal line for the current weather.
 *   4. NOMINAL  - not even a room target configured. HeatingCurve::NOMINAL_ROOM_TENTHS.
 *
 * WHY THE HOLD EXPIRES. A held reading is a measurement with an age, and the room moves.
 * Measured on 2026-10-23: the room rose 0.6 C over a 44-minute burn, about 0.014 C/min, and
 * it drifts down a little faster than that overnight. Ten minutes of hold is therefore worth
 * at most a few tenths of error, well inside the ~1.2 C that assuming "room = target" can be
 * wrong by. Past that the comparison flips: a two-hour-old reading keeps biasing the curve
 * on evidence that is no longer evidence, with no room feedback to notice, so the nominal
 * line becomes the better guess. In practice the window rarely matters - with the room
 * sensor required in SPACE_HEATING, TemperatureSensorFallback escalates to SHUTDOWN while a
 * dropout is still tens of seconds old - so the hold is doing its work in the first seconds,
 * which is exactly the failure that actually occurs.
 *
 * WHY A HELD READING STILL GETS THE ROOM-DEVIATION SHIFT. The caller applies the weather
 * mode's Parallelverschiebung from (target - inside) whenever the source is MEASURED or
 * HELD, and skips it for TARGET and NOMINAL (where the deviation is zero by construction
 * anyway). Without that, a dropout would change the curve output even though the base
 * temperature was unchanged - the whole point of holding it is that nothing changes.
 */
namespace RoomTempFallbackPolicy {

    /// How long a measured reading may stand in for a missing one. See the header comment
    /// for the drift arithmetic behind the number.
    constexpr uint32_t MAX_HOLD_MS = 600000;  // 10 minutes

    /// Where the room temperature the curve used came from. Published as `isrc` on
    /// boiler/status/heating/control, because a held value is indistinguishable from a
    /// measured one in the `i` field and that would be a new blind spot.
    enum class Source : uint8_t {
        MEASURED,  // the sensor
        HELD,      // the sensor, up to MAX_HOLD_MS ago
        TARGET,    // heating/targetTemperatureInside
        NOMINAL    // HeatingCurve::NOMINAL_ROOM_TENTHS
    };

    /// The last valid reading. Owned by the caller so this header stays stateless.
    struct Sample {
        Temperature_t value = TEMP_INVALID;
        uint32_t atMs = 0;
        bool valid = false;
    };

    struct Substitute {
        Temperature_t value;
        Source source;
    };

    inline void record(Sample& sample, Temperature_t measured, uint32_t nowMs) {
        sample.value = measured;
        sample.atMs = nowMs;
        sample.valid = true;
    }

    /**
     * @brief Is the remembered reading young enough to stand in?
     *
     * The subtraction is deliberately done in uint32_t: millis() rolls over every 49.7 days
     * and `nowMs - atMs` stays correct across the rollover, where `atMs + MAX_HOLD_MS` would
     * not. A rollover mid-dropout is otherwise a once-per-50-days chance to use a
     * 49-day-old room temperature.
     */
    inline bool holdUsable(const Sample& sample, uint32_t nowMs) {
        return sample.valid && (nowMs - sample.atMs) <= MAX_HOLD_MS;
    }

    /**
     * @param sample     the caller's remembered reading
     * @param nowMs      millis()
     * @param roomTarget heating/targetTemperatureInside, tenths; <= 0 means unset
     */
    inline Substitute substitute(const Sample& sample, uint32_t nowMs, Temperature_t roomTarget) {
        if (holdUsable(sample, nowMs)) {
            Substitute s = {sample.value, Source::HELD};
            return s;
        }
        if (roomTarget > 0) {
            Substitute s = {roomTarget, Source::TARGET};
            return s;
        }
        Substitute s = {HeatingCurve::NOMINAL_ROOM_TENTHS, Source::NOMINAL};
        return s;
    }

    /// True for the two sources that are an actual room measurement, which is what decides
    /// whether the room-deviation curve shift applies.
    inline bool isMeasurement(Source source) {
        return source == Source::MEASURED || source == Source::HELD;
    }

    inline const char* toString(Source source) {
        switch (source) {
            case Source::MEASURED: return "meas";
            case Source::HELD:     return "held";
            case Source::TARGET:   return "tgt";
            case Source::NOMINAL:  return "nom";
        }
        return "unknown";
    }

} // namespace RoomTempFallbackPolicy

#endif // ROOM_TEMP_FALLBACK_POLICY_H
