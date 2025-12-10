// src/modules/tasks/HeatingControlTask_EventDriven.cpp
// Heating control task - manages space heating with PID control
#include "HeatingControlTask.h"

#include "config/SystemConstants.h"
#include "shared/SharedResources.h"
#include "shared/Temperature.h"
#include "events/SystemEventsGenerated.h"
#include "modules/control/HeatingControlModule.h"
#include "modules/control/BurnerRequestManager.h"
#include "modules/control/BurnerSafetyValidator.h"
#include "modules/control/ErrorRecoveryManager.h"
#include "modules/control/TemperatureSensorFallback.h"
#include "utils/ResourceGuard.h"
#include "utils/MutexRetryHelper.h"
#include "LoggingMacros.h"
#include "config/ProjectConfig.h"
#include <esp_task_wdt.h>
#include <algorithm>  // for std::max
#include "core/SystemResourceProvider.h"
#include <TaskManager.h>
#include <esp_log.h>

// Constants
#define DEFAULT_BOILER_TARGET_TEMP tempFromFloat(70.0f)

// Timer handles
static TimerHandle_t safetyCheckTimer = nullptr;
static TimerHandle_t processTimer = nullptr;
static TaskHandle_t heatingTaskHandle = nullptr;

// State tracking - all task state is consolidated here to prevent scattered statics
// NOTE: Only accessed by HeatingControlTask - single task, no mutex needed
static struct {
    HeatingControlState state = HeatingOff;
    Temperature_t lastBoilerTarget = 0;
    bool initialized = false;
    HeatingControlModule* heatingControl = nullptr;
} heatingState;

// Forward declarations
static void safetyCheckCallback(TimerHandle_t xTimer);
static void processTimerCallback(TimerHandle_t xTimer);
static void processHeatingState();
static bool checkIfSpaceHeatingNeededEvent();

