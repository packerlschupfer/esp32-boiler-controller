// src/modules/tasks/WheaterControlTask_EventDriven.cpp
// Water heater control task - manages hot water heating
#include "WheaterControlTask.h"
#include <cmath>  // Round 15 Issue #10: For std::isfinite

#include "shared/SharedResources.h"
#include "shared/Temperature.h"
#include "events/SystemEventsGenerated.h"
#include "modules/control/BurnerRequestManager.h"
#include "modules/control/BurnerSafetyValidator.h"
#include "modules/control/ErrorRecoveryManager.h"
#include "modules/control/TemperatureSensorFallback.h"
#include "utils/ResourceGuard.h"
#include "LoggingMacros.h"
#include "config/ProjectConfig.h"
#include "core/SystemResourceProvider.h"
#include "core/SharedResourceManager.h"
#include <TaskManager.h>

#include "modules/control/BurnerSystemController.h"
#include "modules/control/WheaterControlModule.h"

// Constants
#define DEFAULT_WHEATER_BOILER_TARGET tempFromFloat(65.0f)
#define MIN_BOILER_TEMP tempFromFloat(30.0f)
#define MAX_BOILER_TEMP tempFromFloat(80.0f)
#define DEFAULT_CHARGE_DELTA tempFromFloat(10.0f)  // Default offset if setting invalid

// Timer handles
static TimerHandle_t safetyCheckTimer = nullptr;
static TimerHandle_t processTimer = nullptr;
static TaskHandle_t wheaterTaskHandle = nullptr;

// State tracking - all task state is consolidated here to prevent scattered statics
// NOTE: Only accessed by WheaterControlTask - single task, no mutex needed
static struct {
    WheaterControlState state = WheaterOff;
    Temperature_t lastBoilerTarget = 0;
    bool initialized = false;
    bool lastHeatingNeeded = false;  // Hysteresis state for checkIfWaterHeatingNeededEvent()
} waterState;

// Forward declarations
static void safetyCheckCallback(TimerHandle_t xTimer);
static void processTimerCallback(TimerHandle_t xTimer);
static void processWaterHeatingState();
static bool checkIfWaterHeatingNeededEvent();
static Temperature_t calculateBoilerTarget(const SystemSettings& settings, const SharedSensorReadings& readings);

/**
 * @brief Calculate boiler target temperature for water heating
 *
 * Uses water tank temperature + charge delta offset.
 * Boiler should be 5-10°C hotter than the tank to effectively charge it.
 *
 * @param settings System settings containing charge delta
 * @param readings Current sensor readings
 * @return Boiler target temperature (clamped to MIN_BOILER_TEMP..MAX_BOILER_TEMP)
 */
static Temperature_t calculateBoilerTarget(const SystemSettings& settings, const SharedSensorReadings& readings) {
    const char* TAG = "WaterTarget";
    Temperature_t boilerTarget;

    // Round 15 Issue #10 fix: Check for NaN/Inf before converting float to Temperature_t
    // Get charge delta from settings (convert float to Temperature_t)
    float rawDelta = settings.wHeaterConfTempChargeDelta;
    Temperature_t chargeDelta;
    if (!std::isfinite(rawDelta) || rawDelta < 5.0f || rawDelta > 20.0f) {
        LOG_WARN(TAG, "Invalid charge delta: %.2f, using default", rawDelta);
        chargeDelta = DEFAULT_CHARGE_DELTA;  // Use default 10°C if invalid
    } else {
        chargeDelta = tempFromFloat(rawDelta);
    }

    // Calculate target based on water tank temperature + delta
    // Boiler needs to be hotter than the tank to charge it effectively
    boilerTarget = tempAdd(readings.wHeaterTempTank, chargeDelta);

    char tankBuf[16], deltaBuf[16], targetBuf[16];
    formatTemp(tankBuf, sizeof(tankBuf), readings.wHeaterTempTank);
    formatTemp(deltaBuf, sizeof(deltaBuf), chargeDelta);
    formatTemp(targetBuf, sizeof(targetBuf), boilerTarget);
    LOG_DEBUG(TAG, "Boiler target = tank(%s) + delta(%s) = %s°C",
              tankBuf, deltaBuf, targetBuf);

    // Clamp to valid range - use settings.burner_low_limit as minimum
    Temperature_t minTemp = settings.burner_low_limit;
    if (minTemp < MIN_BOILER_TEMP) {
        minTemp = MIN_BOILER_TEMP;  // Absolute minimum safety limit
    }
    if (boilerTarget < minTemp) {
        boilerTarget = minTemp;
    }
    if (boilerTarget > MAX_BOILER_TEMP) {
        boilerTarget = MAX_BOILER_TEMP;
    }

    return boilerTarget;
}

