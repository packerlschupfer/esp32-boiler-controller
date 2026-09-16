// src/modules/control/TemperatureSensorFallback.cpp
#include "modules/control/TemperatureSensorFallback.h"
#include "modules/control/SafetyInterlocks.h"
#include "modules/control/CentralizedFailsafe.h"
#include "core/SystemResourceProvider.h"
#include "shared/SharedSensorReadings.h"
#include "shared/Temperature.h"  // For tempToFloat
#include "events/SystemEventsGenerated.h"
#include "utils/ErrorHandler.h"
#include "utils/MutexRetryHelper.h"
#include "monitoring/HealthMonitor.h"
#include "config/SystemConstants.h"
#include "config/SafetyConfig.h"  // M3: For standardized sensorStaleMs
#include "LoggingMacros.h"
#include "modules/tasks/MQTTTask.h"
#include "MQTTTopics.h"
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/timers.h>

static const char* TAG = "TempSensorFallback";

// Guards currentStatus, the hysteresis counters and the mode change side effects (retained
// MQTT publishes, error bits, health monitor, emergency release) as one sequence (review R4-3).
// Callers: BurnerControlTask (priority 4), HeatingControlTask and WheaterControlTask
// (priority 3), their 60 s safetyCheckCallback timers (timer daemon task) and the MQTT
// emergency_reset command (CentralizedFailsafe::clearEmergencyStop).
// Lock order: this mutex first; under it the sensor readings mutex, the health monitor mutex,
// the logger and the MQTT queues (no wait) are used. None of their holders calls into
// TemperatureSensorFallback. Read-only getters (getCurrentMode, getStatus) stay lock-free.
static SemaphoreHandle_t fallbackMutex = nullptr;
static constexpr TickType_t FALLBACK_MUTEX_TIMEOUT = pdMS_TO_TICKS(50);

static bool takeFallbackMutex() {
    if (fallbackMutex == nullptr) {
        return true;  // before initialize() (creation failure is logged there)
    }
    // Never block the timer daemon task (HeatingControlTask/WheaterControlTask safetyCheckCallback)
    const bool inTimerTask = (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) &&
                             (xTaskGetCurrentTaskHandle() == xTimerGetTimerDaemonTaskHandle());
    return xSemaphoreTake(fallbackMutex, inTimerTask ? 0 : FALLBACK_MUTEX_TIMEOUT) == pdTRUE;
}

static void giveFallbackMutex() {
    if (fallbackMutex != nullptr) {
        xSemaphoreGive(fallbackMutex);
    }
}

// Helper function to publish sensor fallback status immediately on mode change
static void publishFallbackModeChange(TemperatureSensorFallback::FallbackMode newMode,
                                       TemperatureSensorFallback::FallbackMode previousMode,
                                       const TemperatureSensorFallback::SensorStatus& status) {
    // Build JSON payload
    JsonDocument doc;  // ArduinoJson v7

    // Mode as string and numeric
    const char* modeNames[] = {"STARTUP", "NORMAL", "SHUTDOWN"};
    doc["mode"] = modeNames[static_cast<int>(newMode)];
    doc["mode_id"] = static_cast<int>(newMode);
    doc["previous_mode"] = modeNames[static_cast<int>(previousMode)];

    // Add missing sensors if degraded
    if (newMode != TemperatureSensorFallback::FallbackMode::NORMAL) {
        JsonArray missing = doc["missing"].to<JsonArray>();
        if (status.missingBoilerOutput) missing.add("boiler_output");
        if (status.missingBoilerReturn) missing.add("boiler_return");
        if (status.missingWaterTemp) missing.add("water_tank");
        if (status.missingRoomTemp) missing.add("room_temp");
    }

    doc["timestamp"] = millis();

    // Serialize and publish with HIGH priority and RETAIN flag
    char buffer[192];
    serializeJson(doc, buffer, sizeof(buffer));

    // Publish to dedicated retained topic for immediate notification to subscribers
    MQTTTask::publish(MQTT_STATUS_SENSOR_FALLBACK, buffer, 0, true, MQTTPriority::PRIORITY_HIGH);

    // Also publish simple mode string to mode topic (retained)
    MQTTTask::publish(MQTT_STATUS_SENSOR_MODE, modeNames[static_cast<int>(newMode)], 0, true, MQTTPriority::PRIORITY_HIGH);
}

