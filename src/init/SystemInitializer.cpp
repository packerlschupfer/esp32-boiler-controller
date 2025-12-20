// src/init/SystemInitializer.cpp
#include "SystemInitializer.h"
#include "ModbusDeviceInitializer.h"
#include "TaskInitializer.h"
#include "LoggingInitializer.h"
#include "HardwareInitializer.h"
#include "NetworkInitializer.h"

#include <Arduino.h>
#include <time.h>
#include "LoggingMacros.h"
#include "events/SystemEventsGenerated.h"

// TAG is defined but not used directly - using TAG instead
#ifndef LOG_NO_CUSTOM_LOGGER
#include <Logger.h>
#include <ConsoleBackend.h>
#endif
#include <MutexGuard.h>  // Include MutexGuard before TaskManager
#include <TaskManager.h>
#include <SemaphoreGuard.h>
#include "EthernetManager.h"
#include "OTAManager.h"
#include <MB8ART.h>
#include <RYN4.h>
#include <ANDRTF3.h>
#include "MQTTManager.h"
#include <Watchdog.h>
#include <esp32ModbusRTU.h>
#include <esp_log.h>
#include <ModbusRegistry.h>

// Control modules
#include "modules/control/HeatingControlModule.h"
#include "modules/control/WheaterControlModule.h"
#include "modules/control/PIDControlModule.h"
#include "modules/control/CentralizedFailsafe.h"
#include "modules/control/TemperatureSensorFallback.h"
#include "modules/control/BurnerRequestManager.h"

// Error handling
#include "utils/ErrorHandler.h"

// Core services
#include "core/SharedResourceManager.h"
#include "core/SystemResourceProvider.h"
#include "core/StateManager.h"
#include "modules/control/BurnerSystemController.h"

// Shared resources
#include "shared/SharedResources.h"
#include "shared/SharedSensorReadings.h"
#include "shared/SharedRelayReadings.h"
#include "shared/RelayState.h"

// HAL includes
#include "hal/HardwareAbstractionLayer.h"
#include "DS3231Controller.h"
#include "shared/SharedI2CInitializer.h"
#include <time.h>
#include <sys/time.h>
#include <RuntimeStorage.h>


static const char* TAG = "SystemInitializer";
// Type alias to avoid IntelliSense confusion with namespace/class naming
using RuntimeStoragePtr = class rtstorage::RuntimeStorage*;

// External globals
extern rtstorage::RuntimeStorage* gRuntimeStorage;

// Forward declarations for HAL configuration
namespace andrtf3 { class ANDRTF3; }

SystemInitializer::SystemInitializer()
    : currentStage_(InitStage::NONE),
      mb8art_(nullptr),
      ryn4_(nullptr),
      andrtf3_(nullptr),
      ds3231_(nullptr),
      runtimeStorage_(nullptr),
      mqttManager_(nullptr),
      heatingControl_(nullptr),
      wheaterControl_(nullptr),
      pidControl_(nullptr),
      burnerSystemController_(nullptr),
      deviceReadyEventGroup_(nullptr) {
}

SystemInitializer::~SystemInitializer() {
    if (currentStage_ != InitStage::NONE) {
        cleanup();
    }
}

