#pragma once
#include <cstdint>

namespace RelayIndex {
    // =========================================================
    // SINGLE SOURCE OF TRUTH FOR RELAY ASSIGNMENTS
    //
    // To change relay assignments:
    // 1. Change ONLY the values below
    // 2. Rebuild the project
    // 3. No other files need modification
    // =========================================================

    // Logical function → Array index (0-7)
    // Array index maps to physical relay: physical = index + 1

    constexpr uint8_t HEATING_PUMP   = 0;  // Physical Relay 1 - Heating circulation pump
    constexpr uint8_t WATER_PUMP     = 1;  // Physical Relay 2 - Water heating circulation pump
    constexpr uint8_t BURNER_ENABLE  = 2;  // Physical Relay 3 - Burner on/off control
    constexpr uint8_t HALF_POWER     = 3;  // Physical Relay 4 - Half power selector (ON=half, OFF=full)
    constexpr uint8_t WATER_MODE     = 4;  // Physical Relay 5 - Water heating mode (disables over-temp safety)
    constexpr uint8_t VALVE          = 5;  // Physical Relay 6 - Valve control
    constexpr uint8_t SPARE_7        = 6;  // Physical Relay 7 - Spare
    constexpr uint8_t SPARE_8        = 7;  // Physical Relay 8 - Spare

    constexpr uint8_t MAX_RELAYS = 8;

    // Convert array index to physical relay number (1-8)
    constexpr uint8_t toPhysical(uint8_t index) { return index + 1; }

    // Convert physical relay number (1-8) to array index (0-7)
    constexpr uint8_t fromPhysical(uint8_t physical) { return physical - 1; }
}