// Static member definitions
TemperatureSensorFallback::SensorStatus TemperatureSensorFallback::currentStatus;
uint32_t TemperatureSensorFallback::initializationTime = 0;
uint8_t TemperatureSensorFallback::consecutiveValidCount = 0;
uint8_t TemperatureSensorFallback::consecutiveInvalidCount = 0;

void TemperatureSensorFallback::initialize() {
    LOG_INFO(TAG, "Initializing temperature sensor fallback system with hysteresis");
    // Called by SystemInitializer and again by BurnerControlTask: create the mutex once
    if (fallbackMutex == nullptr) {
        fallbackMutex = xSemaphoreCreateMutex();
        if (fallbackMutex == nullptr) {
            LOG_ERROR(TAG, "Failed to create fallback mutex - state updates unguarded");
        }
    }
    const bool locked = takeFallbackMutex();
    currentStatus = SensorStatus(); // Reset to defaults
    initializationTime = millis();
    consecutiveValidCount = 0;
    consecutiveInvalidCount = 0;

    // Start in STARTUP mode
    currentStatus.currentMode = FallbackMode::STARTUP;
    currentStatus.currentOperationMode = OperationMode::NONE;
    if (locked) {
        giveFallbackMutex();
    }
}

void TemperatureSensorFallback::cleanup() {
    // The mutex is kept: another task may still call in
    const bool locked = takeFallbackMutex();
    currentStatus = SensorStatus();
    initializationTime = 0;
    consecutiveValidCount = 0;
    consecutiveInvalidCount = 0;
    if (locked) {
        giveFallbackMutex();
    }
    LOG_INFO(TAG, "Temperature sensor fallback cleaned up");
}

void TemperatureSensorFallback::setOperationMode(OperationMode mode) {
    if (!takeFallbackMutex()) {
        // Another task is evaluating; BurnerControlTask sets the mode again on its next
        // sensor read or request change
        LOG_WARN(TAG, "setOperationMode: mutex timeout - operation mode not changed");
        return;
    }
    if (currentStatus.currentOperationMode != mode) {
        const char* modeNames[] = {"NONE", "SPACE_HEATING", "WATER_HEATING", "BOTH"};
        LOG_INFO(TAG, "Operation mode changed: %s -> %s",
                modeNames[static_cast<int>(currentStatus.currentOperationMode)],
                modeNames[static_cast<int>(mode)]);
        currentStatus.currentOperationMode = mode;

        // Force immediate sensor status update when mode changes
        updateSensorStatusLocked();
    }
    giveFallbackMutex();
}

TemperatureSensorFallback::FallbackMode TemperatureSensorFallback::updateSensorStatus() {
    if (!takeFallbackMutex()) {
        // Another task (or the timer task found the mutex taken): report the last mode and
        // change nothing - the holder is evaluating the same readings
        return currentStatus.currentMode;
    }
    const FallbackMode mode = updateSensorStatusLocked();
    giveFallbackMutex();
    return mode;
}

