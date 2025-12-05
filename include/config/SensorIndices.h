#pragma once
#include <cstdint>

namespace SensorIndex {
    // =========================================================
    // SINGLE SOURCE OF TRUTH FOR SENSOR ASSIGNMENTS
    //
    // To change sensor channel assignments:
    // 1. Change ONLY the values below
    // 2. Rebuild the project
    // 3. No other files need modification
    // =========================================================

    // Logical function → MB8ART channel index (0-7)

    constexpr uint8_t BOILER_OUTPUT   = 0;  // CH0 - Boiler output temperature
    constexpr uint8_t BOILER_RETURN   = 1;  // CH1 - Boiler return temperature
    constexpr uint8_t WATER_TANK      = 2;  // CH2 - Water heater tank temperature
    constexpr uint8_t WATER_OUTPUT    = 3;  // CH3 - Water heater output temperature
    constexpr uint8_t WATER_RETURN    = 4;  // CH4 - Water heater return temperature
    constexpr uint8_t HEATING_RETURN  = 5;  // CH5 - Heating system return temperature
    constexpr uint8_t OUTSIDE         = 6;  // CH6 - Outside temperature
    // CH7 - Reserved for pressure sensor (handled separately)

    constexpr uint8_t MAX_TEMP_SENSORS = 7;  // Temperature sensors (CH0-CH6)
    constexpr uint8_t PRESSURE_CHANNEL = 7;  // CH7 - Pressure sensor (4-20mA)

    // Convert index to MB8ART channel (currently 1:1, but allows future flexibility)
    constexpr uint8_t toChannel(uint8_t index) { return index; }
}

namespace ANDRTF3Index {
    // ANDRTF3 sensor assignments (separate device)
    constexpr uint8_t INSIDE_TEMP     = 0;  // Inside/room temperature
    constexpr uint8_t INSIDE_HUMIDITY = 1;  // Inside humidity

    constexpr uint8_t MAX_ANDRTF3_SENSORS = 2;
}
