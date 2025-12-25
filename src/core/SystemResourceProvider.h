// src/core/SystemResourceProvider.h
#ifndef SYSTEM_RESOURCE_PROVIDER_H
#define SYSTEM_RESOURCE_PROVIDER_H

#include "config/ProjectConfig.h"
#include "SharedResourceManager.h"
#include "LoggingMacros.h"  // Use LoggingMacros.h instead of Logger.h
#if defined(USE_CUSTOM_LOGGER) && !defined(LOG_NO_CUSTOM_LOGGER)
#include "Logger.h"  // Include Logger when using custom logger
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include <unordered_map>
#include <vector>

// Forward declarations
struct SharedSensorReadings;
struct SharedRelayReadings;
struct SystemSettings;
namespace modbus { class ModbusDevice; }
namespace rtstorage { class RuntimeStorage; }
namespace andrtf3 { class ANDRTF3; }
class DS3231Controller;
class Syslog;

/**
 * @brief Convenience class to provide easy access to system resources
 * 
 * This class provides static methods to quickly access commonly used
 * system resources without needing to repeatedly call getInstance() methods.
 * It helps reduce boilerplate code while maintaining clean architecture.
 */
class SystemResourceProvider {
public:
    // Event Groups
    // NOTE: SYSTEM event group was consolidated into SYSTEM_STATE (M1 refactoring)
    // Both were using SystemEvents::SystemState bits - now unified
    static EventGroupHandle_t getSystemStateEventGroup() {
        return SharedResourceManager::getInstance().getEventGroup(
            SharedResourceManager::EventGroups::SYSTEM_STATE);
    }
    
    static EventGroupHandle_t getBurnerEventGroup() {
        return SharedResourceManager::getInstance().getEventGroup(
            SharedResourceManager::EventGroups::BURNER);
    }
    
    static EventGroupHandle_t getBurnerRequestEventGroup() {
        return SharedResourceManager::getInstance().getEventGroup(
            SharedResourceManager::EventGroups::BURNER_REQUEST);
    }
    
    static EventGroupHandle_t getHeatingEventGroup() {
        return SharedResourceManager::getInstance().getEventGroup(
            SharedResourceManager::EventGroups::HEATING);
    }
    
    // WHEATER event group REMOVED - was unused
    // Use HEATING event group for heating-related events

    static EventGroupHandle_t getSensorEventGroup() {
        return SharedResourceManager::getInstance().getEventGroup(
            SharedResourceManager::EventGroups::SENSOR);
    }
    
    static EventGroupHandle_t getRelayEventGroup() {
        return SharedResourceManager::getInstance().getEventGroup(
            SharedResourceManager::EventGroups::RELAY);
    }
    
    static EventGroupHandle_t getControlRequestsEventGroup() {
        return SharedResourceManager::getInstance().getEventGroup(
            SharedResourceManager::EventGroups::CONTROL_REQUESTS);
    }

    // NOTE: MQTT event group was consolidated into SYSTEM_STATE (M1 refactoring)
    // MQTT bits (MQTT_OPERATIONAL, etc.) are in SystemEvents::SystemState namespace

    static EventGroupHandle_t getErrorNotificationEventGroup() {
        return SharedResourceManager::getInstance().getEventGroup(
            SharedResourceManager::EventGroups::ERROR_NOTIFICATION);
    }
    
    // TIMER event group REMOVED - was unused
    // Timer/scheduler functionality uses CONTROL_REQUESTS event group

    static EventGroupHandle_t getRelayStatusEventGroup() {
        return SharedResourceManager::getInstance().getEventGroup(
            SharedResourceManager::EventGroups::RELAY_STATUS);
    }
    
    // BLE event group removed - using MB8ART channel 7 for inside temperature
    // static EventGroupHandle_t getSensorMiThEventGroup() { ... }
    
    // Mutexes
    static SemaphoreHandle_t getSensorReadingsMutex() {
        return SharedResourceManager::getInstance().getMutex(
            SharedResourceManager::Mutexes::SENSOR_READINGS);
    }
    
    static SemaphoreHandle_t getRelayReadingsMutex() {
        return SharedResourceManager::getInstance().getMutex(
            SharedResourceManager::Mutexes::RELAY_READINGS);
    }
    