TemperatureSensorFallback::FallbackMode TemperatureSensorFallback::updateSensorStatusLocked() {
    uint32_t now = millis();
    
    // Reset missing sensor flags
    currentStatus.missingBoilerOutput = false;
    currentStatus.missingBoilerReturn = false;
    currentStatus.missingWaterTemp = false;
    currentStatus.missingRoomTemp = false;
    
    // Get current sensor readings
    {
        auto guard = MutexRetryHelper::acquireGuard(
            SRP::getSensorReadingsMutex(),
            "SensorReadings-Fallback"
        );
        if (guard) {
            const auto& readings = SRP::getSensorReadings();

            // Check if sensor data is fresh
            // M3: Use configurable SafetyConfig::sensorStaleMs instead of hardcoded SENSOR_TIMEOUT_MS
            // F5: freshness is per-source. Boiler/water sensors come from the
            // MB8ART (lastBoilerTempUpdateTimestamp); the room sensor comes from
            // the ANDRTF3 (lastUpdateTimestamp). Gating boiler validity on the
            // shared timestamp let a live room sensor mask a dead MB8ART.
            bool boilerDataFresh = isSensorDataFresh(readings.lastBoilerTempUpdateTimestamp, SafetyConfig::sensorStaleMs);
            bool roomDataFresh   = isSensorDataFresh(readings.lastUpdateTimestamp, SafetyConfig::sensorStaleMs);

            if (boilerDataFresh) {
                // Update sensor validity - pass Temperature_t directly, no conversion needed
                currentStatus.boilerOutputValid = readings.isBoilerTempOutputValid &&
                                                 validateSensorReading(readings.boilerTempOutput);
                currentStatus.boilerReturnValid = readings.isBoilerTempReturnValid &&
                                                 validateSensorReading(readings.boilerTempReturn);
                currentStatus.waterTempValid = readings.isWaterHeaterTempTankValid &&
                                              validateSensorReading(readings.waterHeaterTempTank);
                currentStatus.outsideTempValid = readings.isOutsideTempValid &&
                                                validateSensorReading(readings.outsideTemp);
            } else {
                // Boiler/water data stale - mark those sensors invalid
                LOG_WARN(TAG, "Boiler sensor data is stale (age: %ld ms) - marking boiler/water sensors invalid",
                         now - readings.lastBoilerTempUpdateTimestamp);
                currentStatus.boilerOutputValid = false;
                currentStatus.boilerReturnValid = false;
                currentStatus.waterTempValid = false;
                currentStatus.outsideTempValid = false;
            }

            if (roomDataFresh) {
                currentStatus.roomTempValid = readings.isInsideTempValid &&
                                             validateSensorReading(readings.insideTemp);
            } else {
                currentStatus.roomTempValid = false;
            }
        }
    }
    
    // Determine fallback mode based on sensor availability with hysteresis
    // This prevents mode chattering when sensors are intermittently failing
    FallbackMode previousMode = currentStatus.currentMode;
    bool sensorsCurrentlyValid = hasRequiredSensors();

    // Update hysteresis counters
    if (sensorsCurrentlyValid) {
        consecutiveValidCount++;
        consecutiveInvalidCount = 0;
        if (consecutiveValidCount > 100) consecutiveValidCount = 100;  // Prevent overflow
    } else {
        consecutiveInvalidCount++;
        consecutiveValidCount = 0;
        if (consecutiveInvalidCount > 100) consecutiveInvalidCount = 100;

        // Track which sensors are missing for error reporting
        currentStatus.missingBoilerOutput = !currentStatus.boilerOutputValid;
        currentStatus.missingBoilerReturn = !currentStatus.boilerReturnValid &&
                                           (currentStatus.currentOperationMode == OperationMode::SPACE_HEATING ||
                                            currentStatus.currentOperationMode == OperationMode::BOTH);
        currentStatus.missingWaterTemp = !currentStatus.waterTempValid &&
                                        (currentStatus.currentOperationMode == OperationMode::WATER_HEATING ||
                                         currentStatus.currentOperationMode == OperationMode::BOTH);
        currentStatus.missingRoomTemp = !currentStatus.roomTempValid &&
                                       (currentStatus.currentOperationMode == OperationMode::SPACE_HEATING ||
                                        currentStatus.currentOperationMode == OperationMode::BOTH);
    }

    // Apply hysteresis to mode transitions
    bool inStartupPeriod = (now - initializationTime) < STARTUP_PERIOD_MS;

    switch (currentStatus.currentMode) {
        case FallbackMode::STARTUP:
            // From STARTUP: need VALID_COUNT_TO_ENTER_NORMAL consecutive valid readings
            if (consecutiveValidCount >= VALID_COUNT_TO_ENTER_NORMAL) {
                currentStatus.currentMode = FallbackMode::NORMAL;
                LOG_INFO(TAG, "Sensors stable - transitioning to NORMAL mode");
            } else if (!inStartupPeriod && consecutiveInvalidCount >= INVALID_COUNT_TO_SHUTDOWN) {
                // After startup period, if sensors consistently invalid, go to SHUTDOWN
                currentStatus.currentMode = FallbackMode::SHUTDOWN;
            }
            break;

        case FallbackMode::NORMAL:
            // From NORMAL: need INVALID_COUNT_TO_SHUTDOWN consecutive invalid readings to shutdown
            if (consecutiveInvalidCount >= INVALID_COUNT_TO_SHUTDOWN) {
                currentStatus.currentMode = FallbackMode::SHUTDOWN;
                LOG_WARN(TAG, "Sensors failed %d consecutive checks - transitioning to SHUTDOWN",
                         consecutiveInvalidCount);
            }
            break;

        case FallbackMode::SHUTDOWN:
            // From SHUTDOWN: need VALID_COUNT_TO_ENTER_NORMAL consecutive valid readings to recover
            if (consecutiveValidCount >= VALID_COUNT_TO_ENTER_NORMAL) {
                currentStatus.currentMode = FallbackMode::NORMAL;
                LOG_INFO(TAG, "Sensors recovered after %d consecutive valid checks - transitioning to NORMAL",
                         consecutiveValidCount);
            }
            break;
    }
    
    // Error bits follow the missing sensor (2026-09-16, user decision): only a missing boiler
    // output reading leaves the overheat protection blind, so only that sets SENSOR_FAILURE,
    // which SafetyInterlocks::checkSystemErrors() treats as critical (emergency stop within the
    // 5 s full check). A missing room/tank/return reading marks degraded operation only: the
    // mode task stops the burner normally and BurnerControlTask's 10 s confirmation
    // (SensorFailureConfirm) handles a demand that persists. Re-evaluated on every update so a
    // boiler output failing later inside SHUTDOWN still sets the bit.
    if (currentStatus.currentMode == FallbackMode::SHUTDOWN) {
        xEventGroupSetBits(SRP::getErrorNotificationEventGroup(),
                           SystemEvents::GeneralSystem::SENSOR_DEGRADED);
        if (!currentStatus.boilerOutputValid) {
            xEventGroupSetBits(SRP::getErrorNotificationEventGroup(),
                               SystemEvents::Error::SENSOR_FAILURE);
        } else {
            xEventGroupClearBits(SRP::getErrorNotificationEventGroup(),
                                 SystemEvents::Error::SENSOR_FAILURE);
        }
    }

    // Log mode changes
    if (currentStatus.currentMode != previousMode) {
        const char* modeNames[] = {"STARTUP", "NORMAL", "SHUTDOWN"};
        LOG_INFO(TAG, "Fallback mode changed: %s -> %s",
                modeNames[static_cast<int>(previousMode)],
                modeNames[static_cast<int>(currentStatus.currentMode)]);

        // Publish mode change immediately via MQTT (retained for subscribers)
        publishFallbackModeChange(currentStatus.currentMode, previousMode, currentStatus);

        if (currentStatus.currentMode == FallbackMode::SHUTDOWN) {
            LOG_ERROR(TAG, "SHUTDOWN: %s", getMissingSensorMessage());

            // Error bits are set above, depending on which sensor is missing

            // Record in health monitor
            HealthMonitor* healthMonitor = SRP::getHealthMonitor();
            if (healthMonitor) {
                healthMonitor->recordError(HealthMonitor::Subsystem::SENSORS,
                                         SystemError::SENSOR_FAILURE);
            }
        } else if (currentStatus.currentMode == FallbackMode::NORMAL) {
            // Clear error bits when returning to normal
            xEventGroupClearBits(SRP::getErrorNotificationEventGroup(),
                                SystemEvents::Error::SENSOR_FAILURE | SystemEvents::GeneralSystem::SENSOR_DEGRADED);

            // Attempt to recover from emergency shutdown if sensors have recovered
            if (previousMode == FallbackMode::SHUTDOWN) {
                LOG_INFO(TAG, "Sensors recovered - attempting to clear emergency shutdown");
                // Releases EMERGENCY_STOP only if stale sensor data caused it; a critical
                // temperature or request watchdog stop needs boiler/cmd/emergency_reset
                CentralizedFailsafe::releaseAfterSensorRecovery();

                // Record recovery in health monitor
                HealthMonitor* healthMonitor = SRP::getHealthMonitor();
                if (healthMonitor) {
                    healthMonitor->recordSuccess(HealthMonitor::Subsystem::SENSORS);
                }

                // Publish recovery notification
                MQTTTask::publish(MQTT_STATUS_SENSOR_FALLBACK "/recovery",
                                 "{\"recovered\":true}", 0, false,
                                 MQTTPriority::PRIORITY_HIGH);
            }
        }
    }
    
    return currentStatus.currentMode;
}