void HeatingControlTask(void *parameter) {
    const char* TAG = "HeatingControlTask";
    
    heatingTaskHandle = xTaskGetCurrentTaskHandle();
    
    LOG_INFO(TAG, "Started (Event-Driven) C%d Stk:%d", xPortGetCoreID(), uxTaskGetStackHighWaterMark(NULL));
    
    // Get heating control module from service container
    heatingState.heatingControl = SRP::getHeatingControl();

    // Validate critical module is available
    if (heatingState.heatingControl == nullptr) {
        LOG_ERROR(TAG, "CRITICAL: HeatingControlModule not available - cannot operate");
        ErrorHandler::logError(TAG, SystemError::NOT_INITIALIZED,
                              "HeatingControlModule not initialized");
        // Continue task but log the issue - system will operate in degraded mode
        // All operations will be safely skipped due to nullptr checks
    }

    LOG_INFO(TAG, "Init OK");
    
    // Watchdog will be registered after initialization
    
    // Create process timer (runs every 5 seconds like MB8ART)
    processTimer = xTimerCreate(
        "HeatingProcess",
        pdMS_TO_TICKS(5000),   // 5 second interval
        pdTRUE,                // Auto-reload
        nullptr,
        processTimerCallback
    );
    
    if (!processTimer) {
        LOG_ERROR(TAG, "Failed to create process timer");
        // No cleanup needed - timer wasn't created
        vTaskDelete(NULL);
        return;
    }

    // Create safety check timer (runs every 60 seconds to handle sensor failures)
    safetyCheckTimer = xTimerCreate(
        "HeatingSafety",
        pdMS_TO_TICKS(60000),  // 60 second safety interval
        pdTRUE,                // Auto-reload
        nullptr,
        safetyCheckCallback
    );

    if (!safetyCheckTimer) {
        LOG_ERROR(TAG, "Failed to create safety timer");
        // Cleanup: delete process timer before exiting
        xTimerDelete(processTimer, 0);
        processTimer = nullptr;
        vTaskDelete(NULL);
        return;
    }

    heatingState.initialized = true;

    // Start timers
    if (xTimerStart(processTimer, pdMS_TO_TICKS(100)) != pdPASS) {
        LOG_ERROR(TAG, "Failed to start process timer");
        // Cleanup: delete both timers before exiting
        xTimerDelete(processTimer, 0);
        xTimerDelete(safetyCheckTimer, 0);
        processTimer = nullptr;
        safetyCheckTimer = nullptr;
        vTaskDelete(NULL);
        return;
    }
    if (xTimerStart(safetyCheckTimer, pdMS_TO_TICKS(100)) != pdPASS) {
        LOG_ERROR(TAG, "Failed to start safety timer");
        // Cleanup: stop and delete both timers before exiting
        xTimerStop(processTimer, 0);
        xTimerDelete(processTimer, 0);
        xTimerDelete(safetyCheckTimer, 0);
        processTimer = nullptr;
        safetyCheckTimer = nullptr;
        vTaskDelete(NULL);
        return;
    }

    LOG_INFO(TAG, "Event-driven mode activated");
    
    // Register with watchdog after initialization
    // Timeout should be at least 4x the room sensor read interval to allow for delays
    // Use configured minimum as a floor to prevent too-short timeouts
    uint32_t sensorInterval = SRP::getRoomSensorReadInterval();  // Use ANDRTF3 interval since we wait for INSIDE_TEMP
    uint32_t watchdogTimeout = std::max(sensorInterval * 4, SystemConstants::System::WDT_HEATING_CONTROL_MS);

    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::enabled(
        false,  // not critical (won't reset system)
        watchdogTimeout
    );

    LOG_INFO(TAG, "Watchdog timeout set to %lu ms (max of 4x sensor %lu ms or min %lu ms)",
             watchdogTimeout, sensorInterval, SystemConstants::System::WDT_HEATING_CONTROL_MS);

    if (!SRP::getTaskManager().registerCurrentTaskWithWatchdog("HeatingControl", wdtConfig)) {
        LOG_ERROR(TAG, "WDT reg failed");
    } else {
        LOG_INFO(TAG, "WDT OK %lums", watchdogTimeout);
        (void)SRP::getTaskManager().feedWatchdog();  // Feed immediately
    }
    
    // Main task loop - use timer notifications like MB8ART
    while (true) {
        // Wait for timer notification with 1 second timeout for watchdog
        uint32_t ulNotificationValue = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        
        if (ulNotificationValue > 0) {
            // Timer triggered - process heating state
            processHeatingState();
        }
        
        // Also check for immediate control events
        EventBits_t controlBits = xEventGroupGetBits(SRP::getControlRequestsEventGroup());
        const EventBits_t CONTROL_EVENTS = SystemEvents::ControlRequest::HEATING_ON_OVERRIDE |
                                          SystemEvents::ControlRequest::HEATING_OFF_OVERRIDE |
                                          SystemEvents::ControlRequest::WATER_PRIORITY_RELEASED;

        if (controlBits & CONTROL_EVENTS) {
            // Control events trigger immediate processing
            xEventGroupClearBits(SRP::getControlRequestsEventGroup(), controlBits & CONTROL_EVENTS);
            processHeatingState();
        }

        // Feed watchdog
        (void)SRP::getTaskManager().feedWatchdog();
    }
}

static void processTimerCallback(TimerHandle_t xTimer) {
    (void)xTimer;
    // Notify task to process - like MB8ART pattern
    if (heatingTaskHandle != nullptr) {
        xTaskNotifyGive(heatingTaskHandle);
    }
}

static void safetyCheckCallback(TimerHandle_t xTimer) {
    (void)xTimer;
    const char* TAG = "HeatingSafety";
    
    // Safety check - ensure we haven't missed sensor updates for too long
    if (heatingState.state == HeatingOn) {
        // Check if sensors are still available
        if (!TemperatureSensorFallback::canContinueOperation()) {
            LOG_ERROR(TAG, "Sensor failure detected during heating - shutting down");
            if (heatingState.heatingControl != nullptr) {
                heatingState.heatingControl->stopHeating();
            } else {
                LOG_ERROR(TAG, "HeatingControlModule unavailable - cannot issue stop command");
            }
            SRP::clearSystemStateEventBits(SystemEvents::SystemState::HEATING_ON);
            SRP::clearHeatingEventBits(SystemEvents::HeatingEvent::True);
            BurnerRequestManager::clearRequest(BurnerRequestManager::RequestSource::HEATING);
            heatingState.state = HeatingOff;
        }
    }
}