Result<void> SystemInitializer::initializeSystem() {
    LOG_DEBUG(TAG, "initializeSystem() called!");
    LOG_INFO(TAG, "Starting system initialization...");
    
    // Initialize logging
    auto result = initializeLogging();
    if (result.isError()) {
        return result;
    }
    currentStage_ = InitStage::LOGGING;
    
    // Initialize shared resources
    result = initializeSharedResources();
    if (result.isError()) {
        cleanup();
        return result;
    }
    currentStage_ = InitStage::SHARED_RESOURCES;
    
    // Initialize TaskManager watchdog
    LOG_INFO(TAG, "Initializing TaskManager watchdog...");
    if (!SRP::getTaskManager().initWatchdog(30, true)) {  // 30 second timeout, panic on timeout
        LOG_ERROR(TAG, "Failed to initialize TaskManager watchdog");
        return Result<void>(SystemError::WATCHDOG_INIT_FAILED, "Failed to initialize TaskManager watchdog");
    }
    LOG_INFO(TAG, "TaskManager watchdog initialized successfully");
    
    // Initialize ESP-IDF Task Watchdog via Watchdog class
    LOG_INFO(TAG, "Initializing ESP-IDF Task Watchdog...");
    if (!Watchdog::quickInit(30, true)) {  // 30 second timeout, panic on timeout
        LOG_ERROR(TAG, "Failed to initialize ESP-IDF Task Watchdog");
        // This is not fatal as it might already be initialized
    } else {
        LOG_INFO(TAG, "ESP-IDF Task Watchdog initialized successfully");
    }
    
    // Initialize hardware
    result = initializeHardware();
    if (result.isError()) {
        cleanup();
        return result;
    }
    currentStage_ = InitStage::HARDWARE;
    
    // Initialize critical Modbus devices FIRST (MB8ART sensors, RYN4 relays)
    result = initializeModbusDevices();
    if (result.isError()) {
        cleanup();
        return result;
    }
    currentStage_ = InitStage::MODBUS_DEVICES;
    
    // Initialize network asynchronously (non-blocking)
    result = initializeNetworkAsync();
    if (result.isError()) {
        // Network failure is not critical for basic operation, but set degraded mode
        LOG_ERROR(TAG, "Network initialization failed: %s - operating in degraded mode",
                 result.message().c_str());
        SRP::setSystemStateEventBits(SystemEvents::SystemState::DEGRADED_MODE);
        ErrorHandler::logError(TAG, result.error(),
                              "Network unavailable - operating in degraded mode");
    }
    currentStage_ = InitStage::NETWORK;
    
    // Initialize control modules
    result = initializeControlModules();
    if (result.isError()) {
        cleanup();
        return result;
    }
    currentStage_ = InitStage::CONTROL_MODULES;
    
    // Initialize MQTT - Skip if using event-driven MQTT task
    #ifndef USE_EVENT_DRIVEN_MQTT
    result = initializeMQTT();
    if (result.isError()) {
        // MQTT failure is not critical but set degraded mode for monitoring
        LOG_ERROR(TAG, "MQTT initialization failed: %s - monitoring unavailable",
                 result.message().c_str());
        SRP::setSystemStateEventBits(SystemEvents::SystemState::DEGRADED_MODE);
        ErrorHandler::logError(TAG, result.error(),
                              "MQTT monitoring unavailable");
    }
    #else
    LOG_INFO(TAG, "Skipping MQTT initialization - handled by event-driven task");
    #endif
    currentStage_ = InitStage::MQTT;
    
    // Initialize tasks
    LOG_DEBUG(TAG, "About to call initializeTasks at %lu ms", millis());
    result = initializeTasks();
    LOG_DEBUG(TAG, "initializeTasks returned at %lu ms", millis());
    if (result.isError()) {
        cleanup();
        return result;
    }
    currentStage_ = InitStage::TASKS;
    
    currentStage_ = InitStage::COMPLETE;
    LOG_INFO(TAG, "System initialization complete!");
    LOG_INFO(TAG, "Free heap: %d bytes", ESP.getFreeHeap());
    
    return Result<void>();
}

Result<void> SystemInitializer::initializeLogging() {
    // Delegate to LoggingInitializer
    return LoggingInitializer::initialize();
}

