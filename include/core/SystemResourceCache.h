// include/core/SystemResourceCache.h
#ifndef SYSTEM_RESOURCE_CACHE_H
#define SYSTEM_RESOURCE_CACHE_H

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include "LoggingMacros.h"

/**
 * @file SystemResourceCache.h
 * @brief Caches frequently accessed system resources to avoid mutex contention
 * 
 * This class provides cached access to system resources that are accessed
 * frequently from high-priority tasks. Resources are loaded once during
 * initialization and then accessed without mutex overhead.
 */
class SystemResourceCache {
private:
    static constexpr const char* TAG = "ResourceCache";
    
    // Cached event groups
    static EventGroupHandle_t sensorEventGroup_;
    static EventGroupHandle_t relayEventGroup_;
    // systemEventGroup_ removed - consolidated into systemStateEventGroup_
    static EventGroupHandle_t systemStateEventGroup_;
    static EventGroupHandle_t burnerEventGroup_;
    static EventGroupHandle_t heatingEventGroup_;
    // wheaterEventGroup_ REMOVED - unused
    static EventGroupHandle_t controlRequestsEventGroup_;
    static EventGroupHandle_t burnerRequestEventGroup_;
    static EventGroupHandle_t errorNotificationEventGroup_;
    // Note: GeneralSystem event group accessed via SRP::getGeneralSystemEventGroup()
    // timerEventGroup_ REMOVED - unused
    static EventGroupHandle_t relayStatusEventGroup_;
    // mqttEventGroup_ removed - consolidated into systemStateEventGroup_
    
    // Cached mutexes
    static SemaphoreHandle_t sensorReadingsMutex_;
    static SemaphoreHandle_t relayReadingsMutex_;
    static SemaphoreHandle_t sharedResourcesMutex_;
    static SemaphoreHandle_t systemSettingsMutex_;
    
    static bool initialized_;
    
public:
    /**
     * @brief Initialize the cache by loading all resources
     * @return true if successful
     */
    static bool initialize();
    
    /**
     * @brief Check if cache is initialized
     */
    static bool isInitialized() { return initialized_; }
    
    // Event group getters - no mutex needed after initialization
    static EventGroupHandle_t getSensorEventGroup() {
        if (!initialized_) {
            LOG_ERROR(TAG, "Cache not initialized!");
            return nullptr;
        }
        return sensorEventGroup_;
    }
    
    static EventGroupHandle_t getRelayEventGroup() {
        if (!initialized_) return nullptr;
        return relayEventGroup_;
    }

    // getSystemEventGroup() removed - use getSystemStateEventGroup() instead
    // (consolidated in M1 refactoring)

    static EventGroupHandle_t getSystemStateEventGroup() {
        if (!initialized_) return nullptr;
        return systemStateEventGroup_;
    }
    
    static EventGroupHandle_t getBurnerEventGroup() {
        if (!initialized_) return nullptr;
        return burnerEventGroup_;
    }
    
    static EventGroupHandle_t getHeatingEventGroup() {
        if (!initialized_) return nullptr;
        return heatingEventGroup_;
    }

    // getWheaterEventGroup() REMOVED - unused

    static EventGroupHandle_t getControlRequestsEventGroup() {
        if (!initialized_) return nullptr;
        return controlRequestsEventGroup_;
    }
    
    static EventGroupHandle_t getBurnerRequestEventGroup() {
        if (!initialized_) return nullptr;
        return burnerRequestEventGroup_;
    }
    
    static EventGroupHandle_t getErrorNotificationEventGroup() {
        if (!initialized_) return nullptr;
        return errorNotificationEventGroup_;
    }

    // Note: Use SRP::getGeneralSystemEventGroup() instead

    // getTimerEventGroup() REMOVED - unused

    static EventGroupHandle_t getRelayStatusEventGroup() {
        if (!initialized_) return nullptr;
        return relayStatusEventGroup_;
    }

    // getMqttEventGroup() removed - use getSystemStateEventGroup() instead
    // (consolidated in M1 refactoring)

    // Mutex getters
    static SemaphoreHandle_t getSensorReadingsMutex() {
        if (!initialized_) return nullptr;
        return sensorReadingsMutex_;
    }
    
    static SemaphoreHandle_t getRelayReadingsMutex() {
        if (!initialized_) return nullptr;
        return relayReadingsMutex_;
    }
    
    static SemaphoreHandle_t getSharedResourcesMutex() {
        if (!initialized_) return nullptr;
        return sharedResourcesMutex_;
    }
    
    static SemaphoreHandle_t getSystemSettingsMutex() {
        if (!initialized_) return nullptr;
        return systemSettingsMutex_;
    }
};

#endif // SYSTEM_RESOURCE_CACHE_H