    static SemaphoreHandle_t getSystemSettingsMutex() {
        return SharedResourceManager::getInstance().getMutex(
            SharedResourceManager::Mutexes::SYSTEM_SETTINGS);
    }
    
    static SemaphoreHandle_t getMQTTMutex() {
        return SharedResourceManager::getInstance().getMutex(
            SharedResourceManager::Mutexes::MQTT);
    }
    
    // BLE mutex removed - using MB8ART channel 7 for inside temperature
    // static SemaphoreHandle_t getSensorMiThMutex() { ... }
    
    // Service accessors - implemented in SystemResourceProvider.cpp
    // These use gSystemInitializer to access services (ServiceContainer removed)
    static class MB8ART* getMB8ART();
    static class RYN4* getRYN4();
    static class MQTTManager* getMQTTManager();
    static class PIDControlModule* getPIDControl();
    static class HeatingControlModule* getHeatingControl();
    static class WheaterControlModule* getWheaterControl();
    static class FlameDetection* getFlameDetection();
    static class BurnerSystemController* getBurnerSystemController();
    static class EthernetManager* getEthernetManager();
    static DS3231Controller* getDS3231();
    static andrtf3::ANDRTF3* getANDRTF3();
    static Syslog* getSyslog();
    static void setSyslog(Syslog* syslog);

    // Logger access - uses Logger singleton
    #if defined(USE_CUSTOM_LOGGER) && !defined(LOG_NO_CUSTOM_LOGGER)
    static Logger& getLogger() {
        return Logger::getInstance();
    }
    #endif
    
    // Event Group Operations - provide complete abstraction
    // System State Event Group operations
    static EventBits_t getSystemStateEventBits() {
        return xEventGroupGetBits(getSystemStateEventGroup());
    }
    
    static EventBits_t setSystemStateEventBits(const EventBits_t bitsToSet) {
        return xEventGroupSetBits(getSystemStateEventGroup(), bitsToSet);
    }
    
    static EventBits_t clearSystemStateEventBits(const EventBits_t bitsToClear) {
        return xEventGroupClearBits(getSystemStateEventGroup(), bitsToClear);
    }
    
    static EventBits_t waitSystemStateEventBits(const EventBits_t bitsToWaitFor, 
                                                const BaseType_t clearOnExit,
                                                const BaseType_t waitForAllBits,
                                                TickType_t ticksToWait) {
        return xEventGroupWaitBits(getSystemStateEventGroup(), bitsToWaitFor, 
                                  clearOnExit, waitForAllBits, ticksToWait);
    }
    
    // Control Requests Event Group operations
    static EventBits_t getControlRequestsEventBits() {
        return xEventGroupGetBits(getControlRequestsEventGroup());
    }
    
    static EventBits_t setControlRequestsEventBits(const EventBits_t bitsToSet) {
        return xEventGroupSetBits(getControlRequestsEventGroup(), bitsToSet);
    }
    
    static EventBits_t clearControlRequestsEventBits(const EventBits_t bitsToClear) {
        return xEventGroupClearBits(getControlRequestsEventGroup(), bitsToClear);
    }
    
    static EventBits_t waitControlRequestsEventBits(const EventBits_t bitsToWaitFor, 
                                                   const BaseType_t clearOnExit,
                                                   const BaseType_t waitForAllBits,
                                                   TickType_t ticksToWait) {
        return xEventGroupWaitBits(getControlRequestsEventGroup(), bitsToWaitFor, 
                                  clearOnExit, waitForAllBits, ticksToWait);
    }
    
    // Heating Event Group operations
    static EventBits_t getHeatingEventBits() {
        return xEventGroupGetBits(getHeatingEventGroup());
    }
    
    static EventBits_t setHeatingEventBits(const EventBits_t bitsToSet) {
        return xEventGroupSetBits(getHeatingEventGroup(), bitsToSet);
    }
    
    static EventBits_t clearHeatingEventBits(const EventBits_t bitsToClear) {
        return xEventGroupClearBits(getHeatingEventGroup(), bitsToClear);
    }
    
