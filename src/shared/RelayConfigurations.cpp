// src/shared/RelayConfigurations.cpp
#include "RelayConfigurations.h"
#include "ryn4/RelayDefs.h"
#include "SharedRelayReadings.h"
#include "core/SystemResourceProvider.h"

// No external declarations needed - using SRP methods

// Define relay configurations with NORMAL logic for all relays
// All relays start OFF (false) for safe initialization
std::vector<base::BaseRelayMapping> relayConfigurations = {
    // Relay 1: Heating Circulation Pump - NORMAL LOGIC
    // Start with relay OFF (pump OFF)
    {1, &SRP::getRelayReadings().relayHpump, ryn4::RELAY1_OPEN_BIT, ryn4::RELAY1_CLOSE_BIT, 
     ryn4::RELAY1_STATUS_BIT, ryn4::RELAY1_UPDATE_BIT, ryn4::RELAY1_ERROR_BIT, false, false},
    
    // Relay 2: Water Heating Circulation Pump - NORMAL LOGIC
    // Start with relay OFF (pump OFF)
    {2, &SRP::getRelayReadings().relayWhpump, ryn4::RELAY2_OPEN_BIT, ryn4::RELAY2_CLOSE_BIT, 
     ryn4::RELAY2_STATUS_BIT, ryn4::RELAY2_UPDATE_BIT, ryn4::RELAY2_ERROR_BIT, false, false},
    
    // Relay 3: Burner Enable - NORMAL LOGIC
    // When ON: Burner is enabled (must be ON for any burner operation)
    // When OFF: Burner is completely disabled
    // Start with relay OFF (burner disabled for safety)
    {3, &SRP::getRelayReadings().relayBurnerEnable, ryn4::RELAY3_OPEN_BIT, ryn4::RELAY3_CLOSE_BIT, 
     ryn4::RELAY3_STATUS_BIT, ryn4::RELAY3_UPDATE_BIT, ryn4::RELAY3_ERROR_BIT, false, false},
    
    // Relay 4: Power Level Selector - NORMAL LOGIC
    // When ON: Half power mode
    // When OFF: Full power mode
    // Start with relay OFF (full power selected, but burner is off via relay 3)
    {4, &SRP::getRelayReadings().relayHalfPower, ryn4::RELAY4_OPEN_BIT, ryn4::RELAY4_CLOSE_BIT, 
     ryn4::RELAY4_STATUS_BIT, ryn4::RELAY4_UPDATE_BIT, ryn4::RELAY4_ERROR_BIT, false, false},
    
    // Relay 5: Water Heating Mode Enable - NORMAL LOGIC
    // When ON: Enables water heating mode (disables hardware over-temp safety)
    // When OFF: Normal heating mode with hardware safety active
    // Start with relay OFF (safety enabled)
    {5, &SRP::getRelayReadings().relayWheaterMode, ryn4::RELAY5_OPEN_BIT, ryn4::RELAY5_CLOSE_BIT, 
     ryn4::RELAY5_STATUS_BIT, ryn4::RELAY5_UPDATE_BIT, ryn4::RELAY5_ERROR_BIT, false, false},
    
    // Relay 6: Valve - NORMAL LOGIC
    // Start with relay OFF
    {6, &SRP::getRelayReadings().relayValve, ryn4::RELAY6_OPEN_BIT, ryn4::RELAY6_CLOSE_BIT, 
     ryn4::RELAY6_STATUS_BIT, ryn4::RELAY6_UPDATE_BIT, ryn4::RELAY6_ERROR_BIT, false, false},
    
    // Relay 7: Spare - NORMAL LOGIC
    {7, nullptr, ryn4::RELAY7_OPEN_BIT, ryn4::RELAY7_CLOSE_BIT, 
     ryn4::RELAY7_STATUS_BIT, ryn4::RELAY7_UPDATE_BIT, ryn4::RELAY7_ERROR_BIT, false, false},
    
    // Relay 8: Spare - NORMAL LOGIC
    {8, nullptr, ryn4::RELAY8_OPEN_BIT, ryn4::RELAY8_CLOSE_BIT, 
     ryn4::RELAY8_STATUS_BIT, ryn4::RELAY8_UPDATE_BIT, ryn4::RELAY8_ERROR_BIT, false, false},
};

// Define the relayConfigs array with updated descriptions
RelayConfig relayConfigs[MAX_RELAYS] = {
    {0, "Heating Circulation Pump", true},      // Relay 1
    {1, "Water Heating Circulation Pump", true}, // Relay 2
    {2, "Burner Enable", true},                 // Relay 3 - Main burner on/off
    {3, "Half Power Select", true},             // Relay 4 - ON=half, OFF=full
    {4, "Water Heating Mode", true},            // Relay 5 - Enables water mode, disables safety
    {5, "Valve", true},                         // Relay 6
    {6, "Spare 1", false},                      // Relay 7
    {7, "Spare 2", false}                       // Relay 8
};

/*
 * Burner Operation States:
 * 
 * OFF:                  R3=OFF, R4=X, R5=OFF
 * HEATING FULL POWER:   R1=ON, R3=ON, R4=OFF, R5=OFF
 * HEATING HALF POWER:   R1=ON, R3=ON, R4=ON, R5=OFF
 * WATER FULL POWER:     R2=ON, R3=ON, R4=OFF, R5=ON
 * WATER HALF POWER:     R2=ON, R3=ON, R4=ON, R5=ON
 * 
 * R1 = Heating Pump
 * R2 = Water Pump
 * R3 = Burner Enable (must be ON for any burner operation)
 * R4 = Half Power Select (ON=half power, OFF=full power)
 * R5 = Water Mode Enable (disables hardware over-temperature safety)
 * 
 * Note: Relay 5 (Water Mode) disables hardware over-temperature safety
 *       when ON, so it should only be enabled during water heating cycles.
 */