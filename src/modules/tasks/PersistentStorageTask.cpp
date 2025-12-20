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
#include "config/SafetyConfig.h"
#include <MQTTManager.h>
#include "events/SystemEventsGenerated.h"
#include <PersistentStorage.h>
#include "MQTTTask.h"
#include "modules/mqtt/MQTTCommandHandlers.h"
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

    // Initialize temperature shadows from current settings (constructor defaults)
    // This ensures new fields have proper defaults even if not in NVS
    temperatureShadows.initializeFromSettings(settings);

    // CRITICAL: Shadows must be initialized BEFORE registerTemperature()
    // If NVS parameter doesn't exist, PersistentStorage uses current shadow value

    // Register system settings parameters
    
    // Water heater configuration
    storage->registerBool("wheater/priorityEnabled", &settings.wheaterPriorityEnabled,
                          "Water heating priority over space heating");

    // Water tank temperature limits - Store as int32_t (tenths of degrees)
    static int32_t tankLow_i32 = static_cast<int32_t>(settings.wHeaterConfTempLimitLow);
    static int32_t tankHigh_i32 = static_cast<int32_t>(settings.wHeaterConfTempLimitHigh);
    static int32_t tankSafeHigh_i32 = static_cast<int32_t>(settings.wHeaterConfTempSafeLimitHigh);
    static int32_t tankSafeLow_i32 = static_cast<int32_t>(settings.wHeaterConfTempSafeLimitLow);

    storage->registerInt("wheater/tempLimitLow", &tankLow_i32, 300, 600, "Tank start heating (tenths °C)");
    storage->registerInt("wheater/tempLimitHigh", &tankHigh_i32, 500, 850, "Tank stop heating (tenths °C)");
    storage->registerInt("wheater/tempSafeLimitHigh", &tankSafeHigh_i32, 600, 950, "Tank safety max (tenths °C)");
    storage->registerInt("wheater/tempSafeLimitLow", &tankSafeLow_i32, 0, 100, "Tank safety min (tenths °C)");

    storage->setOnChange("wheater/tempLimitLow", [](const std::string& name, const void*) {
        SRP::getSystemSettings().wHeaterConfTempLimitLow = static_cast<Temperature_t>(tankLow_i32);
        LOG_INFO(TAG, "Parameter %s changed to %ld", name.c_str(), tankLow_i32);
        parametersChanged = true;
        lastChangeTime = millis();
        SRP::setControlRequestsEventBits(SystemEvents::ControlRequest::SAVE_PARAMETERS);
        xEventGroupSetBits(storageEventGroup, STORAGE_SAVE_REQUEST_BIT);
    });
    storage->setOnChange("wheater/tempLimitHigh", [](const std::string& name, const void*) {
        SRP::getSystemSettings().wHeaterConfTempLimitHigh = static_cast<Temperature_t>(tankHigh_i32);
        LOG_INFO(TAG, "Parameter %s changed to %ld", name.c_str(), tankHigh_i32);
        parametersChanged = true;
        lastChangeTime = millis();
        SRP::setControlRequestsEventBits(SystemEvents::ControlRequest::SAVE_PARAMETERS);
        xEventGroupSetBits(storageEventGroup, STORAGE_SAVE_REQUEST_BIT);
    });
    storage->setOnChange("wheater/tempSafeLimitHigh", [](const std::string& name, const void*) {
        SRP::getSystemSettings().wHeaterConfTempSafeLimitHigh = static_cast<Temperature_t>(tankSafeHigh_i32);
        LOG_INFO(TAG, "Parameter %s changed to %ld", name.c_str(), tankSafeHigh_i32);
        parametersChanged = true;
        lastChangeTime = millis();
        SRP::setControlRequestsEventBits(SystemEvents::ControlRequest::SAVE_PARAMETERS);
        xEventGroupSetBits(storageEventGroup, STORAGE_SAVE_REQUEST_BIT);
    });
    storage->setOnChange("wheater/tempSafeLimitLow", [](const std::string& name, const void*) {
        SRP::getSystemSettings().wHeaterConfTempSafeLimitLow = static_cast<Temperature_t>(tankSafeLow_i32);
        LOG_INFO(TAG, "Parameter %s changed to %ld", name.c_str(), tankSafeLow_i32);
        parametersChanged = true;
        lastChangeTime = millis();
        SRP::setControlRequestsEventBits(SystemEvents::ControlRequest::SAVE_PARAMETERS);
        xEventGroupSetBits(storageEventGroup, STORAGE_SAVE_REQUEST_BIT);
    });

    storage->registerFloat("wheater/tempChargeDelta", &settings.wHeaterConfTempChargeDelta,
                          1.0f, 20.0f, "Water heater charging temperature delta");

    storage->registerFloat("wheater/heatingRate", &settings.waterHeatingRate,
                          0.1f, 5.0f, "Water heating rate (°C per minute)");

    // Room target temperature - Store as int32_t
    static int32_t targetTemp_i32 = static_cast<int32_t>(settings.targetTemperatureInside);
    storage->registerInt("heating/targetTemp", &targetTemp_i32, 100, 300, "Room target (tenths °C)");
    storage->setOnChange("heating/targetTemp", [](const std::string& name, const void*) {
        SRP::getSystemSettings().targetTemperatureInside = static_cast<Temperature_t>(targetTemp_i32);
        LOG_INFO(TAG, "Parameter %s changed to %ld", name.c_str(), targetTemp_i32);
        parametersChanged = true;
        lastChangeTime = millis();
        SRP::setControlRequestsEventBits(SystemEvents::ControlRequest::SAVE_PARAMETERS);
        xEventGroupSetBits(storageEventGroup, STORAGE_SAVE_REQUEST_BIT);
    });
    
    storage->registerFloat("heating/curveShift", &settings.heating_curve_shift,
                          -20.0f, 40.0f, "Heating curve shift");
    
    storage->registerFloat("heating/curveCoeff", &settings.heating_curve_coeff,
                          0.5f, 4.0f, "Heating curve coefficient");
    
    // Global Burner Limits (All Modes) - Store as int32_t (tenths of degrees)
    // Hardcoded defaults ensure correct values when NVS is empty
    static int32_t burner_low_i32 = 380;   // 38.0°C default
    static int32_t burner_high_i32 = 1100; // 110.0°C default

    storage->registerInt("h/bLo", &burner_low_i32, 200, 700, "Burner min (tenths °C)");
    storage->registerInt("h/bHi", &burner_high_i32, 700, 1100, "Burner max (tenths °C)");

    storage->setOnChange("h/bLo", [](const std::string&, const void*) {
        auto& s = SRP::getSystemSettings();
        s.burner_low_limit = static_cast<Temperature_t>(burner_low_i32);
    });
    storage->setOnChange("h/bHi", [](const std::string&, const void*) {
        auto& s = SRP::getSystemSettings();
        s.burner_high_limit = static_cast<Temperature_t>(burner_high_i32);
    });

    // Space Heating Limits
    static int32_t heating_low_i32 = 400;  // 40.0°C default
    static int32_t heating_high_i32 = 750; // 75.0°C default

    storage->registerInt("h/sLo", &heating_low_i32, 300, 600, "Space heating min (tenths °C)");
    storage->registerInt("h/sHi", &heating_high_i32, 500, 900, "Space heating max (tenths °C)");

    storage->setOnChange("h/sLo", [](const std::string&, const void*) {
        auto& s = SRP::getSystemSettings();
        s.heating_low_limit = static_cast<Temperature_t>(heating_low_i32);
    });
    storage->setOnChange("h/sHi", [](const std::string&, const void*) {
        auto& s = SRP::getSystemSettings();
        s.heating_high_limit = static_cast<Temperature_t>(heating_high_i32);
    });

    // Water Heating Limits
    static int32_t water_low_i32 = 400;   // 40.0°C default
    static int32_t water_high_i32 = 900;  // 90.0°C default

    storage->registerInt("w/hLo", &water_low_i32, 300, 700, "Water heating min (tenths °C)");
    storage->registerInt("w/hHi", &water_high_i32, 600, 1000, "Water heating max (tenths °C)");

    storage->setOnChange("w/hLo", [](const std::string&, const void*) {
        auto& s = SRP::getSystemSettings();
        s.water_heating_low_limit = static_cast<Temperature_t>(water_low_i32);
    });
    storage->setOnChange("w/hHi", [](const std::string&, const void*) {
        auto& s = SRP::getSystemSettings();
        s.water_heating_high_limit = static_cast<Temperature_t>(water_high_i32);
    });

    // NOTE: Defaults will be applied to settings AFTER loadAll() runs

    // Heating hysteresis - Store as int32_t
    static int32_t hysteresis_i32 = static_cast<int32_t>(settings.heating_hysteresis);
    storage->registerInt("heating/hysteresis", &hysteresis_i32, 1, 20, "Heating hysteresis (tenths °C)");
    storage->setOnChange("heating/hysteresis", [](const std::string& name, const void*) {
        SRP::getSystemSettings().heating_hysteresis = static_cast<Temperature_t>(hysteresis_i32);
        LOG_INFO(TAG, "Parameter %s changed to %ld", name.c_str(), hysteresis_i32);
        parametersChanged = true;
        lastChangeTime = millis();
        SRP::setControlRequestsEventBits(SystemEvents::ControlRequest::SAVE_PARAMETERS);
        xEventGroupSetBits(storageEventGroup, STORAGE_SAVE_REQUEST_BIT);
    });
    
    // PID parameters for space heating (wider range to accommodate autotune results)
    storage->registerFloat("pid/spaceHeating/kp", &settings.spaceHeatingKp,
                          0.0f, 100.0f, "Space heating PID proportional gain");

    storage->registerFloat("pid/spaceHeating/ki", &settings.spaceHeatingKi,
                          0.0f, 10.0f, "Space heating PID integral gain");

    storage->registerFloat("pid/spaceHeating/kd", &settings.spaceHeatingKd,
                          0.0f, 50.0f, "Space heating PID derivative gain");
    
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

    // Weather-compensated heating control
    storage->registerBool("heating/weatherControl", &settings.useWeatherCompensatedControl,
                          "Weather-compensated control (outside temp determines ON/OFF)");

    // Weather control Temperature_t parameters - Store as int32_t (tenths of degrees)
    static int32_t outsideHeatingThreshold_i32 = static_cast<int32_t>(settings.outsideTempHeatingThreshold);
    static int32_t roomOverheatMargin_i32 = static_cast<int32_t>(settings.roomTempOverheatMargin);

    storage->registerInt("heating/outsideThreshold", &outsideHeatingThreshold_i32, 50, 200,
                         "Outside temp heating threshold (tenths °C, 5-20°C)");
    storage->registerInt("heating/roomOverheatMargin", &roomOverheatMargin_i32, 10, 50,
                         "Room overheat margin (tenths °C, 1-5°C)");

    storage->setOnChange("heating/outsideThreshold", [](const std::string& name, const void*) {
        SRP::getSystemSettings().outsideTempHeatingThreshold = static_cast<Temperature_t>(outsideHeatingThreshold_i32);
        LOG_INFO(TAG, "Parameter %s changed to %ld", name.c_str(), outsideHeatingThreshold_i32);
        parametersChanged = true;
        lastChangeTime = millis();
        SRP::setControlRequestsEventBits(SystemEvents::ControlRequest::SAVE_PARAMETERS);
        xEventGroupSetBits(storageEventGroup, STORAGE_SAVE_REQUEST_BIT);
    });
    storage->setOnChange("heating/roomOverheatMargin", [](const std::string& name, const void*) {
        SRP::getSystemSettings().roomTempOverheatMargin = static_cast<Temperature_t>(roomOverheatMargin_i32);
        LOG_INFO(TAG, "Parameter %s changed to %ld", name.c_str(), roomOverheatMargin_i32);
        parametersChanged = true;
        lastChangeTime = millis();
        SRP::setControlRequestsEventBits(SystemEvents::ControlRequest::SAVE_PARAMETERS);
        xEventGroupSetBits(storageEventGroup, STORAGE_SAVE_REQUEST_BIT);
    });

    // Room temp curve shift factor (float, can be registered directly)
    storage->registerFloat("heating/roomCurveShiftFactor", &settings.roomTempCurveShiftFactor,
                          1.0f, 4.0f, "Room temp curve shift factor (1.0-4.0)");

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
    auto floatParamCallback = [](const std::string& name, const void* value) {
        float val = *(const float*)value;
        LOG_INFO(TAG, "Parameter %s changed to %.2f", name.c_str(), val);
        parametersChanged = true;
        lastChangeTime = millis();
        SRP::setControlRequestsEventBits(SystemEvents::ControlRequest::SAVE_PARAMETERS);
        xEventGroupSetBits(storageEventGroup, STORAGE_SAVE_REQUEST_BIT);
    };

    auto boolParamCallback = [](const std::string& name, const void* value) {
        bool val = *(const bool*)value;
        LOG_INFO(TAG, "Parameter %s changed to %s", name.c_str(), val ? "true" : "false");
        parametersChanged = true;
        lastChangeTime = millis();
        SRP::setControlRequestsEventBits(SystemEvents::ControlRequest::SAVE_PARAMETERS);
        xEventGroupSetBits(storageEventGroup, STORAGE_SAVE_REQUEST_BIT);
    };

    // Generic callback for unknown types (fallback)
    auto paramChangeCallback = [](const std::string& name, const void* value) {
        (void)value;  // Type unknown, can't log value
        LOG_INFO(TAG, "Parameter %s changed", name.c_str());
        parametersChanged = true;
        lastChangeTime = millis();
        SRP::setControlRequestsEventBits(SystemEvents::ControlRequest::SAVE_PARAMETERS);
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
    // PID parameters (floats)
    storage->setOnChange("pid/spaceHeating/kp", floatParamCallback);
    storage->setOnChange("pid/spaceHeating/ki", floatParamCallback);
    storage->setOnChange("pid/spaceHeating/kd", floatParamCallback);
    storage->setOnChange("pid/waterHeater/kp", floatParamCallback);
    storage->setOnChange("pid/waterHeater/ki", floatParamCallback);
    storage->setOnChange("pid/waterHeater/kd", floatParamCallback);
    // Water heater parameters (floats only - int32 callbacks set inline)
    storage->setOnChange("wheater/priorityEnabled", boolParamCallback);
    storage->setOnChange("wheater/heatingRate", floatParamCallback);
    storage->setOnChange("wheater/tempChargeDelta", floatParamCallback);
    // Note: tempLimitLow/High, tempSafeLimitLow/High are int32 with inline callbacks
    // Heating parameters (floats only - int32 callbacks set inline)
    storage->setOnChange("heating/curveShift", floatParamCallback);
    storage->setOnChange("heating/curveCoeff", floatParamCallback);
    // Note: targetTemp, hysteresis are int32 with inline callbacks
    // Note: burnerLowLimit, highLimit don't exist - they're h/bLo, h/bHi, h/sLo, h/sHi
    // PID auto-tuning parameters
    storage->setOnChange("pid/autotune/amplitude", floatParamCallback);
    storage->setOnChange("pid/autotune/hysteresis", floatParamCallback);
    storage->setOnChange("pid/autotune/method", paramChangeCallback);  // int32
    // Weather-compensated heating control
    storage->setOnChange("heating/weatherControl", boolParamCallback);
    storage->setOnChange("heating/roomCurveShiftFactor", floatParamCallback);
    // Note: outsideThreshold and roomOverheatMargin callbacks set inline during registration
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

    // CRITICAL: Re-apply int32_t defaults after load() for parameters not in NVS
    // load() zeros values for missing keys, so restore defaults here
    if (burner_low_i32 == 0) burner_low_i32 = 380;
    if (burner_high_i32 == 0) burner_high_i32 = 1100;
    if (heating_low_i32 == 0) heating_low_i32 = 400;
    if (heating_high_i32 == 0) heating_high_i32 = 750;
    if (water_low_i32 == 0) water_low_i32 = 400;
    if (water_high_i32 == 0 || water_high_i32 == 1) water_high_i32 = 900;
    // Weather control defaults (15.0°C threshold, 2.0°C overheat margin)
    if (outsideHeatingThreshold_i32 == 0) outsideHeatingThreshold_i32 = 150;
    if (roomOverheatMargin_i32 == 0) roomOverheatMargin_i32 = 20;

    // Apply restored defaults to settings
    settings.burner_low_limit = static_cast<Temperature_t>(burner_low_i32);
    settings.burner_high_limit = static_cast<Temperature_t>(burner_high_i32);
    settings.heating_low_limit = static_cast<Temperature_t>(heating_low_i32);
    settings.heating_high_limit = static_cast<Temperature_t>(heating_high_i32);
    settings.water_heating_low_limit = static_cast<Temperature_t>(water_low_i32);
    settings.water_heating_high_limit = static_cast<Temperature_t>(water_high_i32);
    settings.outsideTempHeatingThreshold = static_cast<Temperature_t>(outsideHeatingThreshold_i32);
    settings.roomTempOverheatMargin = static_cast<Temperature_t>(roomOverheatMargin_i32);

    LOG_INFO(TAG, "Temperature limits after load: burner[%d-%d] heating[%d-%d] water[%d-%d]",
             burner_low_i32, burner_high_i32, heating_low_i32, heating_high_i32, water_low_i32, water_high_i32);

    // Load safety configuration from NVS (separate namespace)
    SafetyConfig::loadFromNVS();

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

        // Guard: Verify connection before proceeding (prevents TOCTOU race with MQTT_OPERATIONAL bit)
        bool isConn = mqttMgr->isConnected();
        if (!isConn) {
            LOG_WARN(TAG, "MQTT not connected - deferring subscription setup");
            return false;
        }

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

        // Note: Connection verified at function entry (TOCTOU guard)

        // H14: Use standardized heap threshold - need buffer for MQTT subscriptions
        size_t freeHeap = ESP.getFreeHeap();
        if (freeHeap < SystemConstants::System::MIN_FREE_HEAP_WARNING) {
            LOG_ERROR(TAG, "Low heap for MQTT subs: %d", freeHeap);
            return false;
        }

        LOG_INFO(TAG, "Setting up MQTT subscriptions...");

        // Subscribe to parameter topics (boiler/params/...)
        // Use # wildcard to match all commands: set/*, get/*, save, list, etc.
        // NOTE: Single wildcard subscription handles everything - no specific subs needed
        auto result1 = mqttMgr->subscribe("boiler/params/#",
            [storage](const String& topic, const String& payload) {
                LOG_INFO(TAG, "MQTT cmd: %s", topic.c_str());
                storage->handleMqttCommand(topic.c_str(), payload.c_str());
            });

        if (!result1.isOk()) {
            LOG_ERROR(TAG, "Sub params/# fail");
            return false;
        }

        mqttSubscriptionsActive = true;
        LOG_INFO(TAG, "MQTT subscriptions complete");

        // Publish initial safety configuration
        MQTTCommandHandlers::publishSafetyConfig();

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
            pdMS_TO_TICKS(1000)  // 1 second timeout for MQTT monitoring
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