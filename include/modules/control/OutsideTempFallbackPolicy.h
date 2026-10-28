// include/modules/control/OutsideTempFallbackPolicy.h
#ifndef OUTSIDE_TEMP_FALLBACK_POLICY_H
#define OUTSIDE_TEMP_FALLBACK_POLICY_H

#include <stdint.h>
#include "shared/Temperature.h"

/**
 * @brief What to do about space heating when the OUTSIDE reading is gone.
 *
 * WHY THIS EXISTS (2026-10-28)
 *
 * In weather mode the entire heating decision sat under `if (isOutsideTempValid)`. The else
 * branch set `heatingNeeded = false` with no hold, no time bound and no alternative - so one
 * open PT1000 lead on the weather-exposed sensor stopped space heating COMPLETELY, for as long
 * as the hardware fault lasted, in an occupied house, while a perfectly healthy room sensor and
 * a valid room target sat unused two branches away. A running burn was stopped at the next 5 s
 * tick and logged as "target temperature reached". The only remaining automatic heat was frost
 * protection at 5.0 C to the flow floor, releasing at 10.0 C, so the house settled in a
 * 5-10 C band. `sensor_mode` stayed NORMAL throughout, because the sensor fallback does not
 * require the outside channel for SPACE_HEATING, and nothing else alerted.
 *
 * That was documented as "weather mode fails safe". Failing safe for a BOILER means not firing
 * when you cannot tell whether it is safe to fire. It does not mean abandoning an occupied
 * house in winter when you still know the room temperature and the room target - which is
 * exactly the information a plain room thermostat runs on, and which this controller already
 * has a complete branch for.
 *
 * So there are two fallbacks, in order:
 *
 *   1. HELD - outside temperature is the slowest-moving input in the system. The MB8ART
 *      resumes validity on the first in-range read, so a dropout is usually seconds. Holding
 *      the last good value for up to MAX_HOLD_MS lets the weather curve keep working through
 *      a bus glitch with no change in behaviour at all. The hold is much longer than the room's
 *      10 minutes BECAUSE outside temperature moves slowly: a 60-minute-old reading on a winter
 *      night is off by a degree or two, which the curve and the room backstop both absorb. A
 *      60-minute-old ROOM reading would be worthless, which is why that hold is short.
 *
 *   2. ROOM_THERMOSTAT - past the hold, fall back to the room branch rather than to no heat.
 *      Requires a real room MEASUREMENT and a configured room target; without either there is
 *      genuinely nothing to control on and heating stays off.
 *
 * Pure and header-only so the native tests cover it; the firmware owns the Sample. Same shape
 * as RoomTempFallbackPolicy, deliberately - see that header for the millis()-rollover note,
 * which applies here identically.
 */
namespace OutsideTempFallbackPolicy {

    /// One hour. See the header comment for why this is six times the room's hold.
    constexpr uint32_t MAX_HOLD_MS = 3600000;

    enum class Source : uint8_t {
        MEASURED = 0,      ///< the sensor, this cycle
        HELD,              ///< the sensor, up to MAX_HOLD_MS ago - curve still usable
        ROOM_THERMOSTAT,   ///< no usable outside value; control on the room instead
        UNAVAILABLE        ///< no outside value AND no usable room - heating must stay off
    };

    struct Sample {
        Temperature_t value = 0;
        uint32_t atMs = 0;
        bool valid = false;
    };

    struct Decision {
        Temperature_t outside;  ///< only meaningful for MEASURED and HELD
        Source source;
    };

    inline void record(Sample& sample, Temperature_t measured, uint32_t nowMs) {
        sample.value = measured;
        sample.atMs = nowMs;
        sample.valid = true;
    }

    /// Unsigned subtraction, so this stays correct across the millis() rollover.
    inline bool holdUsable(const Sample& sample, uint32_t nowMs) {
        return sample.valid && (nowMs - sample.atMs) <= MAX_HOLD_MS;
    }

    /**
     * @param sample      caller-owned memory of the last good outside reading
     * @param outsideValid  readings.isOutsideTempValid
     * @param outsideTemp   readings.outsideTemp
     * @param nowMs         millis()
     * @param roomIsMeasurement the room value is a real reading (fresh or held)
     * @param roomTarget    heating/targetTemp, tenths; <= 0 means none configured
     */
    inline Decision resolve(Sample& sample, bool outsideValid, Temperature_t outsideTemp,
                            uint32_t nowMs, bool roomIsMeasurement, Temperature_t roomTarget) {
        if (outsideValid) {
            record(sample, outsideTemp, nowMs);
            Decision d = {outsideTemp, Source::MEASURED};
            return d;
        }
        if (holdUsable(sample, nowMs)) {
            Decision d = {sample.value, Source::HELD};
            return d;
        }
        if (roomIsMeasurement && roomTarget > 0) {
            Decision d = {0, Source::ROOM_THERMOSTAT};
            return d;
        }
        Decision d = {0, Source::UNAVAILABLE};
        return d;
    }

    /// True when the weather curve can still be evaluated on this decision.
    inline bool curveUsable(Source source) {
        return source == Source::MEASURED || source == Source::HELD;
    }

    inline const char* toString(Source source) {
        switch (source) {
            case Source::MEASURED:        return "measured";
            case Source::HELD:            return "held";
            case Source::ROOM_THERMOSTAT: return "room-thermostat fallback";
            case Source::UNAVAILABLE:     return "unavailable";
        }
        return "unknown";
    }

}  // namespace OutsideTempFallbackPolicy

#endif  // OUTSIDE_TEMP_FALLBACK_POLICY_H
