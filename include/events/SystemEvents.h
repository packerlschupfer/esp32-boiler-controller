// include/events/SystemEvents.h
// Zero-overhead event organization using namespaced constants
#pragma once

#include <freertos/event_groups.h>

/**
 * Organized Event Definitions
 * 
 * Zero runtime overhead - these are just constants like your current macros
 * But organized in namespaces to prevent conflicts and improve clarity
 */

namespace SystemEvents {
    
    // ===== System State Events (systemStateEventGroup) =====
    namespace State {
        constexpr EventBits_t BOILER_ENABLED   = (1 << 0);  // Boiler system enabled
        constexpr EventBits_t HEATING_ENABLED  = (1 << 1);  // Space heating enabled
        constexpr EventBits_t WATER_ENABLED    = (1 << 2);  // Water heating enabled
        constexpr EventBits_t BOILER_ON        = (1 << 3);  // Burner currently on
        constexpr EventBits_t HEATING_ON       = (1 << 4);  // Heating pump running
        constexpr EventBits_t WATER_ON         = (1 << 5);  // Water pump running
        constexpr EventBits_t EMERGENCY_STOP   = (1 << 6);  // Emergency stop active
        constexpr EventBits_t ERROR_ACTIVE     = (1 << 7);  // System error active
        // Next free bit: 8
        
        // Useful combinations
        constexpr EventBits_t ALL_ENABLED = BOILER_ENABLED | HEATING_ENABLED | WATER_ENABLED;
        constexpr EventBits_t ANY_ACTIVE = BOILER_ON | HEATING_ON | WATER_ON;
    }
    
    // ===== Burner Request Events (burnerRequestEventGroup) =====
    namespace Request {
        constexpr EventBits_t HEATING         = (1 << 0);   // Space heating request
        constexpr EventBits_t WATER           = (1 << 1);   // Water heating request
        constexpr EventBits_t POWER_LOW       = (1 << 2);   // Low power mode
        constexpr EventBits_t POWER_HIGH      = (1 << 3);   // High power mode
        constexpr EventBits_t IGNITION        = (1 << 4);   // Ignition sequence
        constexpr EventBits_t SHUTDOWN        = (1 << 5);   // Shutdown request
        // Bits 16-23 reserved for temperature encoding
        constexpr int TEMP_ENCODE_SHIFT = 16;
        constexpr EventBits_t TEMP_ENCODE_MASK = (0xFF << TEMP_ENCODE_SHIFT);
        // Next free bit: 6 (until bit 16)
        
        // Useful combinations
        constexpr EventBits_t ANY_REQUEST = HEATING | WATER;
        constexpr EventBits_t POWER_BITS = POWER_LOW | POWER_HIGH;
    }
    
    // ===== Sensor Update Events (sensorEventGroup) =====
    namespace Sensor {
        constexpr EventBits_t BOILER_OUTPUT   = (1 << 0);   // Boiler output temp
        constexpr EventBits_t BOILER_RETURN   = (1 << 1);   // Boiler return temp
        constexpr EventBits_t WATER_TANK      = (1 << 2);   // Water tank temp
        constexpr EventBits_t HEATING_RETURN  = (1 << 3);   // Heating return temp
        constexpr EventBits_t OUTSIDE         = (1 << 4);   // Outside temp
        constexpr EventBits_t INSIDE          = (1 << 5);   // Inside temp
        constexpr EventBits_t EXHAUST         = (1 << 6);   // Exhaust temp
        // Next free bit: 7
        
        // Useful combinations
        constexpr EventBits_t ALL_TEMPS = BOILER_OUTPUT | BOILER_RETURN | 
                                         WATER_TANK | HEATING_RETURN | 
                                         OUTSIDE | INSIDE | EXHAUST;
        constexpr EventBits_t CRITICAL_TEMPS = BOILER_OUTPUT | EXHAUST;
    }
    
    // ===== Error Events (errorEventGroup) =====
    namespace Error {
        constexpr EventBits_t SENSOR_FAIL     = (1 << 0);   // Sensor failure
        constexpr EventBits_t OVERTEMP        = (1 << 1);   // Over temperature
        constexpr EventBits_t UNDERTEMP       = (1 << 2);   // Under temperature
        constexpr EventBits_t IGNITION_FAIL   = (1 << 3);   // Ignition failed
        constexpr EventBits_t FLAME_LOSS      = (1 << 4);   // Flame lost
        constexpr EventBits_t PRESSURE_LOW    = (1 << 5);   // Low pressure
        constexpr EventBits_t PRESSURE_HIGH   = (1 << 6);   // High pressure
        constexpr EventBits_t MODBUS_TIMEOUT  = (1 << 7);   // Modbus timeout
        // Next free bit: 8
        
        // Useful combinations
        constexpr EventBits_t CRITICAL_ERRORS = OVERTEMP | FLAME_LOSS | PRESSURE_HIGH;
        constexpr EventBits_t ANY_ERROR = 0xFF;  // First 8 bits
    }
}

// Usage remains exactly the same as before:
// xEventGroupSetBits(systemStateGroup, SystemEvents::State::BOILER_ENABLED);
// 
// But now:
// 1. No risk of bit conflicts between groups
// 2. Clear organization of related events
// 3. Autocomplete shows available events
// 4. Zero overhead - compiles to same code as macros