bool TemperatureSensorFallback::hasRequiredSensors() {
    // Always need boiler output temperature
    if (!currentStatus.boilerOutputValid) {
        return false;
    }
    
    // Check additional requirements based on operation mode
    switch (currentStatus.currentOperationMode) {
        case OperationMode::SPACE_HEATING:
            // Need boiler return and room temperature for space heating
            return currentStatus.boilerReturnValid && currentStatus.roomTempValid;
            
        case OperationMode::WATER_HEATING:
            // Need water tank temperature for water heating
            return currentStatus.waterTempValid;
            
        case OperationMode::BOTH:
            // Need all sensors when both modes active
            return currentStatus.boilerReturnValid && 
                   currentStatus.roomTempValid && 
                   currentStatus.waterTempValid;
            
        case OperationMode::NONE:
            // If no operation mode set yet, check for basic sensor availability
            // This allows the system to start up even without heating/water requests
            // Just need boiler output sensor as a minimum for safe operation
            return currentStatus.boilerOutputValid;
            
        default:
            return false;
    }
}

bool TemperatureSensorFallback::canContinueOperation() {
    // Decide on the mode this call evaluated, not a later re-read of the shared status
    FallbackMode mode = updateSensorStatus();

    switch (mode) {
        case FallbackMode::STARTUP:
            // During startup, allow operation if sensors become available
            // Re-check sensor status to potentially transition to NORMAL
            mode = updateSensorStatus();
            if (mode == FallbackMode::NORMAL) {
                return true;
            }
            // Still in startup - log occasionally
            {
                static uint32_t lastStartupLogTime = 0;
                uint32_t now = millis();
                if (now - lastStartupLogTime > 5000) {  // Log every 5 seconds
                    LOG_INFO(TAG, "STARTUP mode - waiting for sensors");
                    lastStartupLogTime = now;
                }
            }
            return false;
            
        case FallbackMode::NORMAL:
            // All required sensors working - full operation allowed
            return true;
            
        case FallbackMode::SHUTDOWN:
            // Missing critical sensors - operation not allowed
            // Note: Emergency shutdown is triggered once on mode change (in updateStatus)
            // Here we just return false without retriggering to allow recovery
            {
                static uint32_t lastShutdownLogTime = 0;
                uint32_t now = millis();
                if (now - lastShutdownLogTime > 10000) {  // Log every 10 seconds
                    LOG_WARN(TAG, "SHUTDOWN mode - waiting for sensor recovery");
                    lastShutdownLogTime = now;
                }
            }
            return false;
            
        default:
            return false;
    }
}