Result<void> SystemInitializer::initializeSharedResources() {
    LOG_INFO(TAG, "Initializing shared resources...");

    // SharedResourceManager is a singleton - access directly, no ServiceContainer needed
    auto& resourceManager = SharedResourceManager::getInstance();

    // Initialize all standard resources through SharedResourceManager
    auto initResult = resourceManager.initializeStandardResources();
    if (initResult.isError()) {
        return Result<void>(initResult.error(), initResult.message());
    }
    
    // All resources are now accessed through SystemResourceProvider (SRP)
    // No need to initialize legacy global pointers

    // Create device ready event group for synchronization
    LOG_INFO(TAG, "Creating device ready event group...");
    deviceReadyEventGroup_ = xEventGroupCreate();
    if (!deviceReadyEventGroup_) {
        LOG_ERROR(TAG, "Failed to create device ready event group");
        return Result<void>(SystemError::MEMORY_ALLOCATION_FAILED, "Failed to create device ready event group");
    }
    LOG_INFO(TAG, "Device ready event group created successfully");

    // Clear any stale event bits from previous runs (prevents spurious actions on startup)
    // IMPORTANT: Do this BEFORE setting initial states
    // EventGroups are const char* string constants in SharedResourceManager
    const char* eventGroupsToClear[] = {
        SharedResourceManager::EventGroups::SENSOR,
        SharedResourceManager::EventGroups::BURNER,
        SharedResourceManager::EventGroups::BURNER_REQUEST,
        SharedResourceManager::EventGroups::ERROR_NOTIFICATION,
        // SYSTEM removed - consolidated into SYSTEM_STATE
        SharedResourceManager::EventGroups::SYSTEM_STATE,
        SharedResourceManager::EventGroups::CONTROL_REQUESTS,
        // WHEATER removed - unused
        SharedResourceManager::EventGroups::HEATING,
        // TIMER removed - unused
        SharedResourceManager::EventGroups::RELAY,
        SharedResourceManager::EventGroups::RELAY_STATUS,
        SharedResourceManager::EventGroups::RELAY_REQUEST
        // MQTT removed - consolidated into SYSTEM_STATE
    };

    int clearedCount = 0;
    for (const char* groupName : eventGroupsToClear) {
        EventGroupHandle_t handle = resourceManager.getEventGroup(groupName);
        if (handle) {
            xEventGroupClearBits(handle, 0xFFFFFF);  // Clear all 24 bits
            clearedCount++;
        }
    }
    LOG_INFO(TAG, "Cleared stale event bits from %d event groups", clearedCount);

    // Initialize StateManager - this syncs enable states from settings to event bits
    // AFTER clearing stale bits but BEFORE tasks start
    StateManager::initialize();

    SystemSettings& settings = SRP::getSystemSettings();
    LOG_INFO(TAG, "Initial system states set - Boiler:%s, Heating:%s, Water:%s",
             settings.boilerEnabled ? "EN" : "DIS",
             settings.heatingEnabled ? "EN" : "DIS",
             settings.waterEnabled ? "EN" : "DIS");

    // Initialize relay state tracking (DELAY command support)
    initRelayState();

    LOG_INFO(TAG, "Shared resources initialized successfully");
    LOG_INFO(TAG, "Total resources: %d (EventGroups: %d, Mutexes: %d)", 
             resourceManager.getTotalResourceCount(),
             resourceManager.getResourceCount(SharedResourceManager::ResourceType::EVENT_GROUP),
             resourceManager.getResourceCount(SharedResourceManager::ResourceType::MUTEX));
    
    return Result<void>();
}

Result<void> SystemInitializer::initializeHardware() {
    // Delegate to HardwareInitializer
    return HardwareInitializer::initialize(this);
}

Result<void> SystemInitializer::initializeNetwork() {
    // Delegate to NetworkInitializer (blocking version)
    return NetworkInitializer::initializeBlocking();
}

Result<void> SystemInitializer::initializeNetworkAsync() {
    // Delegate to NetworkInitializer (async version)
    return NetworkInitializer::initializeAsync();
}

// Helper methods
bool SystemInitializer::createMutex(SemaphoreHandle_t* mutex, const char* name) {
    if (*mutex == nullptr) {
        *mutex = xSemaphoreCreateMutex();
        if (*mutex == nullptr) {
            LOG_ERROR(TAG, "Failed to create %s mutex!", name);
            return false;
        }
        createdMutexes_.push_back({mutex, name});
    }
    return true;
}

bool SystemInitializer::createEventGroup(EventGroupHandle_t* group, const char* name) {
    if (*group == nullptr) {
        *group = xEventGroupCreate();
        if (*group == nullptr) {
            LOG_ERROR(TAG, "Failed to create %s event group!", name);
            return false;
        }
        createdEventGroups_.push_back({group, name});
    }
    return true;
}

void SystemInitializer::registerTask(TaskHandle_t handle, const char* name) {
    if (handle != nullptr) {
        createdTasks_.push_back({handle, name});
    }
}

void SystemInitializer::cleanup() {
    LOG_WARN(TAG, "Performing system cleanup from stage: %d", static_cast<int>(currentStage_));
    
    // Cleanup in reverse order of initialization
    if (currentStage_ >= InitStage::TASKS) {
        cleanupTasks();
    }
    
    if (currentStage_ >= InitStage::MQTT) {
        cleanupMQTT();
    }
    
    if (currentStage_ >= InitStage::CONTROL_MODULES) {
        cleanupControlModules();
    }
    
    if (currentStage_ >= InitStage::MODBUS_DEVICES) {
        cleanupModbusDevices();
    }
    
    if (currentStage_ >= InitStage::NETWORK) {
        cleanupNetwork();
    }
    
    if (currentStage_ >= InitStage::HARDWARE) {
        cleanupHardware();
    }
    
    if (currentStage_ >= InitStage::SHARED_RESOURCES) {
        cleanupSharedResources();
    }
    
    currentStage_ = InitStage::NONE;
    LOG_INFO(TAG, "System cleanup complete");
}

