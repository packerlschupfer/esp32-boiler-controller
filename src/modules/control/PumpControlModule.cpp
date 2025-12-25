// src/modules/control/PumpControlModule.cpp
// Unified pump control module for heating and water pumps
#include "PumpControlModule.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "core/SystemResourceProvider.h"
#include "core/SharedResourceManager.h"
#include "config/SystemConstants.h"
#include "config/SystemSettingsStruct.h"
#include "events/SystemEventsGenerated.h"
#include "modules/control/ReturnPreheater.h"  // For yield during preheating
#include "LoggingMacros.h"
#include <TaskManager.h>
#include <RuntimeStorage.h>

// Static state tracking
PumpState PumpControlModule::heatingPumpState_ = PumpState::Off;
PumpState PumpControlModule::waterPumpState_ = PumpState::Off;

// Mutex for thread-safe state access
SemaphoreHandle_t PumpControlModule::stateMutex_ = nullptr;
bool PumpControlModule::mutexInitialized_ = false;

void PumpControlModule::initMutexIfNeeded() {
    if (!mutexInitialized_) {
        stateMutex_ = xSemaphoreCreateMutex();
        if (stateMutex_ != nullptr) {
            mutexInitialized_ = true;
        }
    }
}

// Static configurations for each pump type
const PumpConfig PumpControlModule::heatingPumpConfig_ = {
    .modeActiveBit = SystemEvents::SystemState::HEATING_ON,
    .pumpOnStateBit = SystemEvents::SystemState::HEATING_PUMP_ON,
    .relayOnRequestBit = SystemEvents::RelayRequest::HEATING_PUMP_ON,
    .relayOffRequestBit = SystemEvents::RelayRequest::HEATING_PUMP_OFF,
    .startCounterId = rtstorage::COUNTER_HEATING_PUMP_STARTS,
    .taskName = "HeatingPump",
    .logTag = "HeatingPumpCtrl"
};

const PumpConfig PumpControlModule::waterPumpConfig_ = {
    .modeActiveBit = SystemEvents::SystemState::WATER_ON,
    .pumpOnStateBit = SystemEvents::SystemState::WATER_PUMP_ON,
    .relayOnRequestBit = SystemEvents::RelayRequest::WATER_PUMP_ON,
    .relayOffRequestBit = SystemEvents::RelayRequest::WATER_PUMP_OFF,
    .startCounterId = rtstorage::COUNTER_WATER_PUMP_STARTS,
    .taskName = "WaterPump",
    .logTag = "WaterPumpCtrl"
};

// Entry points that pass the appropriate config
void PumpControlModule::HeatingPumpTask(void* pvParameters) {
    (void)pvParameters;
    PumpControlTask(const_cast<PumpConfig*>(&heatingPumpConfig_));
}

void PumpControlModule::WaterPumpTask(void* pvParameters) {
    (void)pvParameters;
    PumpControlTask(const_cast<PumpConfig*>(&waterPumpConfig_));
}

