// src/modules/tasks/PersistentStorageTask.cpp
// Persistent storage task - handles parameter save/load operations
#include "PersistentStorageTask.h"
#include <TaskManager.h>
#include "LoggingMacros.h"
#include "core/SystemResourceProvider.h"
#include <algorithm>  // Round 15 Issue #16: For std::min in exponential backoff
#include "shared/SharedSensorReadings.h"
#include "shared/SharedRelayReadings.h"
#include "config/SystemSettings.h"
#include "config/SystemConstants.h"
#include <MQTTManager.h>
#include "events/SystemEventsGenerated.h"
#include <PersistentStorage.h>
#include "MQTTTask.h"
#include "utils/ErrorLogFRAM.h"
#include "utils/TemperatureParameterWrapper.h"
#include "core/StateManager.h"

// Global temperature shadows for SystemSettings
static SystemSettingsTemperatureShadows temperatureShadows;

// Constants - use centralized values
static const char* TAG = "PersistentStorageTask";
namespace StorageConstants = SystemConstants::Tasks::Storage;
static const uint32_t IMMEDIATE_SAVE_DELAY_MS = 1000; // 1 second debounce

// Event bits for storage operations
#define STORAGE_SAVE_REQUEST_BIT    (1UL << 0UL)
#define STORAGE_LOAD_REQUEST_BIT    (1UL << 1UL)
#define STORAGE_MQTT_RECONNECT_BIT  (1UL << 2UL)

// Static event group for storage task
static EventGroupHandle_t storageEventGroup = nullptr;

// Track if parameters have changed
static bool parametersChanged = false;
static uint32_t lastChangeTime = 0;

