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
namespace BurnerOperation {
    // OFF:                  BURNER_ENABLE=OFF, WATER_MODE=OFF, POWER_SELECT=OFF
    // HEATING FULL POWER:   HEATING_PUMP=ON, BURNER_ENABLE=ON, WATER_MODE=OFF, POWER_SELECT=OFF
    // HEATING HALF POWER:   HEATING_PUMP=ON, BURNER_ENABLE=ON, WATER_MODE=OFF, POWER_SELECT=ON
    // WATER FULL POWER:     WATER_PUMP=ON, BURNER_ENABLE=ON, WATER_MODE=ON, POWER_SELECT=OFF
    // WATER HALF POWER:     WATER_PUMP=ON, BURNER_ENABLE=ON, WATER_MODE=ON, POWER_SELECT=ON
}

#endif // RELAY_FUNCTION_DEFS_H