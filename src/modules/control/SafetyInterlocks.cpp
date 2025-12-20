// src/modules/control/SafetyInterlocks.cpp
#include "modules/control/SafetyInterlocks.h"
#include "core/SystemResourceProvider.h"
#include "shared/SharedSensorReadings.h"
#include "shared/SharedRelayReadings.h"
#include "shared/Temperature.h"
#include "shared/Pressure.h"
#include "events/SystemEventsGenerated.h"
#include "modules/tasks/RelayControlTask.h"
#include "utils/ErrorHandler.h"
#include "utils/MutexRetryHelper.h"
#include "utils/Utils.h"
#include "monitoring/HealthMonitor.h"
#include "modules/control/CentralizedFailsafe.h"
#include "core/StateManager.h"
#include "config/SystemConstants.h"
#include "config/SafetyConfig.h"  // M3: For standardized sensorStaleMs
#include "LoggingMacros.h"
#include <TaskManager.h>  // Round 14 Issue #9: For watchdog feeding
#include <cstring>
#include <algorithm>
#include <atomic>  // Round 14 Issue #3

static const char* TAG = "SafetyInterlocks";

// Static member definitions
SafetyInterlocks::InterlockStatus SafetyInterlocks::lastStatus;
uint32_t SafetyInterlocks::lastFullCheckTime = 0;

// Round 14 Issue #3: Use atomic counters for thread-safe mutex failure tracking
// After 3 consecutive failures, trigger failsafe instead of assuming safe
static constexpr uint8_t MAX_CONSECUTIVE_MUTEX_FAILURES = 3;
static std::atomic<uint8_t> tempLimitsMutexFailures{0};
static std::atomic<uint8_t> thermalShockMutexFailures{0};

/**
 * H4: MUTEX FAIL-SAFE BEHAVIOR DOCUMENTATION
 *
 * Safety interlocks use a "circuit breaker" pattern for mutex acquisition:
 *
 * 1. FIRST 1-2 FAILURES: Return true (assume safe)
 *    - Rationale: Transient mutex contention is normal in FreeRTOS
 *    - Brief blocking shouldn't halt burner operation
 *    - Actual safety violations are rare during transient mutex waits
 *
 * 2. THIRD CONSECUTIVE FAILURE: Trigger failsafe and return false (block operation)
 *    - Rationale: 3 consecutive failures (~300ms total) indicates serious problem
 *    - Possible causes: deadlock, crashed task holding mutex, priority inversion
 *    - Fail-safe: better to shut down burner than operate blind
 *
 * 3. ON SUCCESS: Reset failure counter to 0
 *    - Allows system to recover from transient issues
 *
 * TRADE-OFF ANALYSIS:
 * - Pro: High availability for transient contention (common case)
 * - Pro: Still catches persistent failures (deadlock, crash)
 * - Con: 2 checks (~200ms window) where safety status is assumed
 * - Mitigation: Physical safety devices (thermal fuse, pressure relief) provide backup
 *
 * This pattern is acceptable because:
 * - SafetyInterlocks runs at 100ms intervals
 * - 2 missed checks = 200ms of assumed-safe operation
 * - Boiler thermal mass means 200ms cannot cause damage
 * - Hardware safety devices provide ultimate protection
 */

