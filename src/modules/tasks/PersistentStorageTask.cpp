// src/modules/tasks/PersistentStorageTask.cpp
// Persistent storage task - handles parameter save/load operations
#include "PersistentStorageTask.h"
#include "modules/control/AutotuneRelayConfig.h"  // autotune setting ranges
#include "modules/control/WaterChargePolicy.h"    // charge delta bounds
#include <TaskManager.h>
#include "LoggingMacros.h"
#include "core/SystemResourceProvider.h"
#include <algorithm>  // Round 15 Issue #16: For std::min in exponential backoff
#include "shared/SharedSensorReadings.h"
#include "shared/SharedRelayReadings.h"
#include "config/SystemSettings.h"
#include "config/SystemConstants.h"
#include <esp_heap_caps.h>
#include "modules/tasks/ParameterSavePolicy.h"
#include "modules/tasks/SensorOffsetMap.h"  // persisted offsets -> MB8ART channel order
#include <MB8ART.h>                         // setChannelOffsets()
#include "config/SafetyConfig.h"
#include <MQTTManager.h>
#include "events/SystemEventsGenerated.h"
#include <PersistentStorage.h>
#include "MQTTTask.h"
#include "modules/mqtt/MQTTCommandHandlers.h"
#include "MQTTTopics.h"  // MQTT_STATUS_HEATING for the room-target status republish
#include "utils/ErrorLogFRAM.h"
#include "utils/TemperatureParameterWrapper.h"
#include "core/StateManager.h"
#include <atomic>
#include <string>

// Global temperature shadows for SystemSettings
static SystemSettingsTemperatureShadows temperatureShadows;

/// Formats tenths of a degree as "<whole>.<tenth>", e.g. 192 -> "19.2".
///
/// Hand-rolled rather than calling formatTemp(), which reaches snprintf("%d.%d") and so
/// pulls newlib's ~800 B _svfprintf_r frame onto whichever task formats. (Integer
/// conversions only - no float formatting - but the frame is the same.) Whether that
/// costs any stack BOUND here depends on processCommandQueue, 1328 B, staying the
/// deepest path on this task: today it is, so the honest saving is 0 B, and the point is
/// not to depend on that. Same reasoning as buildStatusTopic()/buildHashKey() in
/// ESP32-PersistentStorage.
///
/// Precondition: the room target is validated to 100..300 tenths by every writer, so
/// TEMP_INVALID is not handled. Truncates and always NUL-terminates. Needs 8 bytes for
/// the widest output of interest (sign + 3 digits + '.' + digit + NUL).
static void formatTenths(char* out, size_t size, int32_t tenths) {
    if (size == 0) return;
    if (size < 8) { out[0] = '\0'; return; }  // caller's buffer is too small to be safe
    size_t len = 0;
    if (tenths < 0) {
        out[len++] = '-';
        tenths = -tenths;
    }
    int32_t whole = tenths / 10;
    const int32_t frac = tenths % 10;
    char digits[10];
    size_t n = 0;
    do {
        digits[n++] = static_cast<char>('0' + (whole % 10));
        whole /= 10;
    } while (whole != 0 && n < sizeof(digits));
    while (n > 0) out[len++] = digits[--n];
    out[len++] = '.';
    out[len++] = static_cast<char>('0' + frac);
    out[len] = '\0';
}

// Syslog configuration shadows (int32_t shadows for uint8_t/uint16_t fields)
static int32_t syslog_serverIP0 = 192;
static int32_t syslog_serverIP1 = 168;
static int32_t syslog_serverIP2 = 20;
static int32_t syslog_serverIP3 = 27;
static int32_t syslog_port = 514;
static int32_t syslog_facility = 16;  // LOCAL0
static int32_t syslog_minLevel = 2;   // ESP_LOG_WARN

// Preheating and pump overrun shadows (boiler/cmd/config/preheat_*, pump_cooldown_ms).
// These were written to SystemSettings only and lost on reboot (2026-09-15)
static int32_t preheatOffMultiplier_i32 = 5;
static int32_t preheatMaxCycles_i32 = 8;
static int32_t preheatTimeoutMs_i32 = 600000;
static int32_t preheatPumpMinMs_i32 = 3000;
static int32_t preheatSafeDiff_i32 = 250;
static int32_t pumpCooldownMs_i32 = 300000;

static void applyPreheatPumpShadows(SystemSettings& settings) {
    settings.preheatOffMultiplier = static_cast<uint8_t>(preheatOffMultiplier_i32);
    settings.preheatMaxCycles = static_cast<uint8_t>(preheatMaxCycles_i32);
    settings.preheatTimeoutMs = static_cast<uint32_t>(preheatTimeoutMs_i32);
    settings.preheatPumpMinMs = static_cast<uint16_t>(preheatPumpMinMs_i32);
    settings.preheatSafeDiff = static_cast<Temperature_t>(preheatSafeDiff_i32);
    settings.pumpCooldownMs = static_cast<uint32_t>(pumpCooldownMs_i32);
}

// Constants - use centralized values
static const char* TAG = "PersistentStorageTask";
namespace StorageConstants = SystemConstants::Tasks::Storage;
static const uint32_t IMMEDIATE_SAVE_DELAY_MS = 1000; // 1 second debounce

