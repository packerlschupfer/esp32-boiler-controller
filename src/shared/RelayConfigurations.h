#ifndef RELAY_CONFIGURATIONS_H
#define RELAY_CONFIGURATIONS_H

// src/shared/RelayConfigurations.h
#include <vector>
#include <cstddef> // For size_t
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h" // For EventBits_t
#include "base/BaseRelayMapping.h" // Assuming this defines BaseRelayMapping
#include "SharedRelayReadings.h"  // For sharedRelayReadings definition

// Note: relayConfigurations is accessed via SystemResourceProvider::getRelayConfigurations()
// THIS IS WHAT RYN4 USES

struct RelayMapping {
    uint8_t relayIndex;                  // Index of the relay
    bool* relayState;                    // Pointer to the relay state in SharedRelayReadings
    EventBits_t controlOpenBit;          // Bit to trigger opening the relay
    EventBits_t controlCloseBit;         // Bit to trigger closing the relay
    EventBits_t statusBit;               // Bit indicating the relay's current status (open/closed)
    EventBits_t updateBit;               // Bit for successful update notification
    EventBits_t errorBit;                // Bit for error indication
    bool isActive;                       // Indicates if the relay control is active/used
};

struct RelayConfig {
    uint8_t relayIndex;
    char relayFunctionName[32]; // Ensure this is adequately sized for your function names
    bool isActive;
};

// Example of an array to hold the configuration for each relay
constexpr size_t MAX_RELAYS = 8;

// Note: relayConfigs is accessed through SystemResourceProvider
// Use SRP::getRelayConfigs() to access the relay configuration array

#endif // RELAY_CONFIGURATIONS_H