// State accessors (thread-safe)
PumpState PumpControlModule::getHeatingPumpState() {
    initMutexIfNeeded();
    PumpState state = PumpState::Off;
    if (stateMutex_ && xSemaphoreTake(stateMutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
        state = heatingPumpState_;
        xSemaphoreGive(stateMutex_);
    }
    return state;
}

PumpState PumpControlModule::getWaterPumpState() {
    initMutexIfNeeded();
    PumpState state = PumpState::Off;
    if (stateMutex_ && xSemaphoreTake(stateMutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
        state = waterPumpState_;
        xSemaphoreGive(stateMutex_);
    }
    return state;
}

// Unified pump control task
void PumpControlModule::PumpControlTask(void* pvParameters) {
    const PumpConfig* config = static_cast<const PumpConfig*>(pvParameters);
    if (!config) {
        vTaskDelete(NULL);
        return;
    }

    const char* TAG = config->logTag;
    LOG_INFO(TAG, "Task started");

    // Initialize mutex for thread-safe state access
    initMutexIfNeeded();

    // Determine which state variable this task controls
    const bool isHeatingPump = (config == &heatingPumpConfig_);
    PumpState& currentStateRef = isHeatingPump ? heatingPumpState_ : waterPumpState_;

    // Register with watchdog - pumps are critical for proper circulation
    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::enabled(
        true,   // critical task
        10000   // 10 second timeout
    );

    if (!SRP::getTaskManager().registerCurrentTaskWithWatchdog(config->taskName, wdtConfig)) {
        LOG_ERROR(TAG, "Failed to register with watchdog");
    } else {
        LOG_INFO(TAG, "Registered with watchdog (10s timeout)");
    }

    // Get shared resources
    auto& resourceManager = SharedResourceManager::getInstance();
    EventGroupHandle_t systemStateEventGroup = resourceManager.getEventGroup(
        SharedResourceManager::EventGroups::SYSTEM_STATE);
    EventGroupHandle_t relayRequestEventGroup = resourceManager.getEventGroup(
        SharedResourceManager::EventGroups::RELAY_REQUEST);

    if (!systemStateEventGroup || !relayRequestEventGroup) {
        LOG_ERROR(TAG, "Failed to get required event groups");
        vTaskDelete(NULL);
        return;
    }

    PumpState lastLoggedState = PumpState::Off;

    // Pump overrun tracking - keep pump running after heating stops to dissipate residual heat
    bool wasModeActive = false;
    uint32_t overrunStartTime = 0;
    bool inOverrun = false;

    while (true) {
        // Feed watchdog
        (void)SRP::getTaskManager().feedWatchdog();

        // During preheating, let ReturnPreheater decide pump state (thermal shock mitigation)
        bool preheatingActive = isHeatingPump &&
            ReturnPreheater::getState() == ReturnPreheater::State::PREHEATING;

        // Check system state
        EventBits_t systemBits = xEventGroupGetBits(systemStateEventGroup);

        // Determine desired state based on system conditions
        PumpState desiredState = PumpState::Off;

        // Check if system is enabled first - if not, pump must be off (no overrun)
        bool systemEnabled = (systemBits & SystemEvents::SystemState::BOILER_ENABLED) != 0;

        // Pump should be on if system is enabled AND in the appropriate mode
        bool modeActive = (systemBits & config->modeActiveBit) != 0;

        if (systemEnabled && modeActive) {
            desiredState = PumpState::On;
            inOverrun = false;  // Clear overrun when mode becomes active again
        } else if (systemEnabled && wasModeActive && !modeActive) {
            // Mode just turned off - start overrun period
            overrunStartTime = millis();
            inOverrun = true;
            // Get configurable cooldown time from SystemSettings
            SystemSettings& settings = SRP::getSystemSettings();
            LOG_INFO(TAG, "Starting pump overrun (%lu ms) to dissipate residual heat",
                     settings.pumpCooldownMs);
        }

        // Check if we're in overrun period
        if (inOverrun && systemEnabled) {
            uint32_t elapsed = millis() - overrunStartTime;
            // Use configurable cooldown from SystemSettings
            SystemSettings& settings = SRP::getSystemSettings();
            if (elapsed < settings.pumpCooldownMs) {
                // Still in overrun - keep pump running
                desiredState = PumpState::On;
            } else {
                // Overrun complete
                LOG_INFO(TAG, "Pump overrun complete - stopping pump after %lu ms",
                         settings.pumpCooldownMs);
                inOverrun = false;
            }
        }

        // Track mode state for next iteration
        wasModeActive = modeActive;

        // Override: During preheating, ReturnPreheater controls pump cycling
        if (preheatingActive) {
            desiredState = ReturnPreheater::shouldPumpBeOn() ? PumpState::On : PumpState::Off;
        }

        // Read current state with mutex protection
        PumpState currentState = PumpState::Off;
        if (stateMutex_ && xSemaphoreTake(stateMutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
            currentState = currentStateRef;
            xSemaphoreGive(stateMutex_);
        }

        // Update state if changed
        // Motor protection is enforced at RelayControlTask layer (SafetyConfig::pumpProtectionMs)
        if (desiredState != currentState) {
            LOG_INFO(TAG, "State change: %s -> %s",
                     currentState == PumpState::On ? "ON" : "OFF",
                     desiredState == PumpState::On ? "ON" : "OFF");

            // Update logical state with mutex protection
            if (stateMutex_ && xSemaphoreTake(stateMutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
                currentStateRef = desiredState;
                xSemaphoreGive(stateMutex_);
            }

            // Request relay change via event bits
            // RelayControlTask will enforce motor protection timing
            if (desiredState == PumpState::On) {
                xEventGroupSetBits(relayRequestEventGroup, config->relayOnRequestBit);
                xEventGroupSetBits(systemStateEventGroup, config->pumpOnStateBit);

                // Increment pump start counter in FRAM
                rtstorage::RuntimeStorage* storage = SRP::getRuntimeStorage();
                if (storage) {
                    if (storage->incrementCounter(config->startCounterId)) {
                        uint32_t count = storage->getCounter(config->startCounterId);
                        LOG_INFO(TAG, "Pump start count: %lu", count);
                    }
                }
            } else {
                xEventGroupSetBits(relayRequestEventGroup, config->relayOffRequestBit);
                xEventGroupClearBits(systemStateEventGroup, config->pumpOnStateBit);
            }
        }

        // Debug log on state change (less frequent)
        if (lastLoggedState != currentState) {
            #if defined(LOG_MODE_DEBUG_SELECTIVE) || defined(LOG_MODE_DEBUG_FULL)
            LOG_DEBUG(TAG, "Pump state: %s (mode: %s)",
                     currentState == PumpState::On ? "ON" : "OFF",
                     modeActive ? "ACTIVE" : "INACTIVE");
            #endif
            lastLoggedState = currentState;
        }

        vTaskDelay(pdMS_TO_TICKS(SystemConstants::Timing::PUMP_CHECK_INTERVAL_MS));
    }
}