const char* SafetyInterlocks::InterlockStatus::getFailureReason() const {
    // Round 14 Issue #2: Static buffer for failure reason string (ESP32 optimization)
    // THREAD-SAFETY: Static buffer is safe because:
    // 1. Called only from BurnerControl task (single-threaded access pattern)
    // 2. Used immediately for logging, never stored or shared
    // 3. Alternative (stack-local 192B) would risk stack overflow (BurnerControl has ~1.2KB free)
    // 4. Uses abundant global RAM (282KB available) vs. scarce task stack
    // See: docs/MEMORY_OPTIMIZATION.md for complete rationale
    static char buffer[192];
    char* ptr = buffer;
    char* end = buffer + sizeof(buffer) - 1;

    // Build failure string
    // Round 19 Issue #3: pumpRunning check REMOVED (field no longer exists)
    if (!temperatureValid && ptr < end) ptr += snprintf(ptr, end - ptr, "Temp sensors invalid; ");
    if (!temperatureInRange && ptr < end) ptr += snprintf(ptr, end - ptr, "Temp out of range; ");
    if (!noEmergencyStop && ptr < end) ptr += snprintf(ptr, end - ptr, "Emergency stop; ");
    if (!communicationOk && ptr < end) ptr += snprintf(ptr, end - ptr, "Comm failure; ");
    if (!waterFlowDetected && ptr < end) ptr += snprintf(ptr, end - ptr, "No water flow; ");
    if (!noSystemErrors && ptr < end) ptr += snprintf(ptr, end - ptr, "System errors; ");
    if (!minimumSensorsValid && ptr < end) ptr += snprintf(ptr, end - ptr, "Insufficient sensors; ");
    if (!pressureInRange && ptr < end) ptr += snprintf(ptr, end - ptr, "Pressure out of range; ");

    if (ptr == buffer) {
        return "All interlocks passed";
    }

    // Remove trailing "; "
    if (ptr > buffer + 2) {
        ptr[-2] = '\0';
    }

    return buffer;
}

// Note: verifyPumpOperation() removed - was dead code (never called)
// Pump verification is handled by BurnerSafetyValidator::validatePumpOperation()

bool SafetyInterlocks::verifyWaterFlow(uint32_t timeoutMs) {
    // WORKAROUND: No physical flow sensor installed.
    // Uses temperature differential between output/return as proxy for water flow.
    // When burner is running, flow causes temperature difference between pipes.
    // This is NOT as reliable as a real flow sensor but provides basic protection.
    
    uint32_t startTime = millis();
    Temperature_t initialOutput = 0;
    Temperature_t initialReturn = 0;
    bool gotInitial = false;
    
    // Get initial temperatures
    {
        auto guard = MutexRetryHelper::acquireGuard(
            SRP::getSensorReadingsMutex(),
            "SensorReadings-FlowCheck"
        );
        if (guard) {
            if (SRP::getSensorReadings().isBoilerTempOutputValid &&
                SRP::getSensorReadings().isBoilerTempReturnValid) {
                initialOutput = SRP::getSensorReadings().boilerTempOutput;
                initialReturn = SRP::getSensorReadings().boilerTempReturn;
                gotInitial = true;
            }
        }
    }
    
    if (!gotInitial) {
        // NOTE: Fail-open design - when sensors unavailable, allow operation
        // to prevent complete system lockout. Real flow sensor integration is TODO.
        LOG_WARN(TAG, "Cannot verify water flow - temperature sensors invalid, assuming OK (fail-open)");
        return true;
    }
    
    // Wait and check for temperature change indicating flow
    while (Utils::elapsedMs(startTime) < timeoutMs) {
        // Round 14 Issue #9: Feed watchdog if called from watched task
        (void)SRP::getTaskManager().feedWatchdog();

        vTaskDelay(pdMS_TO_TICKS(SystemConstants::Timing::FLOW_CHECK_WAIT_MS));

        Temperature_t currentOutput = 0;
        Temperature_t currentReturn = 0;
        bool gotCurrent = false;

        {
            auto guard = MutexRetryHelper::acquireGuard(
                SRP::getSensorReadingsMutex(),
                "SensorReadings-FlowMonitor"
            );
            if (guard) {
                currentOutput = SRP::getSensorReadings().boilerTempOutput;
                currentReturn = SRP::getSensorReadings().boilerTempReturn;
                gotCurrent = true;
            }
        }

        if (gotCurrent) {
            // If temperatures are changing, we likely have flow
            Temperature_t outputChange = tempAbs(tempSub(currentOutput, initialOutput));
            Temperature_t returnChange = tempAbs(tempSub(currentReturn, initialReturn));

            if (outputChange > tempFromFloat(0.5f) || returnChange > tempFromFloat(0.5f)) {
                char outBuf[16], retBuf[16];
                formatTemp(outBuf, sizeof(outBuf), outputChange);
                formatTemp(retBuf, sizeof(retBuf), returnChange);
                LOG_DEBUG(TAG, "Water flow detected - temp changes: output=%s, return=%s",
                         outBuf, retBuf);
                return true;
            }
        }
    }
    
    LOG_WARN(TAG, "No water flow detected after %ld ms", timeoutMs);
    return false;
}

