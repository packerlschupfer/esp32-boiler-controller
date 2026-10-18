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
    // Named to match RelayIndex constants for clarity
    bool relayHeatingPump = false;  // Relay 1: Heating pump (was relayHpump)
    bool relayWaterPump = false;    // Relay 2: Water heating pump (was relayWhpump)
    bool relayBurnerEnable = false; // Relay 3: Burner enable/disable
    bool relayPowerBoost = false;   // Relay 2: Power boost (ON=Full, OFF=Half)
    bool relayWaterMode = false;    // Relay 5: Water heating mode enable (was relayWheaterMode - typo fixed)
    bool relayValve = false;        // Relay 6: Valve control
    bool relaySpare = false;        // Relay 7: Spare

    // General error code or status flags for relay operations
    int errorCode = 0;  // General error code for relay operations
};

// Note: Access to shared relay readings should be done through
// SystemResourceProvider (SRP) methods:
// - SRP::getRelayReadings() - Get reference to shared readings  
// - SRP::getRelayReadingsMutex() - Get mutex for thread-safe access

#endif // SHARED_RELAY_READINGS_H