    // Burner Event Group operations
    static EventBits_t getBurnerEventBits() {
        return xEventGroupGetBits(getBurnerEventGroup());
    }
    
    static EventBits_t setBurnerEventBits(const EventBits_t bitsToSet) {
        return xEventGroupSetBits(getBurnerEventGroup(), bitsToSet);
    }
    
    static EventBits_t clearBurnerEventBits(const EventBits_t bitsToClear) {
        return xEventGroupClearBits(getBurnerEventGroup(), bitsToClear);
    }
    
    // Burner Request Event Group operations
    static EventBits_t getBurnerRequestEventBits() {
        return xEventGroupGetBits(getBurnerRequestEventGroup());
    }
    
    static EventBits_t setBurnerRequestEventBits(const EventBits_t bitsToSet) {
        return xEventGroupSetBits(getBurnerRequestEventGroup(), bitsToSet);
    }
    
    static EventBits_t clearBurnerRequestEventBits(const EventBits_t bitsToClear) {
        return xEventGroupClearBits(getBurnerRequestEventGroup(), bitsToClear);
    }
    
    static EventBits_t waitBurnerRequestEventBits(const EventBits_t bitsToWaitFor, 
                                                  const BaseType_t clearOnExit,
                                                  const BaseType_t waitForAllBits,
                                                  TickType_t ticksToWait) {
        return xEventGroupWaitBits(getBurnerRequestEventGroup(), bitsToWaitFor, 
                                  clearOnExit, waitForAllBits, ticksToWait);
    }
    
    // Sensor Event Group operations
    static EventBits_t getSensorEventBits() {
        return xEventGroupGetBits(getSensorEventGroup());
    }
    
    static EventBits_t setSensorEventBits(const EventBits_t bitsToSet) {
        return xEventGroupSetBits(getSensorEventGroup(), bitsToSet);
    }
    
    static EventBits_t clearSensorEventBits(const EventBits_t bitsToClear) {
        return xEventGroupClearBits(getSensorEventGroup(), bitsToClear);
    }
    
    static EventBits_t waitSensorEventBits(const EventBits_t bitsToWaitFor, 
                                          const BaseType_t clearOnExit,
                                          const BaseType_t waitForAllBits,
                                          TickType_t ticksToWait) {
        return xEventGroupWaitBits(getSensorEventGroup(), bitsToWaitFor, 
                                  clearOnExit, waitForAllBits, ticksToWait);
    }
    
    // WHEATER Event Group operations REMOVED - was unused
    // Use HEATING event group for heating/water control events

    // Relay Event Group operations
    static EventBits_t getRelayEventBits() {
        return xEventGroupGetBits(getRelayEventGroup());
    }
    
    static EventBits_t setRelayEventBits(const EventBits_t bitsToSet) {
        return xEventGroupSetBits(getRelayEventGroup(), bitsToSet);
    }
    
    static EventBits_t clearRelayEventBits(const EventBits_t bitsToClear) {
        return xEventGroupClearBits(getRelayEventGroup(), bitsToClear);
    }

    // NOTE: MQTT event bits now use SystemState methods (M1 consolidation)
    // Use getSystemStateEventBits(), setSystemStateEventBits(), etc. for MQTT_OPERATIONAL

    // Error Notification Event Group operations
    static EventBits_t getErrorNotificationEventBits() {
        return xEventGroupGetBits(getErrorNotificationEventGroup());
    }
    
    static EventBits_t setErrorNotificationEventBits(const EventBits_t bitsToSet) {
        return xEventGroupSetBits(getErrorNotificationEventGroup(), bitsToSet);
    }
    
    static EventBits_t clearErrorNotificationEventBits(const EventBits_t bitsToClear) {
        return xEventGroupClearBits(getErrorNotificationEventGroup(), bitsToClear);
    }
    
    static EventBits_t waitErrorNotificationEventBits(const EventBits_t bitsToWaitFor, 
                                                      const BaseType_t clearOnExit,
                                                      const BaseType_t waitForAllBits,
                                                      TickType_t ticksToWait) {
        return xEventGroupWaitBits(getErrorNotificationEventGroup(), bitsToWaitFor, 
                                  clearOnExit, waitForAllBits, ticksToWait);
    }
    