bool SafetyInterlocks::verifyTemperatureSensors(uint8_t minRequiredSensors) {
    uint8_t validSensors = 0;

    auto guard = MutexRetryHelper::acquireGuard(
        SRP::getSensorReadingsMutex(),
        "SensorReadings-Safety"
    );
    if (guard) {
        const auto& readings = SRP::getSensorReadings();

        // Count valid sensors
        if (readings.isBoilerTempOutputValid) validSensors++;
        if (readings.isBoilerTempReturnValid) validSensors++;
        if (readings.isWaterHeaterTempTankValid) validSensors++;
        if (readings.isInsideTempValid) validSensors++;

        // Check sensor update timestamps
        // Check if sensor data has ever been received
        // M3: Use configurable SafetyConfig::sensorStaleMs instead of hardcoded SENSOR_TIMEOUT_MS
        if (readings.lastUpdateTimestamp == 0) {
            LOG_WARN(TAG, "No sensor data received yet");
            validSensors = 0; // No data received
        } else if (Utils::elapsedMs(readings.lastUpdateTimestamp) > SafetyConfig::sensorStaleMs) {
            LOG_WARN(TAG, "Sensor data is stale - last update %lu ms ago (threshold: %lu ms)",
                     Utils::elapsedMs(readings.lastUpdateTimestamp), SafetyConfig::sensorStaleMs);
            validSensors = 0; // Consider all invalid if data is too old
        }
    }

    bool result = (validSensors >= minRequiredSensors);
    if (!result) {
        LOG_ERROR(TAG, "Insufficient valid sensors: %d/%d required",
                 validSensors, minRequiredSensors);
    }

    return result;
}

bool SafetyInterlocks::checkTemperatureLimits(Temperature_t maxAllowedTemp) {
    bool withinLimits = true;

    auto guard = MutexRetryHelper::acquireGuard(
        SRP::getSensorReadingsMutex(),
        "SensorReadings-TempLimits"
    );
    if (!guard) {
        // Mutex acquisition failed - track consecutive failures
        tempLimitsMutexFailures++;
        if (tempLimitsMutexFailures >= MAX_CONSECUTIVE_MUTEX_FAILURES) {
            LOG_ERROR(TAG, "FAIL-SAFE: %d consecutive mutex failures in checkTemperatureLimits - blocking burner",
                     tempLimitsMutexFailures.load());
            CentralizedFailsafe::triggerFailsafe(
                CentralizedFailsafe::FailsafeLevel::DEGRADED,
                SystemError::MUTEX_TIMEOUT,
                "Repeated mutex timeout in temperature limit check"
            );
            return false;  // Fail-safe: block operation when cannot verify
        }
        LOG_WARN(TAG, "Mutex timeout in checkTemperatureLimits (%d/%d) - assuming safe (transient)",
                tempLimitsMutexFailures.load(), MAX_CONSECUTIVE_MUTEX_FAILURES);
        return true;
    }
    // Mutex acquired successfully - reset failure counter
    tempLimitsMutexFailures = 0;
    {
        const auto& readings = SRP::getSensorReadings();

        // Check boiler output temperature
        if (readings.isBoilerTempOutputValid) {
            if (readings.boilerTempOutput >= maxAllowedTemp) {
                char tempBuf[16], limitBuf[16];
                formatTemp(tempBuf, sizeof(tempBuf), readings.boilerTempOutput);
                formatTemp(limitBuf, sizeof(limitBuf), maxAllowedTemp);
                LOG_ERROR(TAG, "Boiler output temp %s°C exceeds limit %s°C",
                         tempBuf, limitBuf);
                withinLimits = false;
            }

            // Critical temperature check
            if (readings.boilerTempOutput >= SystemConstants::Temperature::CRITICAL_BOILER_TEMP_C) {
                char tempBuf[16];
                formatTemp(tempBuf, sizeof(tempBuf), readings.boilerTempOutput);
                LOG_ERROR(TAG, "CRITICAL: Boiler temp %s°C exceeds critical limit!", tempBuf);
                triggerEmergencyShutdown("Critical temperature exceeded");
                withinLimits = false;
            }
        }

        // Check boiler return temperature
        if (readings.isBoilerTempReturnValid) {
            if (readings.boilerTempReturn >= maxAllowedTemp) {
                char tempBuf[16], limitBuf[16];
                formatTemp(tempBuf, sizeof(tempBuf), readings.boilerTempReturn);
                formatTemp(limitBuf, sizeof(limitBuf), maxAllowedTemp);
                LOG_ERROR(TAG, "Boiler return temp %s°C exceeds limit %s°C",
                         tempBuf, limitBuf);
                withinLimits = false;
            }
        }

        // Water heater temperature check removed - not relevant during space heating mode.
        // Mode-aware checking is done in BurnerSafetyValidator::validateBurnerOperation()
        // which blocks burner only when in water heating mode.
    }

    return withinLimits;
}