// Task function
void PersistentStorageTask(void* pvParameters) {
    LOG_INFO(TAG, "PersistentStorageTask started");
    
    // Create event group for storage operations
    storageEventGroup = xEventGroupCreate();
    if (!storageEventGroup) {
        LOG_ERROR(TAG, "Failed to create event group!");
        vTaskDelete(NULL);
        return;
    }
    
    // Initialize PersistentStorage
    // Use "boiler/params" prefix to match other MQTT topics (boiler/cmd, boiler/status, etc.)
    //
    // Round 15 Issue #14: MEMORY MANAGEMENT NOTE
    // This 'storage' pointer is intentionally never deleted because:
    //   1. This task runs forever in a while(true) loop (line ~461)
    //   2. The storage object is used throughout the task's lifetime
    //   3. FreeRTOS tasks can't return - they must call vTaskDelete()
    //   4. If the task were to be deleted, FreeRTOS would reclaim the stack
    //      but not heap allocations - this is a known FreeRTOS pattern
    // The ~200 bytes for PersistentStorage is allocated once at startup.
    PersistentStorage* storage = new PersistentStorage("esplan", "boiler/params");

    // Track whether NVS initialized successfully
    bool nvsAvailable = storage->begin();

    if (!nvsAvailable) {
        LOG_ERROR(TAG, "NVS init failed - attempting recovery by erasing namespace");

        // Try to erase corrupted namespace and reinitialize
        if (storage->eraseNamespace()) {
            LOG_INFO(TAG, "NVS namespace erased, retrying init...");
            nvsAvailable = storage->begin();

            if (nvsAvailable) {
                LOG_INFO(TAG, "NVS recovery successful - will save defaults on first use");
            } else {
                LOG_ERROR(TAG, "NVS recovery failed - operating without persistence");
            }
        } else {
            LOG_ERROR(TAG, "NVS erase failed - operating without persistence");
        }

        if (!nvsAvailable) {
            // System can still operate safely with compile-time defaults
            LOG_WARN(TAG, "System will operate with default parameters (no persistence)");
        }
    }
    
    // PersistentStorage is a local object, no registration needed
    // ServiceContainer has been removed from the codebase

    // Initialize error logging system with FRAM
    rtstorage::RuntimeStorage* runtimeStorage = SRP::getRuntimeStorage();
    if (runtimeStorage && ErrorLogFRAM::begin(runtimeStorage)) {
        LOG_INFO(TAG, "ErrorLogFRAM initialized");
    } else {
        if (!runtimeStorage) {
            LOG_WARN(TAG, "RuntimeStorage not available - error logging disabled");
        } else {
            LOG_ERROR(TAG, "ErrorLogFRAM init failed");
        }
        // Continue anyway - system can work without error logging
    }
    
    // Get references to system settings
    auto& settings = SRP::getSystemSettings();
    
    // Initialize temperature shadows from current settings
    temperatureShadows.initializeFromSettings(settings);
    
    // Register system settings parameters
    
    // Water heater configuration
    storage->registerBool("wheater/priorityEnabled", &settings.wheaterPriorityEnabled,
                          "Water heating priority over space heating");
    
    // Register temperature parameters with wrappers
    // Two-threshold control: start heating below tempLimitLow, stop above tempLimitHigh
    TemperatureParameterWrapper::registerTemperature(storage, "wheater/tempLimitLow",
                          &settings.wHeaterConfTempLimitLow, &temperatureShadows.wHeaterConfTempLimitLow,
                          30.0f, 60.0f, "Start water heating when tank drops below this");

    TemperatureParameterWrapper::registerTemperature(storage, "wheater/tempLimitHigh",
                          &settings.wHeaterConfTempLimitHigh, &temperatureShadows.wHeaterConfTempLimitHigh,
                          50.0f, 85.0f, "Stop water heating when tank rises above this");
    
    storage->registerFloat("wheater/tempChargeDelta", &settings.wHeaterConfTempChargeDelta,
                          1.0f, 20.0f, "Water heater charging temperature delta");
    
    TemperatureParameterWrapper::registerTemperature(storage, "wheater/tempSafeLimitHigh",
                          &settings.wHeaterConfTempSafeLimitHigh, &temperatureShadows.wHeaterConfTempSafeLimitHigh,
                          60.0f, 95.0f, "Water heater safety high limit");
    
    TemperatureParameterWrapper::registerTemperature(storage, "wheater/tempSafeLimitLow",
                          &settings.wHeaterConfTempSafeLimitLow, &temperatureShadows.wHeaterConfTempSafeLimitLow,
                          0.0f, 10.0f, "Water heater safety low limit");

    storage->registerFloat("wheater/heatingRate", &settings.waterHeatingRate,
                          0.1f, 5.0f, "Water heating rate (°C per minute)");
    
    // Heating configuration
    TemperatureParameterWrapper::registerTemperature(storage, "heating/targetTemp",
                          &settings.targetTemperatureInside, &temperatureShadows.targetTemperatureInside,
                          10.0f, 30.0f, "Target indoor temperature");
    
    storage->registerFloat("heating/curveShift", &settings.heating_curve_shift,
                          -20.0f, 40.0f, "Heating curve shift");
    
    storage->registerFloat("heating/curveCoeff", &settings.heating_curve_coeff,
                          0.5f, 4.0f, "Heating curve coefficient");
    
    TemperatureParameterWrapper::registerTemperature(storage, "heating/burnerLowLimit",
                          &settings.burner_low_limit, &temperatureShadows.burner_low_limit,
                          20.0f, 50.0f, "Minimum burner temperature");
    
    TemperatureParameterWrapper::registerTemperature(storage, "heating/highLimit",
                          &settings.heating_high_limit, &temperatureShadows.heating_high_limit,
                          50.0f, 90.0f, "Maximum heating temperature");
    
    TemperatureParameterWrapper::registerTemperature(storage, "heating/hysteresis",
                          &settings.heating_hysteresis, &temperatureShadows.heating_hysteresis,
                          0.1f, 2.0f, "Heating temperature hysteresis");
    
    // PID parameters for space heating
    storage->registerFloat("pid/spaceHeating/kp", &settings.spaceHeatingKp,
                          0.0f, 10.0f, "Space heating PID proportional gain");
    
    storage->registerFloat("pid/spaceHeating/ki", &settings.spaceHeatingKi,
                          0.0f, 5.0f, "Space heating PID integral gain");
    
    storage->registerFloat("pid/spaceHeating/kd", &settings.spaceHeatingKd,
                          0.0f, 5.0f, "Space heating PID derivative gain");
    
    // PID parameters for water heating
    storage->registerFloat("pid/waterHeater/kp", &settings.wHeaterKp,
                          0.0f, 10.0f, "Water heater PID proportional gain");

    storage->registerFloat("pid/waterHeater/ki", &settings.wHeaterKi,
                          0.0f, 5.0f, "Water heater PID integral gain");

    storage->registerFloat("pid/waterHeater/kd", &settings.wHeaterKd,
                          0.0f, 5.0f, "Water heater PID derivative gain");

    // PID auto-tuning configuration
    storage->registerFloat("pid/autotune/amplitude", &settings.autotuneRelayAmplitude,
                          10.0f, 100.0f, "Auto-tune relay amplitude (%)");

    storage->registerFloat("pid/autotune/hysteresis", &settings.autotuneHysteresis,
                          0.5f, 10.0f, "Auto-tune hysteresis band (°C)");

    storage->registerInt("pid/autotune/method", &settings.autotuneMethod,
                        0, 4, "Auto-tune method (0=ZN_PI,1=ZN_PID,2=TL,3=CC,4=Lambda)");

    // System enable states (persisted - remember user preferences across reboots)
    storage->registerBool("system/boilerEnabled", &settings.boilerEnabled,
                          "Boiler system master enable");
    storage->registerBool("system/heatingEnabled", &settings.heatingEnabled,
                          "Space heating enable");
    storage->registerBool("system/waterEnabled", &settings.waterEnabled,
                          "Water heating enable");

    // Override flags (summer mode - block heating/water when manual valves closed)
    storage->registerBool("system/heatingOverrideOff", &settings.heatingOverrideOff,
                          "Heating circuit blocked (summer mode)");
    storage->registerBool("system/waterOverrideOff", &settings.waterOverrideOff,
                          "Water heating blocked");

    // Sensor compensation offsets (MB8ART channels) - int32_t shadows, values in tenths of °C
    // MQTT: Send integer value e.g., -14 for -1.4°C offset, 5 for +0.5°C offset
    storage->registerInt("sensor/offset/boilerOutput", &temperatureShadows.boilerOutputOffset,
                          -50, 50, "Boiler output offset (tenths °C, CH0)");
    storage->registerInt("sensor/offset/boilerReturn", &temperatureShadows.boilerReturnOffset,
                          -50, 50, "Boiler return offset (tenths °C, CH1)");
    storage->registerInt("sensor/offset/waterTank", &temperatureShadows.waterTankOffset,
                          -50, 50, "Water tank offset (tenths °C, CH2)");
    storage->registerInt("sensor/offset/waterOutput", &temperatureShadows.waterOutputOffset,
                          -50, 50, "Water output offset (tenths °C, CH3)");
    storage->registerInt("sensor/offset/waterReturn", &temperatureShadows.waterReturnOffset,
                          -50, 50, "Water return offset (tenths °C, CH4)");
    storage->registerInt("sensor/offset/heatingReturn", &temperatureShadows.heatingReturnOffset,
                          -50, 50, "Heating return offset (tenths °C, CH5)");
    storage->registerInt("sensor/offset/outside", &temperatureShadows.outsideTempOffset,
                          -50, 50, "Outside temp offset (tenths °C, CH6)");
    // ANDRTF3 room temperature
    storage->registerInt("sensor/offset/room", &temperatureShadows.roomTempOffset,
                          -50, 50, "Room temp offset (tenths °C, ANDRTF3)");
    // Pressure sensor - hundredths of BAR (e.g., -5 = -0.05 BAR)
    storage->registerInt("sensor/offset/pressure", &temperatureShadows.pressureOffset,
                          -50, 50, "Pressure offset (hundredths BAR)");

    // Note: Sensor intervals are now compile-time constants in SystemConstants::Timing
    // (MB8ART_SENSOR_READ_INTERVAL_MS, ANDRTF3_SENSOR_READ_INTERVAL_MS)

    // Callbacks for parameter changes
    auto paramChangeCallback = [](const std::string& name, const void* value) {
        LOG_INFO(TAG, "Parameter %s changed", name.c_str());
        parametersChanged = true;
        lastChangeTime = millis();
        // Notify control task
        // Settings changed - no direct equivalent in new system, notify via control request
        SRP::setControlRequestsEventBits(SystemEvents::ControlRequest::SAVE_PARAMETERS);
        // Request save after debounce
        xEventGroupSetBits(storageEventGroup, STORAGE_SAVE_REQUEST_BIT);
    };

    // Callback for sensor offset parameters - syncs int32_t shadow to Temperature_t immediately
    auto sensorOffsetCallback = [&settings](const std::string& name, const void* value) {
        LOG_INFO(TAG, "Sensor offset %s changed", name.c_str());
        // Sync all offset shadows to settings (efficient - just integer casts)
        temperatureShadows.applyToSettings(settings);
        parametersChanged = true;
        lastChangeTime = millis();
        SRP::setControlRequestsEventBits(SystemEvents::ControlRequest::SAVE_PARAMETERS);
        xEventGroupSetBits(storageEventGroup, STORAGE_SAVE_REQUEST_BIT);
    };
    
    // Set callbacks for all parameters
    // PID parameters
    storage->setOnChange("pid/spaceHeating/kp", paramChangeCallback);
    storage->setOnChange("pid/spaceHeating/ki", paramChangeCallback);
    storage->setOnChange("pid/spaceHeating/kd", paramChangeCallback);
    storage->setOnChange("pid/waterHeater/kp", paramChangeCallback);
    storage->setOnChange("pid/waterHeater/ki", paramChangeCallback);
    storage->setOnChange("pid/waterHeater/kd", paramChangeCallback);
    // Water heater parameters
    storage->setOnChange("wheater/priorityEnabled", paramChangeCallback);
    storage->setOnChange("wheater/heatingRate", paramChangeCallback);
    storage->setOnChange("wheater/tempLimitLow", paramChangeCallback);
    storage->setOnChange("wheater/tempLimitHigh", paramChangeCallback);
    storage->setOnChange("wheater/tempChargeDelta", paramChangeCallback);
    storage->setOnChange("wheater/tempSafeLimitHigh", paramChangeCallback);
    storage->setOnChange("wheater/tempSafeLimitLow", paramChangeCallback);
    // Heating parameters
    storage->setOnChange("heating/targetTemp", paramChangeCallback);
    storage->setOnChange("heating/curveShift", paramChangeCallback);
    storage->setOnChange("heating/curveCoeff", paramChangeCallback);
    storage->setOnChange("heating/burnerLowLimit", paramChangeCallback);
    storage->setOnChange("heating/highLimit", paramChangeCallback);
    storage->setOnChange("heating/hysteresis", paramChangeCallback);
    // PID auto-tuning parameters
    storage->setOnChange("pid/autotune/amplitude", paramChangeCallback);
    storage->setOnChange("pid/autotune/hysteresis", paramChangeCallback);
    storage->setOnChange("pid/autotune/method", paramChangeCallback);
    // Sensor offsets - use sensorOffsetCallback to sync int32_t shadows to Temperature_t
    storage->setOnChange("sensor/offset/boilerOutput", sensorOffsetCallback);
    storage->setOnChange("sensor/offset/boilerReturn", sensorOffsetCallback);
    storage->setOnChange("sensor/offset/waterTank", sensorOffsetCallback);
    storage->setOnChange("sensor/offset/waterOutput", sensorOffsetCallback);
    storage->setOnChange("sensor/offset/waterReturn", sensorOffsetCallback);
    storage->setOnChange("sensor/offset/heatingReturn", sensorOffsetCallback);
    storage->setOnChange("sensor/offset/outside", sensorOffsetCallback);
    storage->setOnChange("sensor/offset/room", sensorOffsetCallback);
    storage->setOnChange("sensor/offset/pressure", sensorOffsetCallback);

    // Load all saved parameters (auto-save defaults on first boot)
    LOG_INFO(TAG, "Loading saved parameters...");
    storage->loadAll(true);  // autoSaveDefaults = true

    // Apply loaded shadow values to Temperature_t fields
    temperatureShadows.applyToSettings(settings);

    // Restore system enable states from saved settings via StateManager
    // This allows system to remember user preferences across reboots
    LOG_INFO(TAG, "Restoring system enable states from NVS - Boiler:%s Heating:%s Water:%s Priority:%s",
             settings.boilerEnabled ? "EN" : "DIS",
             settings.heatingEnabled ? "EN" : "DIS",
             settings.waterEnabled ? "EN" : "DIS",
             settings.wheaterPriorityEnabled ? "EN" : "DIS");

    // StateManager atomically syncs settings to event bits
    StateManager::syncEnableStatesToEventBits();

    // Restore override event bits from persisted flags
    // These survive reboot for summer mode when manual valves are closed
    if (settings.heatingOverrideOff) {
        LOG_INFO(TAG, "Restoring HEATING_OFF_OVERRIDE from saved settings (summer mode)");
        SRP::setControlRequestsEventBits(SystemEvents::ControlRequest::HEATING_OFF_OVERRIDE);
    }
    if (settings.waterOverrideOff) {
        LOG_INFO(TAG, "Restoring WATER_OFF_OVERRIDE from saved settings");
        SRP::setControlRequestsEventBits(SystemEvents::ControlRequest::WATER_OFF_OVERRIDE);
    }

    // Track MQTT subscription state
    bool mqttSubscriptionsActive = false;
    bool mqttManagerSetup = false;  // Track if we've set up the storage's MQTT callbacks
    // Round 15 Issue #16 fix: Exponential backoff for subscription retries
    uint32_t subscribeBackoffMs = 1000;  // Start with 1 second
    constexpr uint32_t MAX_SUBSCRIBE_BACKOFF_MS = 60000;  // Max 60 seconds
    uint32_t lastSubscribeAttempt = 0;
    
    // Print NVS statistics
    size_t used, free, total;
    storage->getNvsStats(used, free, total);
    LOG_INFO(TAG, "NVS Stats - Used: %d, Free: %d, Total: %d", used, free, total);
    
    // Signal that persistent storage is ready
    // Set storage ready in general system event group
    EventGroupHandle_t generalSystemEventGroup = SRP::getGeneralSystemEventGroup();
    if (generalSystemEventGroup) {
        xEventGroupSetBits(generalSystemEventGroup, SystemEvents::GeneralSystem::STORAGE_READY);
    }
    
    // Setup lambda for MQTT subscriptions
    // Note: Caller already checked MQTT_OPERATIONAL bit, so we trust MQTT is connected
    // IMPORTANT: Don't capture mqttManager - get it fresh each time since it might be null at task start
    auto setupMqttSubscriptions = [storage, &mqttSubscriptionsActive, &mqttManagerSetup]() -> bool {
        // Get MQTTManager fresh - it might not have been available at task startup
        MQTTManager* mqttMgr = SRP::getMQTTManager();
        if (!mqttMgr) {
            LOG_ERROR(TAG, "MQTTManager not available yet!");
            return false;
        }

        // Log connection state for debugging
        bool isConn = mqttMgr->isConnected();
        LOG_INFO(TAG, "MQTTManager::isConnected() = %s", isConn ? "true" : "false");

        // Set up storage's MQTT manager and publish callback if not done yet
        if (!mqttManagerSetup) {
            LOG_INFO(TAG, "Setting up storage MQTT manager...");
            storage->setMqttManager(mqttMgr);

            // Set up publish callback to use MQTTTask's queue-based publishing with LOW priority
            storage->setMqttPublishCallback([](const char* topic, const char* payload, int qos, bool retain) -> bool {
                return MQTTTask::publish(topic, payload, qos, retain, MQTTPriority::PRIORITY_LOW);
            });
            mqttManagerSetup = true;
        }

        // Don't require isConnected() - we trust MQTT_OPERATIONAL bit from caller
        // The MQTTManager might report disconnected briefly during reconnection

        size_t freeHeap = ESP.getFreeHeap();
        if (freeHeap < 15000) {
            LOG_ERROR(TAG, "Low heap for MQTT subs: %d", freeHeap);
            return false;
        }

        LOG_INFO(TAG, "Setting up MQTT subscriptions...");

        // Subscribe to parameter topics (boiler/params/...)
        auto result1 = mqttMgr->subscribe("boiler/params/+/+",
            [storage](const String& topic, const String& payload) {
                LOG_INFO(TAG, "MQTT cmd: %s", topic.c_str());
                storage->handleMqttCommand(topic.c_str(), payload.c_str());
            });

        if (!result1.isOk()) {
            LOG_ERROR(TAG, "Sub +/+ fail");
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(200));

        auto result2 = mqttMgr->subscribe("boiler/params/+",
            [storage](const String& topic, const String& payload) {
                LOG_INFO(TAG, "MQTT cmd +: %s", topic.c_str());
                storage->handleMqttCommand(topic.c_str(), payload.c_str());
            });

        if (!result2.isOk()) {
            LOG_ERROR(TAG, "Sub + fail");
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(200));

        auto result3 = mqttMgr->subscribe("boiler/params/get/all",
            [storage](const String& topic, const String& payload) {
                LOG_INFO(TAG, "Get all cmd");
                storage->handleMqttCommand(topic.c_str(), payload.c_str());
            });

        if (!result3.isOk()) {
            LOG_ERROR(TAG, "Sub get/all fail");
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(200));

        // Subscribe to save command (already handled by PersistentStorage library)
        auto result4 = mqttMgr->subscribe("boiler/params/save",
            [storage](const String& topic, const String& payload) {
                LOG_INFO(TAG, "Save all parameters cmd");
                storage->handleMqttCommand(topic.c_str(), payload.c_str());
            });

        if (!result4.isOk()) {
            LOG_ERROR(TAG, "Sub save fail");
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(200));

        // Subscribe to save changed command (requires implementation in PersistentStorage library)
        auto result5 = mqttMgr->subscribe("boiler/params/save/changed",
            [storage](const String& topic, const String& payload) {
                LOG_INFO(TAG, "Save changed parameters cmd");
                // Note: parametersChanged tracking would need to be implemented in PersistentStorage
                // For now, just save all parameters
                storage->handleMqttCommand("boiler/params/save", "");
            });
            
        if (!result5.isOk()) {
            LOG_ERROR(TAG, "Sub save/changed fail");
            return false;
        }
        
        // Note: FRAM commands (fram/save, fram/restore) will be added after FRAM library integration
        LOG_INFO(TAG, "FRAM commands pending library implementation");
        
        mqttSubscriptionsActive = true;
        LOG_INFO(TAG, "MQTT subscriptions complete");
        return true;
    };
    
    // Removed automatic save timer - saves now triggered only by MQTT commands
    // This prevents unnecessary flash wear from periodic writes
    LOG_INFO(TAG, "Automatic saves disabled - use MQTT commands to save");
    
    // Main event-driven loop
    
    while (true) {
        // Wait for events with timeout for periodic checks
        EventBits_t bits = xEventGroupWaitBits(
            storageEventGroup,
            STORAGE_SAVE_REQUEST_BIT | STORAGE_LOAD_REQUEST_BIT | STORAGE_MQTT_RECONNECT_BIT,
            pdTRUE,  // Clear on exit
            pdFALSE, // Wait for any bit
            pdMS_TO_TICKS(5000)  // 5 second timeout for MQTT monitoring
        );
        
        // Check MQTT connection state
        EventBits_t mqttBits = SRP::getSystemStateEventBits();

        if ((mqttBits & SystemEvents::SystemState::MQTT_OPERATIONAL) && !mqttSubscriptionsActive) {
            // Round 15 Issue #16 fix: Exponential backoff for subscription retries
            uint32_t now = millis();
            if (now - lastSubscribeAttempt >= subscribeBackoffMs) {
                LOG_INFO(TAG, "MQTT connected, setting up subscriptions (backoff: %lu ms)", subscribeBackoffMs);
                lastSubscribeAttempt = now;
                if (setupMqttSubscriptions()) {
                    // Success - reset backoff
                    subscribeBackoffMs = 1000;
                } else {
                    // Failed - increase backoff with cap at MAX
                    subscribeBackoffMs = std::min(subscribeBackoffMs * 2, MAX_SUBSCRIBE_BACKOFF_MS);
                    LOG_WARN(TAG, "Subscription setup failed, next retry in %lu ms", subscribeBackoffMs);
                }
            }
        } else if (!(mqttBits & SystemEvents::SystemState::MQTT_OPERATIONAL) && mqttSubscriptionsActive) {
            LOG_INFO(TAG, "MQTT disconnected");
            mqttSubscriptionsActive = false;
            // Reset backoff on disconnect to allow quick reconnect
            subscribeBackoffMs = 1000;
        }

        // Process MQTT command queue - this is required for commands like save, get, set to work
        storage->processCommandQueue();

        // Handle save request - now only from MQTT commands
        if (bits & STORAGE_SAVE_REQUEST_BIT) {
            LOG_INFO(TAG, "Manual save requested via MQTT");
            storage->saveAll();
            
            // Update temperature shadows if needed
            temperatureShadows.applyToSettings(settings);
            
            // Clear changed flag after manual save
            parametersChanged = false;
        }
        
        // Handle load request
        if (bits & STORAGE_LOAD_REQUEST_BIT) {
            LOG_INFO(TAG, "Reloading parameters...");
            storage->loadAll();
            temperatureShadows.applyToSettings(settings);
            // Settings changed - no direct equivalent in new system, notify via control request
        SRP::setControlRequestsEventBits(SystemEvents::ControlRequest::SAVE_PARAMETERS);
        }
        
        // Handle MQTT reconnect
        if (bits & STORAGE_MQTT_RECONNECT_BIT) {
            if (!mqttSubscriptionsActive) {
                setupMqttSubscriptions();
            }
        }
    }
}

// Public function to request parameter save
void PersistentStorageTask_RequestSave() {
    if (storageEventGroup) {
        xEventGroupSetBits(storageEventGroup, STORAGE_SAVE_REQUEST_BIT);
    }
}

// Public function to request parameter reload
void PersistentStorageTask_RequestLoad() {
    if (storageEventGroup) {
        xEventGroupSetBits(storageEventGroup, STORAGE_LOAD_REQUEST_BIT);
    }
}