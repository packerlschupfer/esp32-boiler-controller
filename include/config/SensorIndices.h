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
    //
    // Active channels controlled by MB8ART_ACTIVE_CHANNELS in ProjectConfig.h
    // Default: CH0-3 enabled, CH4-7 deactivated at hardware level
    // =========================================================

    // Logical function → MB8ART channel index (0-7)

    // Core sensors (enabled by default when MB8ART_ACTIVE_CHANNELS >= 4)
    constexpr uint8_t BOILER_OUTPUT   = 0;  // CH0 - Boiler output temperature
    constexpr uint8_t BOILER_RETURN   = 1;  // CH1 - Boiler return temperature
    constexpr uint8_t WATER_TANK      = 2;  // CH2 - Water heater tank temperature
    constexpr uint8_t OUTSIDE         = 3;  // CH3 - Outside temperature
    constexpr uint8_t PRESSURE_CHANNEL = 4; // CH4 - Pressure sensor (4-20mA) - deactivated by default

    // Optional sensors (enable via ENABLE_SENSOR_* flags AND increase MB8ART_ACTIVE_CHANNELS)
    constexpr uint8_t WATER_TANK_TOP  = 5;  // CH5 - Top of water tank (stratification)
    constexpr uint8_t WATER_RETURN    = 6;  // CH6 - Water heater return temperature
    constexpr uint8_t HEATING_RETURN  = 7;  // CH7 - Heating system return temperature

    constexpr uint8_t MAX_TEMP_SENSORS = 8;  // All 8 channels used

    // Convert index to MB8ART channel (currently 1:1, but allows future flexibility)
    constexpr uint8_t toChannel(uint8_t index) { return index; }
}

namespace ANDRTF3Index {
    // ANDRTF3 sensor assignments (separate device)
    constexpr uint8_t INSIDE_TEMP     = 0;  // Inside/room temperature
    constexpr uint8_t INSIDE_HUMIDITY = 1;  // Inside humidity

    constexpr uint8_t MAX_ANDRTF3_SENSORS = 2;
}
