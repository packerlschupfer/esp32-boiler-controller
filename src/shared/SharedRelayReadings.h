// SharedRelayReadings.h - Best practice implementation
#ifndef SHARED_RELAY_READINGS_H
#define SHARED_RELAY_READINGS_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Structure to represent the shared relay readings
// Using C++11 default member initializers for clean, guaranteed initialization
struct SharedRelayReadings {
    // Individual relay states - all default to false (OFF)
    bool relayHpump = false;        // Relay 1: Heating pump
    bool relayWhpump = false;       // Relay 2: Water heating pump
    bool relayBurnerEnable = false; // Relay 3: Burner enable/disable
    bool relayHalfPower = false;    // Relay 4: Power level (ON=Half, OFF=Full)
    bool relayWheaterMode = false;  // Relay 5: Water heating mode enable
    bool relayValve = false;        // Relay 6: Valve control
    bool relaySpare = false;        // Relay 7: Spare
    // Note: relayFullPower removed - full power is when relayHalfPower is OFF

    // General error code or status flags for relay operations
    int errorCode = 0;  // General error code for relay operations
};

// Note: Access to shared relay readings should be done through
// SystemResourceProvider (SRP) methods:
// - SRP::getRelayReadings() - Get reference to shared readings  
// - SRP::getRelayReadingsMutex() - Get mutex for thread-safe access

#endif // SHARED_RELAY_READINGS_H