// Cleanup methods
void SystemInitializer::cleanupTasks() {
    LOG_INFO(TAG, "Cleaning up tasks...");
    
    for (const auto& task : createdTasks_) {
        if (task.handle != nullptr) {
            vTaskDelete(task.handle);
            LOG_INFO(TAG, "Deleted task: %s", task.name);
        }
    }
    createdTasks_.clear();
}

void SystemInitializer::cleanupMQTT() {
    LOG_INFO(TAG, "Cleaning up MQTT...");

    if (mqttManager_ != nullptr) {
        if (mqttManager_->isConnected()) {
            mqttManager_->disconnect();
        }
        // Don't delete - MQTTManager is now a singleton
        mqttManager_ = nullptr;
    }
}

void SystemInitializer::cleanupControlModules() {
    LOG_INFO(TAG, "Cleaning up control modules...");

    // Cleanup singletons first (they may hold FreeRTOS resources like mutexes)
    BurnerRequestManager::cleanup();
    CentralizedFailsafe::cleanup();
    TemperatureSensorFallback::cleanup();

    delete pidControl_; pidControl_ = nullptr;
    delete wheaterControl_; wheaterControl_ = nullptr;
    delete heatingControl_; heatingControl_ = nullptr;
}

void SystemInitializer::cleanupModbusDevices() {
    LOG_INFO(TAG, "Cleaning up Modbus devices...");
    
    if (runtimeStorage_ != nullptr) {
        delete runtimeStorage_;
        runtimeStorage_ = nullptr;
        gRuntimeStorage = nullptr;  // Clear global pointer
    }
    
    if (ds3231_ != nullptr) {
        delete ds3231_;
        ds3231_ = nullptr;
    }
    
    if (andrtf3_ != nullptr) {
        delete andrtf3_;
        andrtf3_ = nullptr;
    }
    
    if (ryn4_ != nullptr) {
        delete ryn4_;
        ryn4_ = nullptr;
    }
    
    if (mb8art_ != nullptr) {
        delete mb8art_;
        mb8art_ = nullptr;
    }
}

void SystemInitializer::cleanupNetwork() {
    LOG_INFO(TAG, "Cleaning up network...");
    // EthernetManager cleanup if needed
}

void SystemInitializer::cleanupHardware() {
    LOG_INFO(TAG, "Cleaning up hardware...");
    // NOTE: modbusMaster cleanup is handled in main.cpp
    Serial1.end();
}

void SystemInitializer::cleanupSharedResources() {
    LOG_INFO(TAG, "Cleaning up shared resources...");
    
    // Get SharedResourceManager to clean up all resources
    auto& resourceManager = SharedResourceManager::getInstance();
    resourceManager.cleanup();
    
    // Clean up device ready event group
    if (deviceReadyEventGroup_ != nullptr) {
        vEventGroupDelete(deviceReadyEventGroup_);
        deviceReadyEventGroup_ = nullptr;
    }
    
    // Legacy global pointers no longer exist - nothing to clear
    
    // Clean up old tracking lists (no longer needed)
    createdEventGroups_.clear();
    createdMutexes_.clear();
}

// Delegate to ModbusDeviceInitializer
Result<void> SystemInitializer::initializeModbusDevices() {
    return ModbusDeviceInitializer::initializeDevices(this);
}

// Delegate task creation to TaskInitializer
void SystemInitializer::createMB8ARTTasks() {
    TaskInitializer::createMB8ARTTasks(this);
}

void SystemInitializer::createHeatingControlTask() {
    TaskInitializer::createHeatingControlTask(this);
}

void SystemInitializer::createWaterControlTask() {
    TaskInitializer::createWaterControlTask(this);
}

void SystemInitializer::createBurnerControlTask() {
    TaskInitializer::createBurnerControlTask(this);
}