// Static pointer to BurnerSystemController - received via task parameter
static BurnerSystemController* burnerSystemController = nullptr;

void WheaterControlTask(void *parameter) {
    const char* TAG = "WheaterControlTask";

    // Receive BurnerSystemController via parameter (Pattern B: parameter passing)
    burnerSystemController = static_cast<BurnerSystemController*>(parameter);
    if (burnerSystemController) {
        LOG_INFO(TAG, "Received BurnerSystemController via parameter");
    } else {
        LOG_WARN(TAG, "BurnerSystemController not provided - heat recovery disabled");
    }

    wheaterTaskHandle = xTaskGetCurrentTaskHandle();

    LOG_INFO(TAG, "Started (Event-Driven) - Core: %d", xPortGetCoreID());
    LOG_INFO(TAG, "Stack high water mark: %d words", uxTaskGetStackHighWaterMark(NULL));
    
    // Watchdog will be registered after initialization
    
    // Create process timer (runs every 5 seconds like MB8ART)
    processTimer = xTimerCreate(
        "WaterProcess",
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
        "WaterSafety",
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

    waterState.initialized = true;

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
    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::enabled(
        false,  // not critical (won't reset system)
        SystemConstants::System::WDT_WHEATER_CONTROL_MS
    );

    if (!SRP::getTaskManager().registerCurrentTaskWithWatchdog("WheaterControl", wdtConfig)) {
        LOG_ERROR(TAG, "Failed to register with watchdog - continuing without watchdog protection");
    } else {
        LOG_INFO(TAG, "WDT OK %lums", SystemConstants::System::WDT_WHEATER_CONTROL_MS);
        (void)SRP::getTaskManager().feedWatchdog();  // Feed immediately
    }
    
    // Main task loop - use timer notifications like MB8ART
    while (true) {
        // Wait for timer notification with 1 second timeout for watchdog
        uint32_t ulNotificationValue = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        
        if (ulNotificationValue > 0) {
            // Timer triggered - process water heating state
            processWaterHeatingState();
        }
        
        // Also check for immediate control events
        EventBits_t controlBits = xEventGroupGetBits(SRP::getControlRequestsEventGroup());
        const EventBits_t CONTROL_EVENTS = SystemEvents::ControlRequest::WATER_ON_OVERRIDE | 
                                          SystemEvents::ControlRequest::WATER_OFF_OVERRIDE;
        
        if (controlBits & CONTROL_EVENTS) {
            // Control events trigger immediate processing
            xEventGroupClearBits(SRP::getControlRequestsEventGroup(), controlBits & CONTROL_EVENTS);
            processWaterHeatingState();
        }

        // Feed watchdog
        (void)SRP::getTaskManager().feedWatchdog();
    }
}

static void processTimerCallback(TimerHandle_t xTimer) {
    (void)xTimer;
    // Notify task to process - like MB8ART pattern
    if (wheaterTaskHandle != nullptr) {
        xTaskNotifyGive(wheaterTaskHandle);
    }
}

static void safetyCheckCallback(TimerHandle_t xTimer) {
    (void)xTimer;
    const char* TAG = "WaterSafety";
    
    // Safety check - ensure we haven't missed sensor updates for too long
    if (waterState.state == WheaterOn) {
        // Check if sensors are still available
        if (!TemperatureSensorFallback::canContinueOperation()) {
            LOG_ERROR(TAG, "Sensor failure detected during water heating - shutting down");
            
            // Turn off circulation pump
            xEventGroupSetBits(SRP::getRelayEventGroup(), SystemEvents::RelayControl::WATER_PUMP_OFF);
            
            // Stop water heating PID task
            WheaterControlModule* wheaterControl = SRP::getWheaterControl();
            if (wheaterControl != nullptr) {
                wheaterControl->stopWaterHeatingPIDTask();
            }

            // Clear water heating state
            SRP::clearSystemStateEventBits(SystemEvents::SystemState::WATER_ON);

            // Clear burner request
            BurnerRequestManager::clearRequest(BurnerRequestManager::RequestSource::WATER);

            waterState.state = WheaterOff;
        }
    }
}

static void processWaterHeatingState() {
    const char* TAG = "WaterProcess";
    EventBits_t systemStateBits = SRP::getSystemStateEventBits();
    
    // Check if both boiler and water heating are enabled
    if (!(systemStateBits & SystemEvents::SystemState::BOILER_ENABLED) || 
        !(systemStateBits & SystemEvents::SystemState::WATER_ENABLED)) {
        
        // System disabled - turn off water heating if on
        if (waterState.state == WheaterOn) {
            LOG_INFO(TAG, "System disabled - turning off water heating");
            
            // Turn off circulation pump
            xEventGroupSetBits(SRP::getRelayEventGroup(), SystemEvents::RelayControl::WATER_PUMP_OFF);
            
            // Stop water heating PID task
            WheaterControlModule* wheaterControl = SRP::getWheaterControl();
            if (wheaterControl != nullptr) {
                wheaterControl->stopWaterHeatingPIDTask();
            }

            // Clear water heating state
            SRP::clearSystemStateEventBits(SystemEvents::SystemState::WATER_ON);

            // Clear burner request
            BurnerRequestManager::clearRequest(BurnerRequestManager::RequestSource::WATER);

            waterState.state = WheaterOff;
        }
        return;
    }

    // Operation mode is managed by BurnerControlTask - don't set it here
    
    // Get control bits
    EventBits_t controlBits = SRP::getControlRequestsEventBits();
    
    switch (waterState.state) {
        case WheaterOff: {
            // Check if we should turn on water heating
            if (checkIfWaterHeatingNeededEvent() || (controlBits & SystemEvents::ControlRequest::WATER_ON_OVERRIDE)) {
                // Check if we can start water heating
                if (!TemperatureSensorFallback::canContinueOperation()) {
                    LOG_WARN(TAG, "Cannot start water heating - required sensors unavailable");
                    return;
                }

                // No priority lock check needed - water heating has its own priority flag

                // Get settings and sensor readings for boiler target calculation
                SystemSettings& settings = SRP::getSystemSettings();
                SharedSensorReadings readings = SRP::getSensorReadings();

                // Calculate boiler target: returnTemp + chargeDelta
                Temperature_t boilerTargetTemp = calculateBoilerTarget(settings, readings);

                // Turn on circulation pump
                xEventGroupSetBits(SRP::getRelayEventGroup(), SystemEvents::RelayControl::WATER_PUMP_ON);

                // Set water heating state
                SRP::setSystemStateEventBits(SystemEvents::SystemState::WATER_ON);

                // Request burner operation with priority based on system settings
                BurnerRequestManager::setWaterRequest(boilerTargetTemp, true, settings.wheaterPriorityEnabled);

                LOG_INFO(TAG, "Water pump requested and water heating state set");

                waterState.state = WheaterOn;
                waterState.lastBoilerTarget = boilerTargetTemp;

                // Start water heating PID task for power modulation
                WheaterControlModule* wheaterControl = SRP::getWheaterControl();
                if (wheaterControl != nullptr) {
                    wheaterControl->startWaterHeatingPIDTask();
                }

                char tempStr[16];
                formatTemp(tempStr, sizeof(tempStr), boilerTargetTemp);
                LOG_INFO(TAG, "Water heating activated - Boiler target: %s°C", tempStr);
            }
            break;
        }
        
        case WheaterOn: {
            // Check if we should turn off water heating
            bool waterStillNeeded = checkIfWaterHeatingNeededEvent();
            bool sensorsAvailable = TemperatureSensorFallback::canContinueOperation();
            
            if ((controlBits & SystemEvents::ControlRequest::WATER_OFF_OVERRIDE) || 
                !(systemStateBits & SystemEvents::SystemState::BOILER_ENABLED) ||
                !(systemStateBits & SystemEvents::SystemState::WATER_ENABLED) ||
                !waterStillNeeded ||
                !sensorsAvailable) {
                
                // Determine reason for shutdown
                const char* reason = "unknown";
                if (controlBits & SystemEvents::ControlRequest::WATER_OFF_OVERRIDE) {
                    reason = "remote override OFF";
                } else if (!(systemStateBits & SystemEvents::SystemState::BOILER_ENABLED)) {
                    reason = "boiler disabled";
                } else if (!(systemStateBits & SystemEvents::SystemState::WATER_ENABLED)) {
                    reason = "water heating disabled";
                } else if (!waterStillNeeded) {
                    reason = "target temperature reached";
                } else if (!sensorsAvailable) {
                    reason = "sensor failure";
                }
                
                LOG_INFO(TAG, "Deactivating water heating - reason: %s", reason);
                
                // Turn off circulation pump
                xEventGroupSetBits(SRP::getRelayEventGroup(), SystemEvents::RelayControl::WATER_PUMP_OFF);
                
                // Clear water heating state
                SRP::clearSystemStateEventBits(SystemEvents::SystemState::WATER_ON);
                
                // Clear burner request
                BurnerRequestManager::clearRequest(BurnerRequestManager::RequestSource::WATER);

                // Try heat recovery: use residual boiler heat for space heating
                EventBits_t systemBits = SRP::getSystemStateEventBits();
                bool heatingEnabled = (systemBits & SystemEvents::SystemState::HEATING_ENABLED) != 0;

                if (heatingEnabled && burnerSystemController != nullptr) {
                    // Check if heating is actually needed
                    SystemSettings& settings = SRP::getSystemSettings();
                    SharedSensorReadings sensorReadings = SRP::getSensorReadings();
                    Temperature_t insideTemp = sensorReadings.insideTemp;
                    Temperature_t targetTemp = settings.targetTemperatureInside;
                    Temperature_t hysteresis = settings.heating_hysteresis;

                    if (tempLess(insideTemp, tempSub(targetTemp, hysteresis))) {
                        // Room needs heat - try to use residual boiler heat
                        auto result = burnerSystemController->switchToHeatRecovery();

                        if (result.isSuccess()) {
                            LOG_INFO(TAG, "Switched to heat recovery mode - using residual heat for heating");
                            // Don't notify WATER_PRIORITY_RELEASED yet (still using boiler)
                        } else {
                            LOG_DEBUG(TAG, "Heat recovery not available: %s", result.message().c_str());
                            xEventGroupSetBits(SRP::getControlRequestsEventGroup(),
                                              SystemEvents::ControlRequest::WATER_PRIORITY_RELEASED);
                        }
                    } else {
                        // Room doesn't need heat - normal water off
                        xEventGroupSetBits(SRP::getControlRequestsEventGroup(),
                                          SystemEvents::ControlRequest::WATER_PRIORITY_RELEASED);
                    }
                } else {
                    // Heating not enabled or controller not ready - normal water off
                    xEventGroupSetBits(SRP::getControlRequestsEventGroup(),
                                      SystemEvents::ControlRequest::WATER_PRIORITY_RELEASED);
                }

                // Stop water heating PID task
                WheaterControlModule* wheaterControl = SRP::getWheaterControl();
                if (wheaterControl != nullptr) {
                    wheaterControl->stopWaterHeatingPIDTask();
                }

                waterState.state = WheaterOff;
                
            } else {
                // Recalculate boiler target (return temp + delta) and update if changed
                SystemSettings& settings = SRP::getSystemSettings();
                SharedSensorReadings readings = SRP::getSensorReadings();
                Temperature_t newBoilerTarget = calculateBoilerTarget(settings, readings);

                // Update if target changed by more than 1°C OR every 5 minutes to refresh watchdog
                Temperature_t diff = tempAbs(tempSub(newBoilerTarget, waterState.lastBoilerTarget));
                static uint32_t lastRefreshTime = 0;
                uint32_t now = millis();
                bool targetChanged = (diff > tempFromWhole(1));
                bool needRefresh = (now - lastRefreshTime) > 300000;  // 5 minutes

                if (targetChanged || needRefresh) {
                    BurnerRequestManager::setWaterRequest(newBoilerTarget, true, settings.wheaterPriorityEnabled);
                    waterState.lastBoilerTarget = newBoilerTarget;
                    lastRefreshTime = now;

                    // Only log when target actually changed (not on watchdog refresh)
                    if (targetChanged) {
                        char tempStr[16];
                        formatTemp(tempStr, sizeof(tempStr), newBoilerTarget);
                        LOG_INFO(TAG, "Updated boiler target: %s°C", tempStr);
                    }
                }
            }
            break;
        }
        
        case WheaterError: {
            // In error state - try to recover
            LOG_WARN(TAG, "Water heating in error state - attempting recovery");
            // Check if conditions are now valid
            if (TemperatureSensorFallback::canContinueOperation()) {
                // Stop water heating PID task if running
                WheaterControlModule* wheaterControl = SRP::getWheaterControl();
                if (wheaterControl != nullptr) {
                    wheaterControl->stopWaterHeatingPIDTask();
                }
                waterState.state = WheaterOff;
                LOG_INFO(TAG, "Recovered from error state");
            }
            break;
        }
    }
}

static bool checkIfWaterHeatingNeededEvent() {
    bool heatingNeeded = waterState.lastHeatingNeeded; // Default to current state
    const char* TAG = "WheaterControlTask";

    // Get sensor readings with mutex protection
    if (SRP::takeSensorReadingsMutex(pdMS_TO_TICKS(100))) {
        SharedSensorReadings readings = SRP::getSensorReadings();
        SystemSettings& settings = SRP::getSystemSettings();

        if (readings.isWHeaterTempTankValid &&
            (settings.wHeaterConfTempLimitHigh > 0) &&
            (settings.wHeaterConfTempLimitLow > 0)) {  // Ensure valid thresholds

            Temperature_t currentTemp = readings.wHeaterTempTank;
            Temperature_t lowLimit = settings.wHeaterConfTempLimitLow;   // Start heating below this
            Temperature_t highLimit = settings.wHeaterConfTempLimitHigh; // Stop heating above this

            // Simple two-threshold control (no symmetric hysteresis calculation)
            if (!waterState.lastHeatingNeeded) {
                // Currently OFF - turn ON if temperature drops below low limit
                if (currentTemp < lowLimit) {
                    heatingNeeded = true;
                    char currBuf[16], lowBuf[16];
                    formatTemp(currBuf, sizeof(currBuf), currentTemp);
                    formatTemp(lowBuf, sizeof(lowBuf), lowLimit);
                    LOG_INFO(TAG, "Water heating needed: tank %s°C < low limit %s°C",
                            currBuf, lowBuf);
                }
            } else {
                // Currently ON - turn OFF if temperature rises above high limit
                if (currentTemp > highLimit) {
                    heatingNeeded = false;
                    char currBuf[16], highBuf[16];
                    formatTemp(currBuf, sizeof(currBuf), currentTemp);
                    formatTemp(highBuf, sizeof(highBuf), highLimit);
                    LOG_INFO(TAG, "Water heating complete: tank %s°C > high limit %s°C",
                            currBuf, highBuf);
                }
            }

            // Update state
            waterState.lastHeatingNeeded = heatingNeeded;
        } else {
            LOG_DEBUG(TAG, "Invalid sensor data or thresholds - water heating not needed");
            heatingNeeded = false;
        }

        SRP::giveSensorReadingsMutex();
    } else {
        LOG_ERROR(TAG, "Failed to acquire sensor mutex - maintaining current state");
    }

    return heatingNeeded;
}