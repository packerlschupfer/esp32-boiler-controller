// include/modules/tasks/SensorOffsetMap.h
#ifndef SENSOR_OFFSET_MAP_H
#define SENSOR_OFFSET_MAP_H

#include <array>
#include <cstdint>

#include "config/SensorIndices.h"

/**
 * @brief Persisted sensor offsets -> MB8ART channel-indexed array (header-only, native-testable).
 *
 * MB8ART::setChannelOffsets() takes offsets positionally, one per physical channel. The
 * settings carry them by logical name. That translation is the step that was wrong for the
 * life of the project: the offset comments and MQTT_API.md documented an older map in which
 * `outside` was CH6, when CH6 is WATER_RETURN and CH3 is OUTSIDE. Nothing applied the offsets
 * at the time so the error was invisible, and it would have become a calibration quietly
 * landing on a hardware-deactivated channel the moment they were wired up (2026-10-19).
 *
 * So the mapping lives here rather than inline at the call site, with a test that pins each
 * field to its SensorIndex. Two properties the test holds that are worth stating:
 *
 * - CH4 (PRESSURE_CHANNEL) is ALWAYS 0. It is a 4-20 mA current input, not a temperature,
 *   and its calibration lives in a different layer and unit entirely (pressureOffset, in
 *   hundredths of BAR, applied by MB8ARTTasks). The MB8ART library refuses a non-temperature
 *   channel at apply time as well, so this is the outer of two independent guards.
 * - No two logical offsets map to the same channel. If they did, one would silently win.
 *
 * CH5 (WATER_TANK_TOP) is also always 0: there is no offset parameter for it. CH6 and CH7
 * entries are carried but inert while those channels are disabled - the library stores an
 * offset for an unbound channel and simply never applies it.
 */
namespace SensorOffsetMap {

    static_assert(SensorIndex::MAX_TEMP_SENSORS == 8,
                  "MB8ART::setChannelOffsets() takes std::array<int16_t, 8>");

    /// The persisted MB8ART temperature offsets, in tenths of a degree.
    struct Offsets {
        int16_t boilerOutput = 0;
        int16_t boilerReturn = 0;
        int16_t waterTank = 0;
        int16_t outside = 0;
        int16_t waterReturn = 0;
        int16_t heatingReturn = 0;
    };

    inline std::array<int16_t, SensorIndex::MAX_TEMP_SENSORS> build(const Offsets& o) {
        std::array<int16_t, SensorIndex::MAX_TEMP_SENSORS> out{};
        out[SensorIndex::BOILER_OUTPUT]  = o.boilerOutput;   // CH0
        out[SensorIndex::BOILER_RETURN]  = o.boilerReturn;   // CH1
        out[SensorIndex::WATER_TANK]     = o.waterTank;      // CH2
        out[SensorIndex::OUTSIDE]        = o.outside;        // CH3
        out[SensorIndex::WATER_RETURN]   = o.waterReturn;    // CH6
        out[SensorIndex::HEATING_RETURN] = o.heatingReturn;  // CH7
        // PRESSURE_CHANNEL (CH4) and WATER_TANK_TOP (CH5) deliberately left at 0.
        return out;
    }

} // namespace SensorOffsetMap

#endif // SENSOR_OFFSET_MAP_H