bool SafetyInterlocks::checkThermalShock(Temperature_t maxDifferential) {
    bool safe = true;

    auto guard = MutexRetryHelper::acquireGuard(
        SRP::getSensorReadingsMutex(),
        "SensorReadings-ThermalShock"
    );
    if (!guard) {
        // Mutex acquisition failed - track consecutive failures
        thermalShockMutexFailures++;
        if (thermalShockMutexFailures >= MAX_CONSECUTIVE_MUTEX_FAILURES) {
            LOG_ERROR(TAG, "FAIL-SAFE: %d consecutive mutex failures in checkThermalShock - blocking burner",
                     thermalShockMutexFailures.load());
            CentralizedFailsafe::triggerFailsafe(
                CentralizedFailsafe::FailsafeLevel::DEGRADED,
                SystemError::MUTEX_TIMEOUT,
                "Repeated mutex timeout in thermal shock check"
            );
            return false;  // Fail-safe: block operation when cannot verify
        }
        LOG_WARN(TAG, "Mutex timeout in checkThermalShock (%d/%d) - assuming safe (transient)",
                thermalShockMutexFailures.load(), MAX_CONSECUTIVE_MUTEX_FAILURES);
        return true;
    }
    // Mutex acquired successfully - reset failure counter
    thermalShockMutexFailures = 0;
    {
        const auto& readings = SRP::getSensorReadings();

        if (readings.isBoilerTempOutputValid && readings.isBoilerTempReturnValid) {
            // Calculate temperature difference using Temperature_t
            Temperature_t tempDiff = tempSub(readings.boilerTempOutput, readings.boilerTempReturn);

            if (tempDiff > maxDifferential) {
                char diffBuf[16], limitBuf[16];
                formatTemp(diffBuf, sizeof(diffBuf), tempDiff);
                formatTemp(limitBuf, sizeof(limitBuf), maxDifferential);
                LOG_ERROR(TAG, "Thermal shock risk: differential %s°C exceeds limit %s°C",
                         diffBuf, limitBuf);
                safe = false;
            } else if (tempDiff > ((maxDifferential * SystemConstants::Safety::THERMAL_SHOCK_WARNING_NUM) /
                                   SystemConstants::Safety::THERMAL_SHOCK_WARNING_DEN)) {
                char diffBuf[16], limitBuf[16];
                formatTemp(diffBuf, sizeof(diffBuf), tempDiff);
                formatTemp(limitBuf, sizeof(limitBuf), maxDifferential);
                LOG_WARN(TAG, "High temperature differential: %s°C (limit: %s°C)",
                         diffBuf, limitBuf);
            }
        }
    }

    return safe;
}

bool SafetyInterlocks::checkEmergencyStop() {
    EventBits_t systemBits = xEventGroupGetBits(SRP::getSystemStateEventGroup());
    bool emergencyStop = (systemBits & SystemEvents::SystemState::EMERGENCY_STOP) != 0;
    
    if (emergencyStop) {
        LOG_ERROR(TAG, "Emergency stop is active!");
    }
    
    return !emergencyStop;
}

