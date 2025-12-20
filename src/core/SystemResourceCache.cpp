// src/core/SystemResourceCache.cpp
#include "core/SystemResourceCache.h"
#include "core/SharedResourceManager.h"

// Static member definitions
EventGroupHandle_t SystemResourceCache::sensorEventGroup_ = nullptr;
EventGroupHandle_t SystemResourceCache::relayEventGroup_ = nullptr;
// systemEventGroup_ removed - consolidated into systemStateEventGroup_
EventGroupHandle_t SystemResourceCache::systemStateEventGroup_ = nullptr;
EventGroupHandle_t SystemResourceCache::burnerEventGroup_ = nullptr;
EventGroupHandle_t SystemResourceCache::heatingEventGroup_ = nullptr;
// wheaterEventGroup_ REMOVED - unused
EventGroupHandle_t SystemResourceCache::controlRequestsEventGroup_ = nullptr;
EventGroupHandle_t SystemResourceCache::burnerRequestEventGroup_ = nullptr;
EventGroupHandle_t SystemResourceCache::errorNotificationEventGroup_ = nullptr;
// timerEventGroup_ REMOVED - unused
EventGroupHandle_t SystemResourceCache::relayStatusEventGroup_ = nullptr;
// mqttEventGroup_ removed - consolidated into systemStateEventGroup_

SemaphoreHandle_t SystemResourceCache::sensorReadingsMutex_ = nullptr;
SemaphoreHandle_t SystemResourceCache::relayReadingsMutex_ = nullptr;
SemaphoreHandle_t SystemResourceCache::sharedResourcesMutex_ = nullptr;
SemaphoreHandle_t SystemResourceCache::systemSettingsMutex_ = nullptr;

bool SystemResourceCache::initialized_ = false;

bool SystemResourceCache::initialize() {
    if (initialized_) {
        LOG_WARN(TAG, "Cache already initialized");
        return true;
    }
    
    LOG_INFO(TAG, "Initializing resource cache...");
    
    auto& manager = SharedResourceManager::getInstance();
    
    // Load all event groups
    sensorEventGroup_ = manager.getEventGroup(SharedResourceManager::EventGroups::SENSOR);
    if (!sensorEventGroup_) {
        LOG_ERROR(TAG, "Failed to get sensor event group");
        return false;
    }
    
    relayEventGroup_ = manager.getEventGroup(SharedResourceManager::EventGroups::RELAY);
    if (!relayEventGroup_) {
        LOG_ERROR(TAG, "Failed to get relay event group");
        return false;
    }

    // systemEventGroup_ removed - consolidated into systemStateEventGroup_

    systemStateEventGroup_ = manager.getEventGroup(SharedResourceManager::EventGroups::SYSTEM_STATE);
    if (!systemStateEventGroup_) {
        LOG_ERROR(TAG, "Failed to get system state event group");
        return false;
    }
    
    burnerEventGroup_ = manager.getEventGroup(SharedResourceManager::EventGroups::BURNER);
    if (!burnerEventGroup_) {
        LOG_ERROR(TAG, "Failed to get burner event group");
        return false;
    }
    
    heatingEventGroup_ = manager.getEventGroup(SharedResourceManager::EventGroups::HEATING);
    if (!heatingEventGroup_) {
        LOG_ERROR(TAG, "Failed to get heating event group");
        return false;
    }

    // wheaterEventGroup_ REMOVED - unused

    controlRequestsEventGroup_ = manager.getEventGroup(SharedResourceManager::EventGroups::CONTROL_REQUESTS);
    if (!controlRequestsEventGroup_) {
        LOG_ERROR(TAG, "Failed to get control requests event group");
        return false;
    }
    
    burnerRequestEventGroup_ = manager.getEventGroup(SharedResourceManager::EventGroups::BURNER_REQUEST);
    if (!burnerRequestEventGroup_) {
        LOG_ERROR(TAG, "Failed to get burner request event group");
        return false;
    }
    
    errorNotificationEventGroup_ = manager.getEventGroup(SharedResourceManager::EventGroups::ERROR_NOTIFICATION);
    if (!errorNotificationEventGroup_) {
        LOG_ERROR(TAG, "Failed to get error notification event group");
        return false;
    }

    // Note: GeneralSystem event group is managed separately in main.cpp
    // Use SRP::getGeneralSystemEventGroup() to access it

    // timerEventGroup_ REMOVED - unused

    relayStatusEventGroup_ = manager.getEventGroup(SharedResourceManager::EventGroups::RELAY_STATUS);
    if (!relayStatusEventGroup_) {
        LOG_ERROR(TAG, "Failed to get relay status event group");
        return false;
    }

    // mqttEventGroup_ removed - consolidated into systemStateEventGroup_

    // Load all mutexes
    sensorReadingsMutex_ = manager.getMutex(SharedResourceManager::Mutexes::SENSOR_READINGS);
    if (!sensorReadingsMutex_) {
        LOG_ERROR(TAG, "Failed to get sensor readings mutex");
        return false;
    }
    
    relayReadingsMutex_ = manager.getMutex(SharedResourceManager::Mutexes::RELAY_READINGS);
    if (!relayReadingsMutex_) {
        LOG_ERROR(TAG, "Failed to get relay readings mutex");
        return false;
    }
    
    // Note: SHARED_RESOURCES mutex doesn't exist in SharedResourceManager
    // This appears to be unused, so we'll skip it
    sharedResourcesMutex_ = nullptr;
    
    systemSettingsMutex_ = manager.getMutex(SharedResourceManager::Mutexes::SYSTEM_SETTINGS);
    if (!systemSettingsMutex_) {
        LOG_ERROR(TAG, "Failed to get system settings mutex");
        return false;
    }
    
    initialized_ = true;
    LOG_INFO(TAG, "Resource cache initialized successfully");
    return true;
}