static void processHeatingState() {
    const char* TAG = "HeatingProcess";
    EventBits_t systemStateBits = SRP::getSystemStateEventBits();
    
    // Check if both boiler and heating are enabled
    if (!(systemStateBits & SystemEvents::SystemState::BOILER_ENABLED) || 
        !(systemStateBits & SystemEvents::SystemState::HEATING_ENABLED)) {
        
        // System disabled - turn off heating if on
        if (heatingState.state == HeatingOn) {
            LOG_INFO(TAG, "System disabled - turning off heating");
            if (heatingState.heatingControl != nullptr) {
                heatingState.heatingControl->stopHeating();
            } else {
                LOG_WARN(TAG, "HeatingControlModule unavailable - clearing state only");
            }
            SRP::clearSystemStateEventBits(SystemEvents::SystemState::HEATING_ON);
            SRP::clearHeatingEventBits(SystemEvents::HeatingEvent::True);
            BurnerRequestManager::clearRequest(BurnerRequestManager::RequestSource::HEATING);
            heatingState.state = HeatingOff;
        }
        return;
    }
    
    // Operation mode is managed by BurnerControlTask - don't set it here
    
    // Get control bits
    EventBits_t controlBits = SRP::getControlRequestsEventBits();
    
    switch (heatingState.state) {
        case HeatingOff: {
            // Check if we should turn on heating
            if (checkIfSpaceHeatingNeededEvent() || (controlBits & SystemEvents::ControlRequest::HEATING_ON_OVERRIDE)) {
                // Check settings for water priority (race prevention)
                SystemSettings& settings = SRP::getSystemSettings();
                EventBits_t systemStateBits = SRP::getSystemStateEventBits();
                bool waterEnabled = (systemStateBits & SystemEvents::SystemState::WATER_ENABLED) != 0;
                bool waterPriorityEnabled = settings.wheaterPriorityEnabled;

                // Round 15 Issue #13 fix: Use event-based synchronization instead of timing delay
                // If water priority enabled, check if water task has had a chance to claim priority
                if (waterPriorityEnabled && waterEnabled) {
                    // Wait briefly for water to claim burner OR release priority
                    // This is event-based: WATER request bit indicates water wants burner
                    EventBits_t waitBits = xEventGroupWaitBits(
                        SRP::getBurnerRequestEventGroup(),
                        SystemEvents::BurnerRequest::WATER,
                        pdFALSE,  // Don't clear bits
                        pdFALSE,  // Wait for any bit
                        pdMS_TO_TICKS(200)  // 200ms timeout - if not set, water doesn't need burner
                    );
                    (void)waitBits;  // We check the result below
                }

                // Check if water heating has priority BEFORE setting HEATING_ON
                // (prevents start/stop loop when water reclaims priority)
                EventBits_t burnerRequestBits = xEventGroupGetBits(SRP::getBurnerRequestEventGroup());
                EventBits_t systemStateBitsLocal = xEventGroupGetBits(SRP::getSystemStateEventGroup());
                bool waterActive = (burnerRequestBits & SystemEvents::BurnerRequest::WATER) != 0;
                bool waterPriority = (systemStateBitsLocal & SystemEvents::SystemState::WATER_PRIORITY) != 0;

                if (waterActive && waterPriority) {
                    LOG_INFO(TAG, "Water heating has priority - deferring heating activation");
                    // Clear the override bit since we can't act on it now
                    xEventGroupClearBits(SRP::getControlRequestsEventGroup(),
                                        SystemEvents::ControlRequest::HEATING_ON_OVERRIDE);
                    return;
                }

                // Set HEATING_ON bit now that we know water doesn't have priority
                // (needed for seamless mode switching from water to heating)
                if (heatingState.heatingControl != nullptr) {
                    heatingState.heatingControl->startHeating();
                } else {
                    LOG_ERROR(TAG, "Cannot start heating - HeatingControlModule unavailable");
                    return;
                }

                // Check if we can start heating
                if (!TemperatureSensorFallback::canContinueOperation()) {
                    LOG_WARN(TAG, "Cannot start heating - required sensors unavailable");
                    return;
                }
                
                // Calculate boiler target temperature
                Temperature_t boilerTargetTemp = DEFAULT_BOILER_TARGET_TEMP;
                
                // Get sensor readings for PID calculation
                {
                    auto guard = MutexRetryHelper::acquireGuard(
                        SRP::getSensorReadingsMutex(),
                        "SensorReadings-HeatingCalc"
                    );
                    if (guard) {
                        SharedSensorReadings readings = SRP::getSensorReadings();
                        SystemSettings& settings = SRP::getSystemSettings();

                        if (heatingState.heatingControl != nullptr) {
                            boilerTargetTemp = heatingState.heatingControl->calculateSpaceHeatingTargetTemp(readings, settings);
                        } else {
                            LOG_WARN(TAG, "HeatingControlModule unavailable - using default target");
                        }
                    } else {
                        LOG_ERROR(TAG, "Failed to acquire sensor mutex");
                        return;
                    }
                }

                // Note: HEATING_ON already set by startHeating() above
                SRP::setHeatingEventBits(SystemEvents::HeatingEvent::True);

                // Set heating request
                BurnerRequestManager::setHeatingRequest(boilerTargetTemp, false);
                
                heatingState.state = HeatingOn;
                heatingState.lastBoilerTarget = boilerTargetTemp;
                
                char tempStr[16];
                formatTemp(tempStr, sizeof(tempStr), boilerTargetTemp);
                LOG_INFO(TAG, "Space heating activated - Boiler target: %s°C", tempStr);
            }
            break;
        }
        
        case HeatingOn: {
            // Check if we should turn off heating
            bool heatingStillNeeded = checkIfSpaceHeatingNeededEvent();
            bool sensorsAvailable = TemperatureSensorFallback::canContinueOperation();

            // Check if water heating has priority - if so, we must yield
            EventBits_t burnerRequestBits = xEventGroupGetBits(SRP::getBurnerRequestEventGroup());
            bool waterActive = (burnerRequestBits & SystemEvents::BurnerRequest::WATER) != 0;
            bool waterPriority = (systemStateBits & SystemEvents::SystemState::WATER_PRIORITY) != 0;
            bool mustYieldToWater = waterActive && waterPriority;

            if ((controlBits & SystemEvents::ControlRequest::HEATING_OFF_OVERRIDE) ||
                !(systemStateBits & SystemEvents::SystemState::BOILER_ENABLED) ||
                !(systemStateBits & SystemEvents::SystemState::HEATING_ENABLED) ||
                !heatingStillNeeded ||
                !sensorsAvailable ||
                mustYieldToWater) {
                
                // Determine reason for shutdown
                const char* reason = "unknown";
                if (controlBits & SystemEvents::ControlRequest::HEATING_OFF_OVERRIDE) {
                    reason = "remote override OFF";
                } else if (!(systemStateBits & SystemEvents::SystemState::BOILER_ENABLED)) {
                    reason = "boiler disabled";
                } else if (!(systemStateBits & SystemEvents::SystemState::HEATING_ENABLED)) {
                    reason = "heating disabled";
                } else if (mustYieldToWater) {
                    reason = "water heating has priority";
                } else if (!heatingStillNeeded) {
                    reason = "target temperature reached";
                } else if (!sensorsAvailable) {
                    reason = "sensor failure";
                }
                
                LOG_INFO(TAG, "Deactivating space heating - reason: %s", reason);
                
                // Deactivate heating
                if (heatingState.heatingControl != nullptr) {
                    heatingState.heatingControl->stopHeating();
                } else {
                    LOG_WARN(TAG, "HeatingControlModule unavailable - clearing state only");
                }
                SRP::clearSystemStateEventBits(SystemEvents::SystemState::HEATING_ON);
                SRP::clearHeatingEventBits(SystemEvents::HeatingEvent::True);
                BurnerRequestManager::clearRequest(BurnerRequestManager::RequestSource::HEATING);
                heatingState.state = HeatingOff;
                
            } else {
                // Update boiler target temperature periodically
                Temperature_t newBoilerTarget = DEFAULT_BOILER_TARGET_TEMP;

                {
                    auto guard = MutexRetryHelper::acquireGuard(
                        SRP::getSensorReadingsMutex(),
                        "SensorReadings-HeatingUpdate"
                    );
                    if (guard) {
                        SharedSensorReadings readings = SRP::getSensorReadings();
                        SystemSettings& settings = SRP::getSystemSettings();

                        if (heatingState.heatingControl != nullptr) {
                            newBoilerTarget = heatingState.heatingControl->calculateSpaceHeatingTargetTemp(readings, settings);
                        }
                        // If heatingControl is nullptr, keep default target - already logged at init
                    }
                }
                
                // Update if changed significantly (>1°C) OR every 5 minutes to refresh watchdog
                static uint32_t lastRefreshTime = 0;
                uint32_t now = millis();
                // H1: Use temp-safe functions for proper overflow handling
                bool targetChanged = tempAbs(tempSub(newBoilerTarget, heatingState.lastBoilerTarget)) > tempFromWhole(1);
                bool needRefresh = (now - lastRefreshTime) > 300000;  // 5 minutes

                if (targetChanged || needRefresh) {
                    BurnerRequestManager::setHeatingRequest(newBoilerTarget, false);
                    heatingState.lastBoilerTarget = newBoilerTarget;
                    lastRefreshTime = now;

                    if (targetChanged) {
                        char tempStr[16];
                        formatTemp(tempStr, sizeof(tempStr), newBoilerTarget);
                        LOG_INFO(TAG, "Updated boiler target: %s°C", tempStr);
                    }
                }
            }
            break;
        }
        
        case HeatingError: {
            // In error state - try to recover
            LOG_WARN(TAG, "Heating in error state - attempting recovery");
            // Check if conditions are now valid
            if (TemperatureSensorFallback::canContinueOperation()) {
                heatingState.state = HeatingOff;
                LOG_INFO(TAG, "Recovered from error state");
            }
            break;
        }
    }
}