void TemperatureSensorFallback::getSafeOperatingParams(Temperature_t& maxTemp, float& maxPower, uint32_t& runTime) {
    switch (currentStatus.currentMode) {
        case FallbackMode::STARTUP:
            // No operation during startup
            maxTemp = 0;
            maxPower = 0.0f;
            runTime = 0;
            break;
            
        case FallbackMode::NORMAL:
            // Normal operation - full parameters
            maxTemp = SystemConstants::Temperature::MAX_BOILER_TEMP_C;
            maxPower = 1.0f; // 100%
            runTime = UINT32_MAX; // No time limit
            break;
            
        case FallbackMode::SHUTDOWN:
            // No operation allowed
            maxTemp = 0;
            maxPower = 0.0f;
            runTime = 0;
            break;
    }
}

bool TemperatureSensorFallback::validateSensorReading(Temperature_t temp, Temperature_t minValid, Temperature_t maxValid) {
    // Check for special invalid values first
    if (temp == TEMP_INVALID || temp == TEMP_UNKNOWN) {
        return false;
    }
    
    // Check if temperature is within reasonable range
    if (temp < minValid || temp > maxValid) {
        return false;
    }
    
    // Check for other special invalid values
    if (temp == -9990 || temp == 9990) {  // -999.0 and 999.0 in tenths
        return false;
    }
    
    return true;
}

