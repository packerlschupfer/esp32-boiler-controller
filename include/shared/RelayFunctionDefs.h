// include/shared/RelayFunctionDefs.h
#ifndef RELAY_FUNCTION_DEFS_H
#define RELAY_FUNCTION_DEFS_H

#include "config/SystemConstants.h"

// Centralized relay function index definitions
// These map to the relay indices used throughout the system
// Based on RelayConfigurations.cpp relay assignments

// Use relay assignments from SystemConstants
namespace RelayFunction = SystemConstants::Hardware::RelayFunction;

// Burner operation states for reference
// Relay functions:
//   BURNER_ENABLE (Relay 0) = Heating mode (half power)
//   POWER_BOOST (Relay 1)   = Boost to full power (works with heating or water)
//   WATER_MODE (Relay 2)    = Water mode (half power)
namespace BurnerOperation {
    // OFF:                  BURNER_ENABLE=OFF, POWER_BOOST=OFF, WATER_MODE=OFF
    // HEATING HALF POWER:   BURNER_ENABLE=ON,  POWER_BOOST=OFF, WATER_MODE=OFF
    // HEATING FULL POWER:   BURNER_ENABLE=ON,  POWER_BOOST=ON,  WATER_MODE=OFF
    // WATER HALF POWER:     BURNER_ENABLE=OFF, POWER_BOOST=OFF, WATER_MODE=ON
    // WATER FULL POWER:     BURNER_ENABLE=OFF, POWER_BOOST=ON,  WATER_MODE=ON
}

#endif // RELAY_FUNCTION_DEFS_H