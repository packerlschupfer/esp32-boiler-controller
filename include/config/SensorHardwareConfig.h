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
    constexpr std::array<mb8art::SensorHardwareConfig, 8> CONFIGS = {{
        // Index 0 - BOILER_OUTPUT
        {SensorIndex::toChannel(0), SystemEvents::SensorUpdate::BOILER_OUTPUT,
         SystemEvents::SensorUpdate::BOILER_OUTPUT_ERROR, true},
        // Index 1 - BOILER_RETURN
        {SensorIndex::toChannel(1), SystemEvents::SensorUpdate::BOILER_RETURN,
         SystemEvents::SensorUpdate::BOILER_RETURN_ERROR, true},
        // Index 2 - WATER_TANK
        {SensorIndex::toChannel(2), SystemEvents::SensorUpdate::WATER_TANK,
         SystemEvents::SensorUpdate::WATER_TANK_ERROR, true},
        // Index 3 - WATER_OUTPUT
        {SensorIndex::toChannel(3), SystemEvents::SensorUpdate::WATER_OUTPUT,
         SystemEvents::SensorUpdate::WATER_OUTPUT_ERROR, true},
        // Index 4 - WATER_RETURN
        {SensorIndex::toChannel(4), SystemEvents::SensorUpdate::WATER_RETURN,
         SystemEvents::SensorUpdate::WATER_RETURN_ERROR, true},
        // Index 5 - HEATING_RETURN
        {SensorIndex::toChannel(5), SystemEvents::SensorUpdate::HEATING_RETURN,
         SystemEvents::SensorUpdate::HEATING_RETURN_ERROR, true},
        // Index 6 - OUTSIDE
        {SensorIndex::toChannel(6), SystemEvents::SensorUpdate::OUTSIDE,
         SystemEvents::SensorUpdate::OUTSIDE_ERROR, true},
        // Index 7 - PRESSURE (handled separately - not a temperature sensor)
        {SensorIndex::toChannel(7), SystemEvents::SensorUpdate::PRESSURE,
         SystemEvents::SensorUpdate::PRESSURE_ERROR, true}
    }};

    // Helper to get config by logical index
    constexpr const mb8art::SensorHardwareConfig& get(uint8_t index) {
        return CONFIGS[index];
    }
}