Result<void> SystemInitializer::initializeControlModules() {
    LOG_INFO(TAG, "Initializing control modules...");
    
    // Initialize centralized failsafe system first
    LOG_INFO(TAG, "Initializing centralized failsafe system...");
    CentralizedFailsafe::initialize();
    
    // Initialize temperature sensor fallback system
    LOG_INFO(TAG, "Initializing temperature sensor fallback...");
    TemperatureSensorFallback::initialize();
    
    // Initialize burner request manager for thread-safe operations
    LOG_INFO(TAG, "Initializing burner request manager...");
    BurnerRequestManager::initialize();

    // Get shared resources from SharedResourceManager
    auto& resourceManager = SharedResourceManager::getInstance();
    // SYSTEM consolidated into SYSTEM_STATE (M1 refactoring)
    EventGroupHandle_t systemEventGroup = resourceManager.getEventGroup(SharedResourceManager::EventGroups::SYSTEM_STATE);
    SemaphoreHandle_t sensorMutex = resourceManager.getMutex(SharedResourceManager::Mutexes::SENSOR_READINGS);

    // Initialize control modules
    heatingControl_ = new HeatingControlModule(systemEventGroup, sensorMutex);
    if (!heatingControl_) {
        return Result<void>(SystemError::MEMORY_ALLOCATION_FAILED, "Failed to create HeatingControlModule");
    }
    heatingControl_->initialize();

    wheaterControl_ = new WheaterControlModule();
    if (!wheaterControl_) {
        delete heatingControl_;
        heatingControl_ = nullptr;
        return Result<void>(SystemError::MEMORY_ALLOCATION_FAILED, "Failed to create WheaterControlModule");
    }
    wheaterControl_->initialize();

    pidControl_ = new PIDControlModule();
    if (!pidControl_) {
        delete heatingControl_;
        heatingControl_ = nullptr;
        delete wheaterControl_;
        wheaterControl_ = nullptr;
        return Result<void>(SystemError::MEMORY_ALLOCATION_FAILED, "Failed to create PIDControlModule");
    }
    
    // Pump management is now handled by independent PumpControlModule tasks
    // (HeatingPumpTask and WaterPumpTask) which watch HEATING_ON/WATER_ON event bits.
    // This allows pumps to run while burner is off (coasting for heat distribution).

    // Create BurnerSystemController (burner relay control only - H1 refactoring)
    LOG_INFO(TAG, "Creating BurnerSystemController...");
    burnerSystemController_ = new BurnerSystemController();
    if (!burnerSystemController_) {
        LOG_ERROR(TAG, "Failed to allocate BurnerSystemController!");
        // Non-fatal - system can still operate with legacy pump tasks
    } else {
        auto initResult = burnerSystemController_->initialize();
        if (initResult.isError()) {
            LOG_ERROR(TAG, "BurnerSystemController init failed: %s", initResult.message().c_str());
            delete burnerSystemController_;
            burnerSystemController_ = nullptr;
        } else {
            LOG_INFO(TAG, "BurnerSystemController initialized successfully");
        }
    }

    // Control modules are accessed via SystemInitializer member pointers or SRP
    // No ServiceContainer registration needed

    LOG_INFO(TAG, "Control modules initialized successfully");
    return Result<void>();
}

Result<void> SystemInitializer::initializeMQTT() {
    LOG_INFO(TAG, "Initializing MQTT...");
    LOG_INFO(TAG, "Free heap before MQTT creation: %d bytes", ESP.getFreeHeap());

    #ifdef ENABLE_MQTT
    // Get MQTT manager singleton instance - actual configuration done by MQTTTask
    mqttManager_ = &MQTTManager::getInstance();

    LOG_INFO(TAG, "MQTT manager instance obtained - configuration deferred to MQTTTask");
    #else
    LOG_INFO(TAG, "MQTT disabled in build configuration");
    #endif

    return Result<void>();
}

// Delegate to TaskInitializer
Result<void> SystemInitializer::initializeTasks() {
    return TaskInitializer::initializeTasks(this);
}

void SystemInitializer::cleanupLogging() {
    // Logging cleanup if needed
}

// Accessor implementations - direct member access (ServiceContainer removed)
MB8ART* SystemInitializer::getMB8ART() const {
    return mb8art_;
}

RYN4* SystemInitializer::getRYN4() const {
    return ryn4_;
}

andrtf3::ANDRTF3* SystemInitializer::getANDRTF3() const {
    return andrtf3_;
}

MQTTManager* SystemInitializer::getMQTTManager() const {
    return mqttManager_;
}

PIDControlModule* SystemInitializer::getPIDControl() const {
    return pidControl_;
}

HeatingControlModule* SystemInitializer::getHeatingControl() const {
    return heatingControl_;
}

WheaterControlModule* SystemInitializer::getWheaterControl() const {
    return wheaterControl_;
}

BurnerSystemController* SystemInitializer::getBurnerSystemController() const {
    return burnerSystemController_;
}

DS3231Controller* SystemInitializer::getDS3231() const {
    return ds3231_;
}

// Delegate to TaskInitializer
Result<void> SystemInitializer::initializeBurnerControlTask() {
    return TaskInitializer::initializeBurnerControlTask(this);
}