bool TemperatureSensorFallback::isSensorDataFresh(uint32_t lastUpdateTime, uint32_t maxAge) {
    // Check if data has ever been received (timestamp 0 means uninitialized)
    if (lastUpdateTime == 0) {
        return false; // No data received yet
    }

    uint32_t now = millis();

    // Calculate age handling wrap-around correctly
    // Due to unsigned arithmetic, (now - lastUpdateTime) handles wrap-around automatically
    // as long as the actual elapsed time is less than 2^32 ms (~49 days)
    // If lastUpdateTime is from before wrap and now is after, the subtraction still gives
    // the correct elapsed time due to unsigned overflow behavior
    uint32_t age = now - lastUpdateTime;

    // Sanity check: if age is impossibly large (> 1 hour), something is wrong
    // This catches cases where lastUpdateTime was corrupted or system time jumped
    const uint32_t MAX_REASONABLE_AGE = 3600000; // 1 hour in ms
    if (age > MAX_REASONABLE_AGE) {
        LOG_WARN("TempFallback", "Sensor timestamp sanity check failed: age=%lu ms", age);
        return false; // Consider stale if timestamp seems corrupted
    }

    return age < maxAge;
}

const char* TemperatureSensorFallback::getMissingSensorMessage() {
    // THREAD-SAFETY: Static buffer safe - called from Sensor/Control task only (error logging)
    // Alternative: 128B on Sensor task stack (~800B free) is risky for error path
    // See: docs/MEMORY_OPTIMIZATION.md
    static char message[128];  // Max ~110 chars for sensor messages
    int offset = 0;
    
    // Special case for NONE operation mode
    if (currentStatus.currentOperationMode == OperationMode::NONE) {
        // Check basic sensor availability
        if (!currentStatus.boilerOutputValid) {
            snprintf(message, sizeof(message), "Boiler output sensor missing (waiting for heating request)");
        } else {
            // This shouldn't happen anymore as NONE mode now allows operation with just boiler output
            snprintf(message, sizeof(message), "System idle - waiting for heating or water request");
        }
        return message;
    }
    
    offset += snprintf(message + offset, sizeof(message) - offset, "Missing sensors: ");
    
    bool first = true;
    
    if (currentStatus.missingBoilerOutput) {
        offset += snprintf(message + offset, sizeof(message) - offset, "Boiler Output");
        first = false;
    }
    
    if (currentStatus.missingBoilerReturn) {
        if (!first) offset += snprintf(message + offset, sizeof(message) - offset, ", ");
        offset += snprintf(message + offset, sizeof(message) - offset, "Boiler Return");
        first = false;
    }
    
    if (currentStatus.missingWaterTemp) {
        if (!first) offset += snprintf(message + offset, sizeof(message) - offset, ", ");
        offset += snprintf(message + offset, sizeof(message) - offset, "Water Tank");
        first = false;
    }
    
    if (currentStatus.missingRoomTemp) {
        if (!first) offset += snprintf(message + offset, sizeof(message) - offset, ", ");
        offset += snprintf(message + offset, sizeof(message) - offset, "Room Temperature");
    }
    
    // Add operation context
    switch (currentStatus.currentOperationMode) {
        case OperationMode::SPACE_HEATING:
            snprintf(message + offset, sizeof(message) - offset, " (required for space heating)");
            break;
        case OperationMode::WATER_HEATING:
            snprintf(message + offset, sizeof(message) - offset, " (required for water heating)");
            break;
        case OperationMode::BOTH:
            snprintf(message + offset, sizeof(message) - offset, " (required for heating operation)");
            break;
        default:
            break;
    }
    
    return message;
}