static bool checkIfSpaceHeatingNeededEvent() {
    const char* TAG = "HeatingControlTask";

    // Use actual heating state for hysteresis, not a static variable
    // This ensures correct behavior even after state changes from other sources
    bool currentlyHeating = (heatingState.state == HeatingOn);
    bool heatingNeeded = currentlyHeating; // Default to current state

    // Get sensor readings with mutex protection
    if (SRP::takeSensorReadingsMutex(pdMS_TO_TICKS(100))) {
        SharedSensorReadings readings = SRP::getSensorReadings();
        SystemSettings& settings = SRP::getSystemSettings();

        if (readings.isInsideTempValid &&
            (settings.targetTemperatureInside > 0)) {  // Ensure valid setpoint

            Temperature_t setpoint = settings.targetTemperatureInside;
            Temperature_t currentTemp = readings.insideTemp;
            Temperature_t hysteresis = settings.heating_hysteresis;

            // Determine heating state with configurable hysteresis
            if (!currentlyHeating) {
                // Currently OFF - turn ON if temperature drops below (setpoint - hysteresis)
                if (currentTemp < (setpoint - hysteresis)) {
                    heatingNeeded = true;
                    char currBuf[16], setBuf[16], hystBuf[16];
                    formatTemp(currBuf, sizeof(currBuf), currentTemp);
                    formatTemp(setBuf, sizeof(setBuf), setpoint);
                    formatTemp(hystBuf, sizeof(hystBuf), hysteresis);
                    LOG_INFO(TAG, "Heating needed: room %s°C < target %s°C - %s°C",
                            currBuf, setBuf, hystBuf);
                }
            } else {
                // Currently ON - turn OFF if temperature rises above setpoint
                // No hysteresis on the high side - stop as soon as we reach target
                if (currentTemp >= setpoint) {
                    heatingNeeded = false;
                    char currBuf[16], setBuf[16];
                    formatTemp(currBuf, sizeof(currBuf), currentTemp);
                    formatTemp(setBuf, sizeof(setBuf), setpoint);
                    LOG_INFO(TAG, "Heating not needed: room %s°C >= target %s°C",
                            currBuf, setBuf);
                }
            }
        } else {
            LOG_DEBUG(TAG, "Invalid sensor data or setpoint - heating not needed");
            heatingNeeded = false;
        }

        SRP::giveSensorReadingsMutex();
    } else {
        LOG_ERROR(TAG, "Failed to acquire sensor mutex - maintaining current state");
    }

    return heatingNeeded;
}

// === External API for preemption notifications ===

TaskHandle_t getHeatingTaskHandle() {
    return heatingTaskHandle;
}

void notifyHeatingTaskPreempted() {
    // Wake the task immediately to handle preemption
    if (heatingTaskHandle != nullptr) {
        xTaskNotifyGive(heatingTaskHandle);
    }
}