bool SafetyInterlocks::checkSystemErrors() {
    // Check error notification bits
    EventBits_t errorBits = xEventGroupGetBits(SRP::getErrorNotificationEventGroup());
    
    // Critical errors that should block burner operation
    const EventBits_t CRITICAL_ERRORS = 
        SystemEvents::Error::SENSOR_FAILURE | 
        SystemEvents::Error::MODBUS | 
        SystemEvents::Error::RELAY;
    
    bool hasErrors = (errorBits & CRITICAL_ERRORS) != 0;
    
    if (hasErrors) {
        LOG_ERROR(TAG, "Critical system errors detected: 0x%06X", errorBits);
    }
    
    return !hasErrors;
}

bool SafetyInterlocks::checkCommunicationStatus() {
    // Check Modbus communication
    EventBits_t sensorBits = xEventGroupGetBits(SRP::getSensorEventGroup());
    bool sensorCommOk = (sensorBits & SystemEvents::SensorUpdate::DATA_AVAILABLE) != 0;
    
    // Check relay communication
    EventBits_t relayBits = xEventGroupGetBits(SRP::getRelayStatusEventGroup());
    bool relayCommOk = (relayBits & SystemEvents::RelayStatus::SYNCHRONIZED) != 0;
    
    bool allOk = sensorCommOk && relayCommOk;
    
    if (!allOk) {
        LOG_ERROR(TAG, "Communication failure - Sensors: %s, Relays: %s",
                 sensorCommOk ? "OK" : "FAIL",
                 relayCommOk ? "OK" : "FAIL");
    }
    
    return allOk;
}

SafetyInterlocks::InterlockStatus SafetyInterlocks::performFullSafetyCheck(bool isWaterMode) {
    InterlockStatus status;
    status.lastCheckTime = millis();
    
    LOG_DEBUG(TAG, "Performing full safety interlock check for %s mode (WATER_ON: %s)",
              isWaterMode ? "WATER" : "HEATING",
              (xEventGroupGetBits(SRP::getSystemStateEventGroup()) & SystemEvents::SystemState::WATER_ON) ? "SET" : "NOT SET");
    
    // 1. Check emergency stop
    status.noEmergencyStop = checkEmergencyStop();
    
    // 2. Check system errors
    status.noSystemErrors = checkSystemErrors();
    
    // 3. Verify temperature sensors
    status.minimumSensorsValid = verifyTemperatureSensors(2); // Need at least 2 sensors
    status.temperatureValid = status.minimumSensorsValid;
    
    // 4. Check temperature limits
    status.temperatureInRange = checkTemperatureLimits(SystemConstants::Temperature::MAX_BOILER_TEMP_C) &&
                                checkThermalShock(tempFromWhole(30));  // 30.0°C
    
    // 5. Check communication status
    status.communicationOk = checkCommunicationStatus();
    
    // 6. Pump verification REMOVED (Round 18/19)
    // Pumps are now started atomically with burner via BurnerSystemController batch command.
    // Relay command verification (setMultipleRelayStatesVerified) confirms command succeeded.
    // Physical pump failure is detected via temperature sensors (no heat transfer = no temp change).
    // Round 19 Issue #3: pumpRunning field completely removed from InterlockStatus struct.

    // 7. Check water flow (currently just a stub)
    status.waterFlowDetected = true; // Assume OK until flow sensor available
    
    // 8. Check system pressure
    {
        auto guard = MutexRetryHelper::acquireGuard(
            SRP::getSensorReadingsMutex(),
            "SensorReadings-Pressure"
        );
        if (guard) {
            const auto& readings = SRP::getSensorReadings();
            if (readings.isSystemPressureValid) {
                using namespace SystemConstants::Safety::Pressure;

                status.pressureInRange = (readings.systemPressure >= MIN_OPERATING &&
                                         readings.systemPressure <= MAX_OPERATING);

                if (!status.pressureInRange) {
                    LOG_WARN(TAG, "System pressure %d.%02d BAR out of range (%d.%02d - %d.%02d BAR)",
                            readings.systemPressure / 100, abs(readings.systemPressure % 100),
                            MIN_OPERATING / 100, abs(MIN_OPERATING % 100),
                            MAX_OPERATING / 100, abs(MAX_OPERATING % 100));
                }
            } else {
                // No pressure sensor or invalid reading - allow operation with warning
                status.pressureInRange = true;  // Don't block operation without sensor
                LOG_DEBUG(TAG, "Pressure sensor not available - skipping pressure check");
            }
        }
    }
    
    // Log results
    if (status.allInterlocksPassed()) {
        LOG_DEBUG(TAG, "All safety interlocks PASSED");
    } else {
        LOG_ERROR(TAG, "Safety interlocks FAILED: %s",
                 status.getFailureReason());
    }
    
    // Update last status
    lastStatus = status;
    lastFullCheckTime = status.lastCheckTime;
    
    return status;
}

