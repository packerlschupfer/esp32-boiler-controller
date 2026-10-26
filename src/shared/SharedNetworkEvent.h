// SharedNetworkEvent.h
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include "EthernetManager.h"
#include "core/SystemResourceProvider.h"
#include "events/SystemEventsGenerated.h"

// Network event bits are now in SystemBits.h:
// SYSTEM_NETWORK_CONNECTED_BIT - Network connection established
// SYSTEM_NETWORK_DISCONNECTED_BIT - Network disconnection detected

class SharedNetworkEvent {
public:
    // Check if network is connected (delegates to EthernetManager)
    static bool isConnected() {
        return EthernetManager::isConnected();
    }
    
    // Get current IP address
    static String getIPAddress() {
        if (isConnected()) {
            return ETH.localIP().toString();
        }
        return "0.0.0.0";
    }
    
    // Update system event group based on current connection status
    // Call this periodically or when you need to update event bits
    static void updateSystemEventBits() {
        auto eventGroup = SRP::getSystemStateEventGroup();
        if (eventGroup != nullptr) {
            if (isConnected()) {
                xEventGroupSetBits(eventGroup, SYSTEM_NETWORK_CONNECTED_BIT);
                xEventGroupClearBits(eventGroup, SYSTEM_NETWORK_DISCONNECTED_BIT);
            } else {
                xEventGroupSetBits(eventGroup, SYSTEM_NETWORK_DISCONNECTED_BIT);
                xEventGroupClearBits(eventGroup, SYSTEM_NETWORK_CONNECTED_BIT);
            }
        }
    }
};
