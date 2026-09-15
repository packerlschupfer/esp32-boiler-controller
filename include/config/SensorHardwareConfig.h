#pragma once
#include <array>
#include <cstdint>
#include "MB8ART.h"
#include "events/SystemEventsGenerated.h"
#include "config/SensorIndices.h"

namespace SensorHardware {

    /**
     * @brief Hardware configuration for MB8ART sensor channels (constexpr - flash)
     *
     * This constexpr array defines the hardware configuration for all sensor channels.
     * It lives in flash memory and is indexed by the SensorIndex constants.
     *
     * To change sensor assignments, modify SensorIndices.h ONLY.
     */
    // Array index = MB8ART channel. The library sets these bits in its own sensor event group
    // (MB8ART::getSensorEventGroup(), polled by MB8ARTTasks via hasAnyUpdatePending()), not in
    // SRP's sensor event group. Until ESP32-MB8ART 1af4113 it ignored this table and used
    // interleaved bits (update 2n, error 2n+1); the labels here were shifted from SensorIndices
    // (2026-09-15). 0 = no event for the channel.
    constexpr std::array<mb8art::SensorHardwareConfig, 8> CONFIGS = {{
        // CH0 - boiler output
        {SensorIndex::BOILER_OUTPUT, SystemEvents::SensorUpdate::BOILER_OUTPUT,
         SystemEvents::SensorUpdate::BOILER_OUTPUT_ERROR, true},
        // CH1 - boiler return
        {SensorIndex::BOILER_RETURN, SystemEvents::SensorUpdate::BOILER_RETURN,
         SystemEvents::SensorUpdate::BOILER_RETURN_ERROR, true},
        // CH2 - water tank
        {SensorIndex::WATER_TANK, SystemEvents::SensorUpdate::WATER_TANK,
         SystemEvents::SensorUpdate::WATER_TANK_ERROR, true},
        // CH3 - outside
        {SensorIndex::OUTSIDE, SystemEvents::SensorUpdate::OUTSIDE,
         SystemEvents::SensorUpdate::OUTSIDE_ERROR, true},
        // CH4 - pressure (4-20 mA): MB8ARTTasks sets PRESSURE / PRESSURE_ERROR after conversion
        {SensorIndex::PRESSURE_CHANNEL, 0, 0, true},
        // CH5 - water tank top (optional, no event bit defined)
        {SensorIndex::WATER_TANK_TOP, 0, 0, true},
        // CH6 - water heater return (optional)
        {SensorIndex::WATER_RETURN, SystemEvents::SensorUpdate::WATER_RETURN,
         SystemEvents::SensorUpdate::WATER_RETURN_ERROR, true},
        // CH7 - heating return (optional)
        {SensorIndex::HEATING_RETURN, SystemEvents::SensorUpdate::HEATING_RETURN,
         SystemEvents::SensorUpdate::HEATING_RETURN_ERROR, true}
    }};

    // The library indexes the table by channel: a changed SensorIndices assignment must reorder it
    static_assert(CONFIGS[0].channelNumber == 0 && CONFIGS[1].channelNumber == 1 &&
                  CONFIGS[2].channelNumber == 2 && CONFIGS[3].channelNumber == 3 &&
                  CONFIGS[4].channelNumber == 4 && CONFIGS[5].channelNumber == 5 &&
                  CONFIGS[6].channelNumber == 6 && CONFIGS[7].channelNumber == 7,
                  "SensorHardware::CONFIGS index must equal the MB8ART channel");

    // Helper to get config by logical index
    constexpr const mb8art::SensorHardwareConfig& get(uint8_t index) {
        return CONFIGS[index];
    }
}