// Event bits for storage operations
#define STORAGE_SAVE_REQUEST_BIT    (1UL << 0UL)
#define STORAGE_LOAD_REQUEST_BIT    (1UL << 1UL)
#define STORAGE_MQTT_RECONNECT_BIT  (1UL << 2UL)
// Set when an MQTT parameter command has been queued, so the task drains it at
// once instead of up to 1 s later on the event-wait timeout. Without this, a
// client burst (boiler/params/get/all plus single reads) overran the 5-slot
// command queue and xQueueSend dropped the NEWEST commands: measured 2026-09-16
// as 4 of 8 single reads silently lost, the last four fired (see CHANGELOG)
#define STORAGE_COMMAND_PENDING_BIT (1UL << 3UL)

// Static event group for storage task
static EventGroupHandle_t storageEventGroup = nullptr;

// Track if parameters have changed
static bool parametersChanged = false;
static uint32_t lastChangeTime = 0;

// Storage instance for PersistentStorageTask_SetParameter() (called from the MQTT task)
static std::atomic<PersistentStorage*> storageInstance{nullptr};

// Set once the saved parameters are loaded and applied to SystemSettings
static std::atomic<bool> parametersLoaded{false};

// Task function
// Push the persisted per-channel calibration offsets into the MB8ART driver.
//
// Until 2026-10-19 the seven temperature offsets were registered, persisted and published
// but applied to nothing: the library writes temperatures straight into
// SharedSensorReadings through the pointers bound in SensorBindings.cpp, and its own
// correction hook was a stub with a hardcoded zero and no callers. ESP32-MB8ART b917db6
// added setChannelOffsets(); this is the consumer side.
//
// The library validates at set time (index and magnitude only) and gates at apply time on
// the channel having a binding AND a temperature mode, so the pressure channel is refused
// there regardless of what we send - SensorOffsetMap never sends one for it either.
// Offsets for channels that are disabled (CH6/CH7 here) are stored and never applied.
//
// Called on every offset change and after each load. There is no retry: MB8ART is
// initialised in initializeModbusDevices() before this task starts, so the device is
// normally present by the time parameters load; if its boot init failed outright there are
// no temperature readings to correct anyway. The warning below is the signal for that case.
static bool pushSensorOffsetsToMB8ART(const SystemSettings& s) {
    MB8ART* device = SRP::getMB8ART();
    if (device == nullptr) {
        LOG_WARN(TAG, "MB8ART unavailable - sensor offsets not applied");
        return false;
    }

    SensorOffsetMap::Offsets offsets;
    offsets.boilerOutput  = s.boilerOutputOffset;
    offsets.boilerReturn  = s.boilerReturnOffset;
    offsets.waterTank     = s.waterTankOffset;
    offsets.outside       = s.outsideTempOffset;
    offsets.waterReturn   = s.waterReturnOffset;
    offsets.heatingReturn = s.heatingReturnOffset;

    if (!device->setChannelOffsets(SensorOffsetMap::build(offsets))) {
        LOG_ERROR(TAG, "MB8ART rejected sensor offsets");
        return false;
    }
    return true;
}

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
    storageInstance.store(storage);

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

    // Initialize syslog shadows from current settings
    syslog_serverIP0 = settings.syslogServerIP[0];
    syslog_serverIP1 = settings.syslogServerIP[1];
    syslog_serverIP2 = settings.syslogServerIP[2];
    syslog_serverIP3 = settings.syslogServerIP[3];
    syslog_port = settings.syslogPort;
    syslog_facility = settings.syslogFacility;
    syslog_minLevel = settings.syslogMinLevel;

    // Initialize preheating / pump overrun shadows from current settings
    preheatOffMultiplier_i32 = settings.preheatOffMultiplier;
    preheatMaxCycles_i32 = settings.preheatMaxCycles;
    preheatTimeoutMs_i32 = static_cast<int32_t>(settings.preheatTimeoutMs);
    preheatPumpMinMs_i32 = settings.preheatPumpMinMs;
    preheatSafeDiff_i32 = settings.preheatSafeDiff;
    pumpCooldownMs_i32 = static_cast<int32_t>(settings.pumpCooldownMs);

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
    });
    storage->setOnChange("wheater/tempLimitHigh", [](const std::string& name, const void*) {
        SRP::getSystemSettings().wHeaterConfTempLimitHigh = static_cast<Temperature_t>(tankHigh_i32);
        LOG_INFO(TAG, "Parameter %s changed to %ld", name.c_str(), tankHigh_i32);
        parametersChanged = true;
        lastChangeTime = millis();
    });
    storage->setOnChange("wheater/tempSafeLimitHigh", [](const std::string& name, const void*) {
        SRP::getSystemSettings().wHeaterConfTempSafeLimitHigh = static_cast<Temperature_t>(tankSafeHigh_i32);
        LOG_INFO(TAG, "Parameter %s changed to %ld", name.c_str(), tankSafeHigh_i32);
        parametersChanged = true;
        lastChangeTime = millis();
    });
    storage->setOnChange("wheater/tempSafeLimitLow", [](const std::string& name, const void*) {
        SRP::getSystemSettings().wHeaterConfTempSafeLimitLow = static_cast<Temperature_t>(tankSafeLow_i32);
        LOG_INFO(TAG, "Parameter %s changed to %ld", name.c_str(), tankSafeLow_i32);
        parametersChanged = true;
        lastChangeTime = millis();
    });

    // Bounds from WaterChargePolicy so the storage layer rejects what the use site would
    // reject. These disagreed until 2026-10-23 (registered 1.0, enforced 5.0), so a
    // params/set of 1.0-4.9 was acknowledged and persisted but then ignored at use.
    storage->registerFloat("wheater/tempChargeDelta", &settings.wHeaterConfTempChargeDelta,
                          WaterChargePolicy::CHARGE_DELTA_MIN,
                          WaterChargePolicy::CHARGE_DELTA_MAX,
                          "Water heater charging temperature delta");

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
    // 38.0°C: the space-heating flow floor. At the previous 40.0 the heating curve was
    // clamped flat for every outside temperature above about 10.5 °C, so weather
    // compensation did nothing for most of the shoulder season. The Vaillant VK 42/4-2 is a
    // low-temperature boiler with a 35 °C minimum flow (docs/EQUIPMENT_SPECS.md:52-72); 38
    // matches h/bLo and leaves the manufacturer margin intact (2026-10-19).
    static int32_t heating_low_i32 = 380;
    static int32_t heating_high_i32 = 750; // 75.0°C default

    storage->registerInt("h/sLo", &heating_low_i32, 300, 600, "Space heating min (tenths °C)");
    storage->registerInt("h/sHi", &heating_high_i32, 500, 900, "Space heating max (tenths °C)");

    // Lowest space-heating flow target allowed to use stage 2 (POWER_BOOST, ~34 kW measured).
    // 2026-10-19: measured 11 minutes of full power chasing a 38.0 °C setpoint at 11.5 °C
    // outside, because the bang-bang asks for FULL whenever flow is >10 °C below target and
    // a cold radiator circuit always is. Rationale and the choice of target-over-outside in
    // BurnerBoostPolicy.h. The range ends are the degenerate cases: 380 (= h/sLo) lets every
    // heating target boost, 750 (= h/sHi default) is effectively never.
    static int32_t boostMinTarget_i32 = 500;

    storage->registerInt("h/boostMin", &boostMinTarget_i32, 380, 750,
                         "Min flow target for stage 2, heating only (tenths °C)");

    storage->setOnChange("h/sLo", [](const std::string&, const void*) {
        auto& s = SRP::getSystemSettings();
        s.heating_low_limit = static_cast<Temperature_t>(heating_low_i32);
    });
    storage->setOnChange("h/sHi", [](const std::string&, const void*) {
        auto& s = SRP::getSystemSettings();
        s.heating_high_limit = static_cast<Temperature_t>(heating_high_i32);
    });
    storage->setOnChange("h/boostMin", [](const std::string&, const void*) {
        auto& s = SRP::getSystemSettings();
        s.heating_boost_min_target = static_cast<Temperature_t>(boostMinTarget_i32);
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
    });
    
    // PID parameters for space heating (wider range to accommodate autotune results)
    storage->registerFloat("pid/spaceHeating/kp", &settings.spaceHeatingKp,
                          0.0f, 100.0f, "Space heating PID proportional gain");

    storage->registerFloat("pid/spaceHeating/ki", &settings.spaceHeatingKi,
                          0.0f, 10.0f, "Space heating PID integral gain");

    storage->registerFloat("pid/spaceHeating/kd", &settings.spaceHeatingKd,
                          0.0f, 50.0f, "Space heating PID derivative gain");
    
    // PID parameters for water heating (same ranges as space heating: the same
    // boiler loop is controlled, and relay autotune gives e.g. Kp ~23)
    storage->registerFloat("pid/waterHeater/kp", &settings.wHeaterKp,
                          0.0f, 100.0f, "Water heater PID proportional gain");

    storage->registerFloat("pid/waterHeater/ki", &settings.wHeaterKi,
                          0.0f, 10.0f, "Water heater PID integral gain");

    storage->registerFloat("pid/waterHeater/kd", &settings.wHeaterKd,
                          0.0f, 50.0f, "Water heater PID derivative gain");

    // PID auto-tuning configuration
    storage->registerFloat("pid/autotune/amplitude", &settings.autotuneRelayAmplitude,
                          AutotuneRelayConfig::AMPLITUDE_MIN, AutotuneRelayConfig::AMPLITUDE_MAX,
                          "Auto-tune relay output half-swing (%), 50 = OFF/FULL two-stage burner");

    storage->registerFloat("pid/autotune/hysteresis", &settings.autotuneHysteresis,
                          AutotuneRelayConfig::HYSTERESIS_MIN, AutotuneRelayConfig::HYSTERESIS_MAX,
                          "Auto-tune relay switching band around the setpoint (°C)");

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

    storage->registerInt("heating/outsideThreshold", &outsideHeatingThreshold_i32, 50, 250,
                         "Outside temp heating threshold (tenths °C, 5-25°C)");
    storage->registerInt("heating/roomOverheatMargin", &roomOverheatMargin_i32, 10, 50,
                         "Room overheat margin (tenths °C, 1-5°C)");

    storage->setOnChange("heating/outsideThreshold", [](const std::string& name, const void*) {
        SRP::getSystemSettings().outsideTempHeatingThreshold = static_cast<Temperature_t>(outsideHeatingThreshold_i32);
        LOG_INFO(TAG, "Parameter %s changed to %ld", name.c_str(), outsideHeatingThreshold_i32);
        parametersChanged = true;
        lastChangeTime = millis();
    });
    storage->setOnChange("heating/roomOverheatMargin", [](const std::string& name, const void*) {
        SRP::getSystemSettings().roomTempOverheatMargin = static_cast<Temperature_t>(roomOverheatMargin_i32);
        LOG_INFO(TAG, "Parameter %s changed to %ld", name.c_str(), roomOverheatMargin_i32);
        parametersChanged = true;
        lastChangeTime = millis();
    });

    // Room temp curve shift factor (float, can be registered directly)
    storage->registerFloat("heating/roomCurveShiftFactor", &settings.roomTempCurveShiftFactor,
                          1.0f, 4.0f, "Room temp curve shift factor (1.0-4.0)");

    // Preheating, pump overrun and boiler PID mode (set via boiler/cmd/config/*)
    storage->registerBool("preheat/enabled", &settings.preheatEnabled, "Return preheating enabled");
    storage->registerInt("preheat/offMultiplier", &preheatOffMultiplier_i32, 1, 10,
                         "Preheat OFF duration = ON duration x this");
    storage->registerInt("preheat/maxCycles", &preheatMaxCycles_i32, 1, 20, "Preheat max pump cycles");
    storage->registerInt("preheat/timeoutMs", &preheatTimeoutMs_i32,
                         static_cast<int32_t>(SystemConstants::Safety::ConfigValidation::PREHEAT_TIMEOUT_MIN_MS),
                         static_cast<int32_t>(SystemConstants::Safety::ConfigValidation::PREHEAT_TIMEOUT_MAX_MS),
                         "Preheat timeout (ms)");
    storage->registerInt("preheat/pumpMinMs", &preheatPumpMinMs_i32,
                         static_cast<int32_t>(SystemConstants::Safety::ConfigValidation::PREHEAT_PUMP_MIN_MS_MIN),
                         static_cast<int32_t>(SystemConstants::Safety::ConfigValidation::PREHEAT_PUMP_MIN_MS_MAX),
                         "Preheat min time between pump changes (ms)");
    storage->registerInt("preheat/safeDiff", &preheatSafeDiff_i32, 100, 300,
                         "Preheat exit differential (tenths °C)");
    storage->registerInt("pump/cooldownMs", &pumpCooldownMs_i32, 60000, 900000,
                         "Pump overrun after burner off (ms)");
    storage->registerBool("boiler/pidEnabled", &settings.useBoilerTempPID,
                          "Boiler temperature PID mode (reboot required)");

    auto preheatPumpCallback = [](const std::string& name, const void*) {
        applyPreheatPumpShadows(SRP::getSystemSettings());
        LOG_INFO(TAG, "Parameter %s changed", name.c_str());
        parametersChanged = true;
        lastChangeTime = millis();
    };
    storage->setOnChange("preheat/offMultiplier", preheatPumpCallback);
    storage->setOnChange("preheat/maxCycles", preheatPumpCallback);
    storage->setOnChange("preheat/timeoutMs", preheatPumpCallback);
    storage->setOnChange("preheat/pumpMinMs", preheatPumpCallback);
    storage->setOnChange("preheat/safeDiff", preheatPumpCallback);
    storage->setOnChange("pump/cooldownMs", preheatPumpCallback);

    // Sensor compensation offsets (MB8ART channels) - int32_t shadows, values in tenths of °C
    // MQTT: Send integer value e.g., -14 for -1.4°C offset, 5 for +0.5°C offset
    //
    // Channel numbers are from include/config/SensorIndices.h. Four of the seven
    // descriptions below were wrong until 2026-10-19 (outside said CH6, which is
    // WATER_RETURN and hardware-deactivated). These strings are published by
    // params/list, so a wrong one actively misleads.
    //
    // NOTE: none of these seven reach a reading yet - the MB8ART library owns the
    // pointers it writes (SensorBindings.cpp). See SystemSettingsStruct.h.
    storage->registerInt("sensor/offset/boilerOutput", &temperatureShadows.boilerOutputOffset,
                          -50, 50, "Boiler output offset (tenths °C, CH0)");
    storage->registerInt("sensor/offset/boilerReturn", &temperatureShadows.boilerReturnOffset,
                          -50, 50, "Boiler return offset (tenths °C, CH1)");
    storage->registerInt("sensor/offset/waterTank", &temperatureShadows.waterTankOffset,
                          -50, 50, "Water tank offset (tenths °C, CH2)");
    storage->registerInt("sensor/offset/waterReturn", &temperatureShadows.waterReturnOffset,
                          -50, 50, "Water return offset (tenths °C, CH6)");
    storage->registerInt("sensor/offset/heatingReturn", &temperatureShadows.heatingReturnOffset,
                          -50, 50, "Heating return offset (tenths °C, CH7)");
    storage->registerInt("sensor/offset/outside", &temperatureShadows.outsideTempOffset,
                          -50, 50, "Outside temp offset (tenths °C, CH3)");
    // ANDRTF3 room temperature
    storage->registerInt("sensor/offset/room", &temperatureShadows.roomTempOffset,
                          -50, 50, "Room temp offset (tenths °C, ANDRTF3)");
    // Pressure sensor - hundredths of BAR (e.g., -5 = -0.05 BAR)
    storage->registerInt("sensor/offset/pressure", &temperatureShadows.pressureOffset,
                          -50, 50, "Pressure offset (hundredths BAR)");

    // Note: Sensor intervals are now compile-time constants in SystemConstants::Timing
    // (MB8ART_SENSOR_READ_INTERVAL_MS, ANDRTF3_SENSOR_READ_INTERVAL_MS)

    // Syslog configuration (remote logging)
    // Note: syslogEnabled uses bool directly, others use int32_t shadows
    storage->registerBool("syslog/enabled", &settings.syslogEnabled,
                          "Syslog forwarding enabled");
    storage->registerInt("syslog/serverIP0", &syslog_serverIP0, 0, 255, "Syslog server IP octet 0");
    storage->registerInt("syslog/serverIP1", &syslog_serverIP1, 0, 255, "Syslog server IP octet 1");
    storage->registerInt("syslog/serverIP2", &syslog_serverIP2, 0, 255, "Syslog server IP octet 2");
    storage->registerInt("syslog/serverIP3", &syslog_serverIP3, 0, 255, "Syslog server IP octet 3");
    storage->registerInt("syslog/port", &syslog_port, 1, 65535, "Syslog port");
    storage->registerInt("syslog/facility", &syslog_facility, 0, 23, "Syslog facility (LOCAL0=16)");
    storage->registerInt("syslog/minLevel", &syslog_minLevel, 0, 5, "Syslog min level (0=NONE, 2=WARN)");

    // Callbacks for parameter changes
    auto floatParamCallback = [](const std::string& name, const void* value) {
        float val = *(const float*)value;
        LOG_INFO(TAG, "Parameter %s changed to %.2f", name.c_str(), val);
        parametersChanged = true;
        lastChangeTime = millis();
    };

    auto boolParamCallback = [](const std::string& name, const void* value) {
        bool val = *(const bool*)value;
        LOG_INFO(TAG, "Parameter %s changed to %s", name.c_str(), val ? "true" : "false");
        parametersChanged = true;
        lastChangeTime = millis();
    };

    // Generic callback for unknown types (fallback)
    auto paramChangeCallback = [](const std::string& name, const void* value) {
        (void)value;  // Type unknown, can't log value
        LOG_INFO(TAG, "Parameter %s changed", name.c_str());
        parametersChanged = true;
        lastChangeTime = millis();
    };

    // Callback for sensor offset parameters - syncs int32_t shadow to Temperature_t immediately
    auto sensorOffsetCallback = [&settings](const std::string& name, const void* value) {
        LOG_INFO(TAG, "Sensor offset %s changed", name.c_str());
        // Sync all offset shadows to settings (efficient - just integer casts)
        temperatureShadows.applyToSettings(settings);
        pushSensorOffsetsToMB8ART(settings);
        parametersChanged = true;
        lastChangeTime = millis();
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
    // The enable/priority/override bools below used to write their settings field (and
    // NVS) and nothing else - wheater/priorityEnabled got a log-only callback and the five
    // system/* ones had NO callback at all. But the control path reads the EVENT BIT, and
    // the bits are synced from settings only at boot: syncEnableStatesToEventBits() has
    // two callers, both on the init path, and its inverse syncEventBitsToSettings() has
    // none. So a parameter set left the two stores disagreeing until the next reboot.
    //
    // For water priority that is worse than merely ineffective, because the two stores
    // have different readers: HeatingControlTask.cpp:250 reads the FIELD while
    // BurnerStateMachine.cpp:376/507/754 read the BIT. One set from the add-on's Water tab
    // therefore put those two tasks on opposite answers about whether water has priority.
    //
    // Routed through the same StateManager setters boiler/cmd/* already uses, so field and
    // bit move together and the side effects run (setBoilerEnabled(false) ends a running
    // water charge; the override setters also move the HEATING/WATER_*_OVERRIDE control
    // bits). persist=false because PersistentStorage::setJson() has already written the
    // key - markSettingsDirty() here would only queue a redundant save.
    //
    // The override pair worked before this change (their consumers read the field:
    // BurnerStateMachine.cpp:403, HeatingControlTask.cpp:519, WheaterControlTask.cpp:525)
    // and is routed the same way for one write path rather than two.
    storage->setOnChange("wheater/priorityEnabled", [](const std::string&, const void* v) {
        StateManager::setWaterPriorityEnabled(*static_cast<const bool*>(v), false);
        parametersChanged = true;
        lastChangeTime = millis();
    });
    storage->setOnChange("system/boilerEnabled", [](const std::string&, const void* v) {
        StateManager::setBoilerEnabled(*static_cast<const bool*>(v), false);
        parametersChanged = true;
        lastChangeTime = millis();
    });
    storage->setOnChange("system/heatingEnabled", [](const std::string&, const void* v) {
        StateManager::setHeatingEnabled(*static_cast<const bool*>(v), false);
        parametersChanged = true;
        lastChangeTime = millis();
    });
    storage->setOnChange("system/waterEnabled", [](const std::string&, const void* v) {
        StateManager::setWaterEnabled(*static_cast<const bool*>(v), false);
        parametersChanged = true;
        lastChangeTime = millis();
    });
    storage->setOnChange("system/heatingOverrideOff", [](const std::string&, const void* v) {
        StateManager::setHeatingOverrideOff(*static_cast<const bool*>(v), false);
        parametersChanged = true;
        lastChangeTime = millis();
    });
    storage->setOnChange("system/waterOverrideOff", [](const std::string&, const void* v) {
        StateManager::setWaterOverrideOff(*static_cast<const bool*>(v), false);
        parametersChanged = true;
        lastChangeTime = millis();
    });
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
    storage->setOnChange("preheat/enabled", boolParamCallback);
    storage->setOnChange("boiler/pidEnabled", boolParamCallback);
    // Note: outsideThreshold and roomOverheatMargin callbacks set inline during registration
    // Sensor offsets - use sensorOffsetCallback to sync int32_t shadows to Temperature_t
    storage->setOnChange("sensor/offset/boilerOutput", sensorOffsetCallback);
    storage->setOnChange("sensor/offset/boilerReturn", sensorOffsetCallback);
    storage->setOnChange("sensor/offset/waterTank", sensorOffsetCallback);
    storage->setOnChange("sensor/offset/waterReturn", sensorOffsetCallback);
    storage->setOnChange("sensor/offset/heatingReturn", sensorOffsetCallback);
    storage->setOnChange("sensor/offset/outside", sensorOffsetCallback);
    storage->setOnChange("sensor/offset/room", sensorOffsetCallback);
    storage->setOnChange("sensor/offset/pressure", sensorOffsetCallback);

    // Syslog configuration callbacks - sync shadows to settings
    auto syslogCallback = [&settings](const std::string& name, const void* value) {
        // Sync syslog shadows to settings
        settings.syslogServerIP[0] = static_cast<uint8_t>(syslog_serverIP0);
        settings.syslogServerIP[1] = static_cast<uint8_t>(syslog_serverIP1);
        settings.syslogServerIP[2] = static_cast<uint8_t>(syslog_serverIP2);
        settings.syslogServerIP[3] = static_cast<uint8_t>(syslog_serverIP3);
        settings.syslogPort = static_cast<uint16_t>(syslog_port);
        settings.syslogFacility = static_cast<uint8_t>(syslog_facility);
        settings.syslogMinLevel = static_cast<uint8_t>(syslog_minLevel);
        LOG_INFO(TAG, "Syslog config %s changed", name.c_str());
        parametersChanged = true;
        lastChangeTime = millis();
    };
    storage->setOnChange("syslog/enabled", boolParamCallback);
    storage->setOnChange("syslog/serverIP0", syslogCallback);
    storage->setOnChange("syslog/serverIP1", syslogCallback);
    storage->setOnChange("syslog/serverIP2", syslogCallback);
    storage->setOnChange("syslog/serverIP3", syslogCallback);
    storage->setOnChange("syslog/port", syslogCallback);
    storage->setOnChange("syslog/facility", syslogCallback);
    storage->setOnChange("syslog/minLevel", syslogCallback);

    // Load all saved parameters (auto-save defaults on first boot)
    LOG_INFO(TAG, "Loading saved parameters...");
    storage->loadAll(true);  // autoSaveDefaults = true

    // Apply loaded shadow values to Temperature_t fields
    temperatureShadows.applyToSettings(settings);

    // Hand the restored calibration offsets to the MB8ART driver
    pushSensorOffsetsToMB8ART(settings);

    // Apply loaded syslog shadows to settings
    settings.syslogServerIP[0] = static_cast<uint8_t>(syslog_serverIP0);
    settings.syslogServerIP[1] = static_cast<uint8_t>(syslog_serverIP1);
    settings.syslogServerIP[2] = static_cast<uint8_t>(syslog_serverIP2);
    settings.syslogServerIP[3] = static_cast<uint8_t>(syslog_serverIP3);
    settings.syslogPort = static_cast<uint16_t>(syslog_port);
    settings.syslogFacility = static_cast<uint8_t>(syslog_facility);
    settings.syslogMinLevel = static_cast<uint8_t>(syslog_minLevel);

    // CRITICAL: Re-apply int32_t defaults after load() for parameters not in NVS
    // load() zeros values for missing keys, so restore defaults here
    if (burner_low_i32 == 0) burner_low_i32 = 380;
    if (burner_high_i32 == 0) burner_high_i32 = 1100;
    if (heating_low_i32 == 0) heating_low_i32 = 380;
    if (heating_high_i32 == 0) heating_high_i32 = 750;
    if (boostMinTarget_i32 == 0) boostMinTarget_i32 = 500;
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
    settings.heating_boost_min_target = static_cast<Temperature_t>(boostMinTarget_i32);
    settings.water_heating_low_limit = static_cast<Temperature_t>(water_low_i32);
    settings.water_heating_high_limit = static_cast<Temperature_t>(water_high_i32);
    settings.outsideTempHeatingThreshold = static_cast<Temperature_t>(outsideHeatingThreshold_i32);
    settings.roomTempOverheatMargin = static_cast<Temperature_t>(roomOverheatMargin_i32);

    // Apply loaded preheating / pump overrun shadows (onChange does not fire on load)
    applyPreheatPumpShadows(settings);

    // BUG FIX: Apply loaded wheater tank limits to SystemSettings.
    // setOnChange callbacks fire only on MQTT-triggered changes, NOT on initial NVS load.
    // Without this explicit sync, settings.wHeaterConfTempLimitHigh stays at the struct
    // default (65°C) even though NVS holds a different value - causing heating to ignore NVS.
    settings.wHeaterConfTempLimitLow = static_cast<Temperature_t>(tankLow_i32);
    settings.wHeaterConfTempLimitHigh = static_cast<Temperature_t>(tankHigh_i32);
    settings.wHeaterConfTempSafeLimitHigh = static_cast<Temperature_t>(tankSafeHigh_i32);
    settings.wHeaterConfTempSafeLimitLow = static_cast<Temperature_t>(tankSafeLow_i32);

    // BUG FIX (C2): Apply loaded room setpoint and hysteresis to SystemSettings.
    // Same bug pattern as wHeaterConfTempLimitHigh above: setOnChange only fires on MQTT,
    // not on initial NVS load. Without this, room target and hysteresis revert to struct
    // defaults (18.0°C / 0.5°C) on every reboot, ignoring any saved NVS values.
    if (targetTemp_i32 == 0) targetTemp_i32 = 180;  // 18.0°C default (struct default)
    if (hysteresis_i32 == 0) hysteresis_i32 = 5;    // 0.5°C default (struct default)
    settings.targetTemperatureInside = static_cast<Temperature_t>(targetTemp_i32);
    settings.heating_hysteresis = static_cast<Temperature_t>(hysteresis_i32);
    LOG_INFO(TAG, "Room control params after load: targetTemp=%ld hysteresis=%ld",
             targetTemp_i32, hysteresis_i32);

    LOG_INFO(TAG, "Temperature limits after load: burner[%ld-%ld] heating[%ld-%ld] water[%ld-%ld]",
             burner_low_i32, burner_high_i32, heating_low_i32, heating_high_i32, water_low_i32, water_high_i32);
    LOG_INFO(TAG, "Wheater tank limits after load: low=%ld high=%ld safeHigh=%ld safeLow=%ld",
             tankLow_i32, tankHigh_i32, tankSafeHigh_i32, tankSafeLow_i32);

    parametersLoaded.store(true);

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

        // Need DRAM for the subscription list. MALLOC_CAP_DEFAULT, not ESP.getFreeHeap():
        // that is MALLOC_CAP_INTERNAL, counts IRAM, reads ~58 kB on this chip, and so the
        // old comparison against MIN_FREE_HEAP_WARNING (20000) was true-by-construction -
        // this gate had never once refused, so the "do not subscribe when memory is
        // short" protection it claimed did not exist (2026-10-18 review).
        //
        // Refusing is safe and self-correcting: returning false leaves
        // mqttSubscriptionsActive false, and the loop retries with exponential backoff.
        const size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
        if (largestBlock < SystemConstants::System::MIN_DRAM_BLOCK_WARNING) {
            LOG_ERROR(TAG, "Low DRAM for MQTT subs: largest block %u B (need %u B)",
                      static_cast<unsigned>(largestBlock),
                      static_cast<unsigned>(SystemConstants::System::MIN_DRAM_BLOCK_WARNING));
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
                // Wake the storage task now; the command queue holds only a few
                // entries and handleMqttCommand() drops on overflow without
                // telling the caller. Runs in the esp-mqtt task, so only set a bit.
                if (storageEventGroup) {
                    xEventGroupSetBits(storageEventGroup, STORAGE_COMMAND_PENDING_BIT);
                }
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
    LOG_INFO(TAG, "No periodic saves - saved on parameter set, boiler/params/save and enable/priority/override commands");
    
    // Re-sync the int32 shadows FROM settings, which are the source of truth once boot has
    // applied the loaded values.
    //
    // Why this is needed: registerInt() cannot bind a Temperature_t, so those parameters are
    // registered against an int32 shadow and setOnChange() copies shadow -> setting. That is
    // one-way. A write from anywhere else leaves the shadow behind - and the shadow is BOTH
    // what PersistentStorage publishes on boiler/params/status/* AND what it writes to NVS.
    // A stale shadow therefore misreports the value and then silently reverts it on the next
    // boot, because loadAll() applies shadow -> setting again (see "BUG FIX (C2)" above).
    //
    // Measured 2026-10-15: the room target read 20.0 live and 18.0 in the params dump, after
    // being set through boiler/cmd (MQTTCommandHandlers.cpp:193, :223). 18.0 was not even a
    // saved value - it is the struct default this function substitutes for an empty NVS key.
    //
    // targetTemperatureInside is the ONLY shadowed setting with writers outside its own
    // callback: the two command handlers above and the scheduler
    // (SpaceHeatingScheduleAction.cpp:201). The other 11 shadows were each checked and have
    // none, so they are deliberately NOT synced here - a blanket settings -> shadow copy
    // would clobber a loaded NVS value for any parameter whose apply-after-load path is
    // missing. If another setting gains an external writer, add it here and verify its
    // apply-after-load first.
    //
    // The retained boiler/status/heating/target topic is republished from here for the same
    // reason the shadow is synced here: this is the one place that sees every writer. It used
    // to be published ONLY by handleRoomTargetCommand() (MQTTCommandHandlers.cpp:210), so the
    // parameter path, the boiler/cmd/heating <number> path (:193, which publishes
    // "target:19.2" on boiler/status/heating instead) and the scheduler (which publishes
    // nothing) all left it stale. Measured 2026-10-15: the broker served 19.8 for an hour
    // while the device ran 19.2, so anything reading that topic - the HA target display
    // included - showed the wrong setpoint. This is the mirror image of 57d6456, where the
    // cmd path published but did not persist.
    //
    // Publishing on change only: this lambda runs on every pass of the loop below.
    // A failed publish leaves lastPublishedTarget alone so the next pass retries, which also
    // means the retained topic gets a correct value once MQTT comes up after a boot - it
    // previously kept whatever the last boiler/cmd/room_target had written, possibly reboots
    // ago.
    static int32_t lastPublishedTarget = -1;  // no real target is negative
    auto syncShadowsFromSettings = [&settings]() {
        targetTemp_i32 = static_cast<int32_t>(settings.targetTemperatureInside);
        if (targetTemp_i32 != lastPublishedTarget) {
            char targetBuf[8];
            formatTenths(targetBuf, sizeof(targetBuf), targetTemp_i32);
            if (MQTTTask::publish(MQTT_STATUS_HEATING "/target", targetBuf, 0, true,
                                  MQTTPriority::PRIORITY_LOW)) {
                lastPublishedTarget = targetTemp_i32;
            }
        }
    };

    // Main event-driven loop

    while (true) {
        // Wait for events with timeout for periodic checks
        EventBits_t bits = xEventGroupWaitBits(
            storageEventGroup,
            STORAGE_SAVE_REQUEST_BIT | STORAGE_LOAD_REQUEST_BIT | STORAGE_MQTT_RECONNECT_BIT |
                STORAGE_COMMAND_PENDING_BIT,
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

        // Before the queue is processed, so a boiler/params/get publishes the live values
        // rather than a shadow left behind by a boiler/cmd or scheduler write.
        syncShadowsFromSettings();

        // Process MQTT command queue - this is required for commands like save, get, set to work
        storage->processCommandQueue();

        // Two kinds of save.
        //
        // EXPLICIT (boiler/params/save, StateManager enable/priority/override, the syslog
        // and autotune writers) goes through PersistentStorageTask_RequestSave() and saves
        // at once - someone asked for it, so do not make them wait.
        //
        // DEBOUNCED covers the parameter onChange callbacks. They now only set
        // parametersChanged/lastChangeTime and the save is coalesced over
        // PARAM_SAVE_DEBOUNCE_MS, so a thermostat card clicking the room target up five
        // times costs ONE saveAll instead of five. saveAll() is 60 parameters x (nvs_set +
        // nvs_commit) because Preferences commits per key, and nothing is at risk while we
        // wait: PersistentStorage::setJson() has already written the one key that changed.
        // The full pass still earns its place - it is what persists a setting written
        // straight to SystemSettings by boiler/cmd/room_target or boiler/cmd/heating, which
        // never goes near setJson() (see MQTTCommandHandlers.cpp:193, :223).
        //
        // parametersChanged and lastChangeTime were assigned by all 14 callbacks and read by
        // NOTHING until now (found 2026-10-18): the debounce was designed and never wired
        // up, so every set ran the full save immediately.
        const bool explicitSave = (bits & STORAGE_SAVE_REQUEST_BIT) != 0;
        if (ParameterSavePolicy::shouldSave(
                explicitSave, parametersChanged, millis(), lastChangeTime,
                SystemConstants::Tasks::Storage::PARAM_SAVE_DEBOUNCE_MS)) {
            LOG_INFO(TAG, "Parameter save (%s)", explicitSave ? "requested" : "debounced");
            // Repeated here, not just at the top of the loop: another task can write a
            // setting after the sync above and before this save, and what NVS keeps is
            // whatever the shadow holds at this instant.
            syncShadowsFromSettings();
            storage->saveAll();
            
            // Update temperature shadows if needed
            temperatureShadows.applyToSettings(settings);
            
            parametersChanged = false;
        }
        
        // Handle load request
        if (bits & STORAGE_LOAD_REQUEST_BIT) {
            LOG_INFO(TAG, "Reloading parameters...");
            storage->loadAll();
            temperatureShadows.applyToSettings(settings);
            // A reload is the one case where the SHADOW is newer than the setting, so the
            // copy has to run the other way - and it must happen here, or the sync at the
            // top of the next iteration would write the pre-reload setting straight back
            // over the value just read from NVS. Mirrors the apply-after-load that the
            // init path already does for this parameter ("BUG FIX (C2)" above); the loop
            // path never had it, so a runtime reload never reached live settings.
            settings.targetTemperatureInside = static_cast<Temperature_t>(targetTemp_i32);
            settings.heating_hysteresis = static_cast<Temperature_t>(hysteresis_i32);

            // Same reason: a reload can change the offsets, so the driver needs them again
            pushSensorOffsetsToMB8ART(settings);
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

bool PersistentStorageTask_SetParameter(const char* name, const char* payload) {
    PersistentStorage* storage = storageInstance.load();
    if (storage == nullptr || name == nullptr || payload == nullptr) {
        return false;
    }
    // Same queued path as an MQTT boiler/params/set/<name> message
    return storage->handleMqttCommand(std::string("boiler/params/set/") + name, payload);
}

bool PersistentStorageTask_ParametersLoaded() {
    return parametersLoaded.load();
}