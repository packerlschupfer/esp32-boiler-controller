// src/modules/control/PumpControlModule.cpp
// Unified pump control module for heating and water pumps
#include "PumpControlModule.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "core/SystemResourceProvider.h"
#include "core/SharedResourceManager.h"
#include "config/SystemConstants.h"
#include "events/SystemEventsGenerated.h"
#include "LoggingMacros.h"
#include <TaskManager.h>
#include <RuntimeStorage.h>

// Static state tracking
PumpState PumpControlModule::heatingPumpState_ = PumpState::Off;
PumpState PumpControlModule::waterPumpState_ = PumpState::Off;

// Timestamps for motor protection (initialized to allow immediate first change)
uint32_t PumpControlModule::heatingPumpLastChangeTime_ = 0;
uint32_t PumpControlModule::waterPumpLastChangeTime_ = 0;

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

// Motor protection check (thread-safe)
bool PumpControlModule::canChangeState(bool isHeatingPump) {
    initMutexIfNeeded();

    uint32_t now = millis();
    uint32_t lastChange = 0;

    if (stateMutex_ && xSemaphoreTake(stateMutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
        lastChange = isHeatingPump ? heatingPumpLastChangeTime_ : waterPumpLastChangeTime_;
        xSemaphoreGive(stateMutex_);
    }

    // Allow first change (lastChange == 0) or if interval has elapsed
    if (lastChange == 0) {
        return true;
    }

    uint32_t elapsed = now - lastChange;
    return elapsed >= MOTOR_PROTECTION_INTERVAL_MS;
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
    uint32_t& lastChangeTimeRef = isHeatingPump ? heatingPumpLastChangeTime_ : waterPumpLastChangeTime_;

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

    PumpState lastState = PumpState::Off;

    while (true) {
        // Feed watchdog
        (void)SRP::getTaskManager().feedWatchdog();

        // Check system state
        EventBits_t systemBits = xEventGroupGetBits(systemStateEventGroup);

        // Determine desired state based on system conditions
        PumpState desiredState = PumpState::Off;

        // Check if system is enabled first - if not, pump must be off
        bool systemEnabled = (systemBits & SystemEvents::SystemState::BOILER_ENABLED) != 0;

        // Pump should be on if system is enabled AND in the appropriate mode
        bool modeActive = (systemBits & config->modeActiveBit) != 0;

        if (systemEnabled && modeActive) {
            desiredState = PumpState::On;
        }

        // Read current state with mutex protection
        PumpState currentState = PumpState::Off;
        if (stateMutex_ && xSemaphoreTake(stateMutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
            currentState = currentStateRef;
            xSemaphoreGive(stateMutex_);
        }

        // Update state if changed (with motor protection)
        // Motor protection enforced BOTH here AND at relay layer for defense-in-depth
        if (desiredState != currentState) {
            // Check motor protection interval
            if (!canChangeState(isHeatingPump)) {
                uint32_t lastChange = 0;
                if (stateMutex_ && xSemaphoreTake(stateMutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
                    lastChange = lastChangeTimeRef;
                    xSemaphoreGive(stateMutex_);
                }
                uint32_t elapsed = millis() - lastChange;
                uint32_t remaining = MOTOR_PROTECTION_INTERVAL_MS - elapsed;
                LOG_DEBUG(TAG, "Motor protection: %lu ms remaining before state change allowed",
                         remaining);
                // Skip state change this cycle - will retry next iteration
            } else {
                LOG_INFO(TAG, "State change: %s -> %s",
                         currentState == PumpState::On ? "ON" : "OFF",
                         desiredState == PumpState::On ? "ON" : "OFF");

                // Update state with mutex protection
                if (stateMutex_ && xSemaphoreTake(stateMutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
                    currentStateRef = desiredState;
                    lastChangeTimeRef = millis();
                    xSemaphoreGive(stateMutex_);
                }

                // Request relay change via event bits
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
        }

        // Debug log on state change
        if (lastState != currentState) {
            #if defined(LOG_MODE_DEBUG_SELECTIVE) || defined(LOG_MODE_DEBUG_FULL)
            LOG_DEBUG(TAG, "Pump state: %s (mode: %s)",
                     currentState == PumpState::On ? "ON" : "OFF",
                     modeActive ? "ACTIVE" : "INACTIVE");
            #endif
            lastState = currentState;
        }

        vTaskDelay(pdMS_TO_TICKS(SystemConstants::Timing::PUMP_CHECK_INTERVAL_MS));
    }
}