    // General System Event Group operations
    static EventBits_t getGeneralSystemEventBits() {
        return xEventGroupGetBits(getGeneralSystemEventGroup());
    }
    
    static EventBits_t setGeneralSystemEventBits(const EventBits_t bitsToSet) {
        return xEventGroupSetBits(getGeneralSystemEventGroup(), bitsToSet);
    }
    
    static EventBits_t clearGeneralSystemEventBits(const EventBits_t bitsToClear) {
        return xEventGroupClearBits(getGeneralSystemEventGroup(), bitsToClear);
    }
    
    static EventBits_t waitGeneralSystemEventBits(const EventBits_t bitsToWaitFor, 
                                                  const BaseType_t clearOnExit,
                                                  const BaseType_t waitForAllBits,
                                                  TickType_t ticksToWait) {
        return xEventGroupWaitBits(getGeneralSystemEventGroup(), bitsToWaitFor, 
                                  clearOnExit, waitForAllBits, ticksToWait);
    }
    
    // Semaphore Operations - provide complete abstraction
    static BaseType_t takeSensorReadingsMutex(TickType_t xTicksToWait) {
        return xSemaphoreTake(getSensorReadingsMutex(), xTicksToWait);
    }
    
    static BaseType_t giveSensorReadingsMutex() {
        return xSemaphoreGive(getSensorReadingsMutex());
    }
    
    static BaseType_t takeRelayReadingsMutex(TickType_t xTicksToWait) {
        return xSemaphoreTake(getRelayReadingsMutex(), xTicksToWait);
    }
    
    static BaseType_t giveRelayReadingsMutex() {
        return xSemaphoreGive(getRelayReadingsMutex());
    }
    
    static BaseType_t takeSystemSettingsMutex(TickType_t xTicksToWait) {
        return xSemaphoreTake(getSystemSettingsMutex(), xTicksToWait);
    }
    
    static BaseType_t giveSystemSettingsMutex() {
        return xSemaphoreGive(getSystemSettingsMutex());
    }
    
    static BaseType_t takeMQTTMutex(TickType_t xTicksToWait) {
        return xSemaphoreTake(getMQTTMutex(), xTicksToWait);
    }
    
    static BaseType_t giveMQTTMutex() {
        return xSemaphoreGive(getMQTTMutex());
    }
    
    // BLE mutex operations removed - using MB8ART channel 7 for inside temperature
    
    // Shared Data Structure Access
    static SharedSensorReadings& getSensorReadings();
    static SharedRelayReadings& getRelayReadings();
    static SystemSettings& getSystemSettings();
    
    // Core System Resources
    static class TaskManager& getTaskManager();
    static class esp32ModbusRTU& getModbusMaster();
    static std::unordered_map<uint8_t, modbus::ModbusDevice*>& getDeviceMap();
    static SemaphoreHandle_t getDeviceMapMutex();
    
    // Health Monitor
    static class HealthMonitor* getHealthMonitor();
    
    // Runtime Storage (FRAM)
    static rtstorage::RuntimeStorage* getRuntimeStorage();
    
    // Memory Monitor
    // static class MemoryMonitor& getMemoryMonitor(); // Removed - MemoryMonitor deleted
    
    // Additional Event Groups
    static EventGroupHandle_t getGeneralSystemEventGroup();
    
    // Relay-specific resources (from RYN4)
    static EventBits_t& getRelayAllUpdateBits();
    static EventBits_t& getRelayAllErrorBits();
    
    
    // Task handles
    static TaskHandle_t& getBurnerTaskHandle();

    // Get the primary sensor read interval (MB8ART) for watchdog calculations
    static uint32_t getPrimarySensorReadInterval();
    
    // Get the room sensor read interval (ANDRTF3) for heating control
    static uint32_t getRoomSensorReadInterval();
    
    static int& getPidFactorSpaceHeating();
    static int& getPidFactorWaterHeating();
};

// Convenience macros for even shorter access (optional)
#define SRP SystemResourceProvider

#endif // SYSTEM_RESOURCE_PROVIDER_H