bool SafetyInterlocks::continuousSafetyMonitor() {
    // Skip safety checks if boiler is disabled - we're intentionally shutting down
    // The graceful shutdown is handled by processBurnerRequest() and the state machine
    // will transition out of running states. We don't want to trigger emergency
    // shutdown just because the pump turned off before the burner state changed.
    EventBits_t systemState = xEventGroupGetBits(SRP::getSystemStateEventGroup());
    if (!(systemState & SystemEvents::SystemState::BOILER_ENABLED)) {
        return true;  // Skip safety checks during graceful shutdown
    }

    uint32_t now = millis();

    // Perform critical checks frequently (every call)
    // 1. Emergency stop check
    if (!checkEmergencyStop()) {
        LOG_ERROR(TAG, "Critical safety check failed: Emergency stop active");
        return false;
    }

    // 2. Temperature limits check
    if (!checkTemperatureLimits(SystemConstants::Temperature::CRITICAL_BOILER_TEMP_C)) {
        LOG_ERROR(TAG, "Critical safety check failed: Temperature limit exceeded");
        return false;
    }

    // 3. Sensor staleness check - CRITICAL: Must have fresh sensor data during operation
    // Uses StateManager which checks lastUpdateTimestamp (15s threshold)
    if (StateManager::isSensorStale(StateManager::SensorChannel::BOILER_OUTPUT)) {
        uint32_t age = StateManager::getSensorAge(StateManager::SensorChannel::BOILER_OUTPUT);
        LOG_ERROR(TAG, "Critical safety check failed: Sensor data stale (%lu ms)", age);
        triggerEmergencyShutdown("Sensor data stale during operation");
        return false;
    }
    
    // Perform full check periodically
    if ((now - lastFullCheckTime) >= FULL_CHECK_INTERVAL_MS) {
        // Determine current mode from system state
        // Check SYSTEM_STATE bits for actual mode, not BURNER_STATE which may not be set yet
        EventBits_t systemState = xEventGroupGetBits(SRP::getSystemStateEventGroup());
        bool isWaterMode = (systemState & SystemEvents::SystemState::WATER_ON) != 0;
        bool isHeatingMode = (systemState & SystemEvents::SystemState::HEATING_ON) != 0;

        // Skip pump interlock check if transitioning between modes (neither active)
        // This prevents false alarms when switching modes or shutting down
        if (!isWaterMode && !isHeatingMode) {
            // In transition or idle - skip full check
            lastFullCheckTime = now;
            return true;
        }

        InterlockStatus status = performFullSafetyCheck(isWaterMode);
        return status.allInterlocksPassed();
    }
    
    // Between full checks, return last status
    return lastStatus.allInterlocksPassed();
}

void SafetyInterlocks::triggerEmergencyShutdown(const char* reason) {
    LOG_ERROR(TAG, "EMERGENCY SHUTDOWN triggered: %s", reason);

    // Route through CentralizedFailsafe for coordinated emergency response
    // CentralizedFailsafe::emergencyStop() handles:
    // - Burner shutdown
    // - Pump management (keeps running for heat dissipation)
    // - Event bit management (EMERGENCY_STOP)
    // - Error logging
    // - State persistence
    CentralizedFailsafe::emergencyStop(reason);
}