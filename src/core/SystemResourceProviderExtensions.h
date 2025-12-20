#pragma once

#include "SystemResourceProvider.h"
#include "MQTTManager.h"
#include "EthernetManager.h"
#include "events/SystemEventsGenerated.h"

// Extension methods for SystemResourceProvider to provide consistent access patterns

class SRPExtensions {
public:
    // Service access methods - delegate to SRP (ServiceContainer removed)
    static MQTTManager* getMQTTManager() {
        return SRP::getMQTTManager();
    }

    static class EthernetManager* getEthernetManager() {
        return SRP::getEthernetManager();
    }
    
    // Event handling utilities
    static void clearAllSensorUpdateBits() {
        constexpr EventBits_t SENSOR_UPDATE_MASK = 
            SystemEvents::SensorUpdate::BOILER_OUTPUT | 
            SystemEvents::SensorUpdate::BOILER_RETURN |
            SystemEvents::SensorUpdate::WATER_TANK |
            SystemEvents::SensorUpdate::WATER_OUTPUT |
            SystemEvents::SensorUpdate::WATER_RETURN |
            SystemEvents::SensorUpdate::HEATING_RETURN |
            SystemEvents::SensorUpdate::OUTSIDE |
            SystemEvents::SensorUpdate::INSIDE;
            
        SRP::clearSensorEventBits(SENSOR_UPDATE_MASK);
    }
    
    // Common timeout constants
    namespace Timeouts {
        constexpr TickType_t MUTEX_WAIT = pdMS_TO_TICKS(100);
        constexpr TickType_t EVENT_WAIT = pdMS_TO_TICKS(1000);
        constexpr TickType_t NETWORK_WAIT = pdMS_TO_TICKS(5000);
        constexpr TickType_t SENSOR_WAIT = pdMS_TO_TICKS(2000);
    }
};

// Convenience alias
using SRPX = SRPExtensions;