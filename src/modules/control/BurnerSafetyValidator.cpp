// src/modules/control/BurnerSafetyValidator.cpp
#include "modules/control/BurnerSafetyValidator.h"
#include "core/SystemResourceProvider.h"
#include "config/SystemConstants.h"
#include "config/SafetyConfig.h"
#include "shared/SharedRelayReadings.h"
#include "shared/Temperature.h"  // For temperature conversions
#include "shared/Pressure.h"  // For pressure types
#include "events/SystemEventsGenerated.h"
#include "utils/ErrorHandler.h"
#include "utils/MutexRetryHelper.h"
#include "modules/tasks/RelayControlTask.h"
#include "LoggingMacros.h"
#include <TaskManager.h>  // Round 14 Issue #8: For watchdog feeding
#include <algorithm>
#include <cmath>
#include <atomic>  // Round 14 Issue #2, #3
#include "utils/Utils.h"  // H1: For consistent elapsedMs() usage

static const char* TAG = "BurnerSafetyValidator";

// Static member definitions
SemaphoreHandle_t BurnerSafetyValidator::stateMutex_ = nullptr;
uint32_t BurnerSafetyValidator::lastBurnerStartTime = 0;
uint32_t BurnerSafetyValidator::totalRuntimeToday = 0;
uint32_t BurnerSafetyValidator::lastDayReset = 0;

void BurnerSafetyValidator::initMutex() {
    if (stateMutex_ == nullptr) {
        stateMutex_ = xSemaphoreCreateMutex();
    }
}

BurnerSafetyValidator::ValidationResult BurnerSafetyValidator::validateBurnerOperation(
    const SharedSensorReadings& readings,
    const SafetyConfig& config,
    bool isWaterMode) {
    
    // 1. Check emergency stop first
    EventBits_t systemBits = xEventGroupGetBits(SRP::getSystemStateEventGroup());
    if (systemBits & SystemEvents::SystemState::EMERGENCY_STOP) {
        LOG_ERROR(TAG, "Emergency stop is active");
        return ValidationResult::EMERGENCY_STOP_ACTIVE;
    }
    
    // 2. Validate sufficient sensors are working
    uint8_t validSensors = validateTemperatureSensors(readings, config);
    if (validSensors < config.minRequiredSensors) {
        LOG_ERROR(TAG, "Insufficient sensors: %d valid, %d required", 
                 validSensors, config.minRequiredSensors);
        return ValidationResult::INSUFFICIENT_SENSORS;
    }
    
    // 3. Check temperature limits (>= ensures limit itself triggers protection)
    if (readings.isBoilerTempOutputValid &&
        readings.boilerTempOutput >= config.maxBoilerTemp) {
        char tempBuf[16], limitBuf[16];
        formatTemp(tempBuf, sizeof(tempBuf), readings.boilerTempOutput);
        formatTemp(limitBuf, sizeof(limitBuf), config.maxBoilerTemp);
        LOG_ERROR(TAG, "Boiler temp %s exceeds limit %s", tempBuf, limitBuf);
        return ValidationResult::TEMPERATURE_EXCEEDED;
    }
    
    // Only check water tank temp limit during water heating mode
    // During space heating, the water tank temperature is irrelevant
    if (isWaterMode &&
        readings.isWaterHeaterTempTankValid &&
        readings.waterHeaterTempTank >= config.maxWaterTemp) {
        char tempBuf[16], limitBuf[16];
        formatTemp(tempBuf, sizeof(tempBuf), readings.waterHeaterTempTank);
        formatTemp(limitBuf, sizeof(limitBuf), config.maxWaterTemp);
        LOG_ERROR(TAG, "Water temp %s exceeds limit %s", tempBuf, limitBuf);
        return ValidationResult::TEMPERATURE_EXCEEDED;
    }

    // 4. Check runtime limits (protected by mutex)
    checkDailyReset();  // checkDailyReset has its own mutex protection
    {
        uint32_t currentRuntime = 0;
        uint32_t dailyRuntime = 0;
        uint32_t startTime = 0;

        // RAII guard for state access
        {
            initMutex();
            auto guard = MutexRetryHelper::acquireGuard(stateMutex_, "checkRuntime");
            if (guard) {
                uint32_t now = millis();
                startTime = lastBurnerStartTime;
                dailyRuntime = totalRuntimeToday;
                if (startTime > 0) {
                    currentRuntime = now - startTime;
                }
            }
        }  // Guard auto-releases

        if (startTime > 0 && currentRuntime > config.maxContinuousRuntimeMs) {
            LOG_ERROR(TAG, "Continuous runtime %lu ms exceeds limit %lu ms",
                     currentRuntime, config.maxContinuousRuntimeMs);
            return ValidationResult::RUNTIME_EXCEEDED;
        }

        if (dailyRuntime > config.maxDailyRuntimeMs) {
            LOG_ERROR(TAG, "Daily runtime %lu ms exceeds limit %lu ms",
                     dailyRuntime, config.maxDailyRuntimeMs);
            return ValidationResult::RUNTIME_EXCEEDED;
        }
    }
    
    // 5. Check system pressure (critical for safety)
    if (readings.isSystemPressureValid) {
        using namespace SystemConstants::Safety::Pressure;

        if (readings.systemPressure < MIN_OPERATING) {
            LOG_ERROR(TAG, "System pressure %d.%02d BAR below minimum %d.%02d BAR",
                     readings.systemPressure / 100, abs(readings.systemPressure % 100),
                     MIN_OPERATING / 100, abs(MIN_OPERATING % 100));
            return ValidationResult::PRESSURE_EXCEEDED;  // Also covers low pressure
        }

        if (readings.systemPressure > MAX_OPERATING) {
            LOG_ERROR(TAG, "System pressure %d.%02d BAR exceeds maximum %d.%02d BAR",
                     readings.systemPressure / 100, abs(readings.systemPressure % 100),
                     MAX_OPERATING / 100, abs(MAX_OPERATING % 100));
            return ValidationResult::PRESSURE_EXCEEDED;
        }
    } else {
        // Round 20 Issue #7: Explicit build flag required to allow no pressure sensor
#ifdef ALLOW_NO_PRESSURE_SENSOR
        // Pressure sensor not valid - allow operation with warning (development/testing only)
        LOG_WARN(TAG, "Pressure sensor not available - operating in degraded mode (ALLOW_NO_PRESSURE_SENSOR enabled)");
#else
        // Production: No pressure sensor = block burner operation
        LOG_ERROR(TAG, "Pressure sensor not available - burner operation blocked (production safety)");
        return ValidationResult::SENSOR_FAILURE;
#endif
    }
    
    // 6. Check hardware interlocks
    if (!checkHardwareInterlocks()) {
        LOG_ERROR(TAG, "Hardware interlock is open");
        return ValidationResult::HARDWARE_INTERLOCK_OPEN;
    }

    // 7. Check thermal shock risk (boiler output vs return temperature differential)
    // If differential is too high, cold return water hitting hot boiler causes thermal stress
    if (readings.isBoilerTempOutputValid && readings.isBoilerTempReturnValid) {
        Temperature_t differential = tempSub(readings.boilerTempOutput, readings.boilerTempReturn);
        if (differential > SystemConstants::Safety::ReturnPreheat::MAX_DIFFERENTIAL) {
            char outBuf[16], retBuf[16], diffBuf[16];
            formatTemp(outBuf, sizeof(outBuf), readings.boilerTempOutput);
            formatTemp(retBuf, sizeof(retBuf), readings.boilerTempReturn);
            formatTemp(diffBuf, sizeof(diffBuf), differential);
            LOG_WARN(TAG, "Thermal shock risk: output=%s return=%s diff=%s (max 30.0°C)",
                     outBuf, retBuf, diffBuf);
            return ValidationResult::THERMAL_SHOCK_RISK;
        }
    }

    // 8. Pump verification REMOVED (Round 18)
    // Pumps are now started atomically with burner via BurnerSystemController batch command.
    // Relay command verification (setMultipleRelayStatesVerified) confirms command succeeded.
    // Physical pump failure is detected via temperature sensors (no heat transfer = no temp change).

    // All checks passed
    LOG_DEBUG(TAG, "All safety validations passed");
    return ValidationResult::SAFE_TO_OPERATE;
}

bool BurnerSafetyValidator::validatePumpOperation(uint8_t pumpId, bool requireFlow) {
    // Check relay state first
    bool pumpRelayOn = false;

    {
        auto guard = MutexRetryHelper::acquireGuard(
            SRP::getRelayReadingsMutex(),
            "RelayReadings-PumpCheck"
        );
        if (guard) {
            if (pumpId == 1) {  // Heating pump
                pumpRelayOn = SRP::getRelayReadings().relayHeatingPump;
            } else if (pumpId == 2) {  // Water pump
                pumpRelayOn = SRP::getRelayReadings().relayWaterPump;
            }
        }
    }

    if (!pumpRelayOn) {
        LOG_WARN(TAG, "Pump %d relay is OFF", pumpId);
        return false;
    }

    if (requireFlow) {
        // NOTE: Fail-open design - when flow sensor unavailable, allow operation
        // to prevent complete system lockout. Real flow sensor integration is TODO.
        // Pump relay status already verified above - if relay is on, assume flow present.
        LOG_WARN(TAG, "Flow sensor not implemented - assuming flow present (fail-open)");
    }

    return pumpRelayOn;
}

uint8_t BurnerSafetyValidator::validateTemperatureSensors(
    const SharedSensorReadings& readings,
    const SafetyConfig& config) {

    uint8_t validCount = 0;
    
    // Check each sensor for validity and freshness using SystemConstants ranges
    if (readings.isBoilerTempOutputValid) {
        // Additional range check using sensor-specific limits
        if (readings.boilerTempOutput >= SystemConstants::Temperature::SensorRange::BOILER_SENSOR_MIN &&
            readings.boilerTempOutput <= SystemConstants::Temperature::SensorRange::BOILER_SENSOR_MAX) {
            validCount++;
        } else {
            char tempBuf[16];
            formatTemp(tempBuf, sizeof(tempBuf), readings.boilerTempOutput);
            LOG_WARN(TAG, "Boiler output temp %s out of range", tempBuf);
        }
    }

    if (readings.isBoilerTempReturnValid) {
        if (readings.boilerTempReturn >= SystemConstants::Temperature::SensorRange::BOILER_SENSOR_MIN &&
            readings.boilerTempReturn <= SystemConstants::Temperature::SensorRange::BOILER_SENSOR_MAX) {
            validCount++;
        }
    }

    if (readings.isWaterHeaterTempTankValid) {
        if (readings.waterHeaterTempTank >= SystemConstants::Temperature::SensorRange::WATER_TANK_SENSOR_MIN &&
            readings.waterHeaterTempTank <= SystemConstants::Temperature::SensorRange::WATER_TANK_SENSOR_MAX) {
            validCount++;
        }
    }
    
    // Check sensor data freshness using configurable timeout
    // H1: Use Utils::elapsedMs() for safe elapsed time (handles millis() wraparound)
    if (readings.lastUpdateTimestamp > 0) {
        uint32_t sensorAge = Utils::elapsedMs(readings.lastUpdateTimestamp);
        if (sensorAge > ::SafetyConfig::sensorStaleMs) {
            LOG_ERROR(TAG, "Sensor data is stale: %lu ms old (threshold: %lu ms)",
                     sensorAge, ::SafetyConfig::sensorStaleMs);
            return 0;  // All sensors considered invalid if data is stale
        }
    }
    
    return validCount;
}


bool BurnerSafetyValidator::checkHardwareInterlocks() {
    // STUB: Hardware interlocks not wired to GPIO in current hardware revision.
    //
    // When implemented, would read GPIO pins connected to:
    // - Pressure switches (high/low pressure cutoffs)
    // - Temperature limit switches (thermal fuses)
    // - Manual safety switches (emergency stop button)
    // - Gas valve feedback (valve position confirmation)
    //
    // FAIL-OPEN DESIGN: Returns true to allow operation without hardware interlocks.
    // This is intentional - the system relies on software safety checks.
    // Future hardware revision should wire these GPIOs and implement actual checking.

    // Round 14 Issue #2: Use atomic for thread-safe one-time log
    static std::atomic<bool> warningLogged{false};
    if (!warningLogged.exchange(true, std::memory_order_relaxed)) {
        LOG_WARN(TAG, "STUB: Hardware interlocks not implemented - assuming safe (fail-open)");
    }

    return true;  // Always returns true - no hardware interlocks wired
}

void BurnerSafetyValidator::checkDailyReset() {
    // RAII guard for state access
    initMutex();
    auto guard = MutexRetryHelper::acquireGuard(stateMutex_, "checkDailyReset");
    if (!guard) return;

    // H1: Use Utils::elapsedMs() for safe elapsed time (handles millis() wraparound)
    if (Utils::elapsedMs(lastDayReset) > SystemConstants::Timing::MS_PER_DAY) {
        totalRuntimeToday = 0;
        lastDayReset = millis();
        LOG_INFO(TAG, "Daily runtime counter reset");
    }
    // Guard auto-releases
}

const char* BurnerSafetyValidator::getValidationErrorMessage(ValidationResult result) {
    switch (result) {
        case ValidationResult::SAFE_TO_OPERATE:
            return "Safe to operate";
        case ValidationResult::SENSOR_FAILURE:
            return "Temperature sensor failure";
        case ValidationResult::TEMPERATURE_EXCEEDED:
            return "Temperature limit exceeded";
        case ValidationResult::PUMP_FAILURE:
            return "Pump not operating";
        case ValidationResult::WATER_FLOW_FAILURE:
            return "No water flow detected";
        case ValidationResult::PRESSURE_EXCEEDED:
            return "Pressure limit exceeded";
        case ValidationResult::FLAME_DETECTION_FAILURE:
            return "No flame detected";
        case ValidationResult::RUNTIME_EXCEEDED:
            return "Runtime limit exceeded";
        case ValidationResult::EMERGENCY_STOP_ACTIVE:
            return "Emergency stop is active";
        case ValidationResult::INSUFFICIENT_SENSORS:
            return "Insufficient working sensors";
        case ValidationResult::HARDWARE_INTERLOCK_OPEN:
            return "Hardware safety interlock open";
        case ValidationResult::THERMAL_SHOCK_RISK:
            return "Thermal shock risk - return too cold";
        default:
            return "Unknown validation error";
    }
}

void BurnerSafetyValidator::logSafetyEvent(ValidationResult result, const char* details) {
    const char* message = getValidationErrorMessage(result);
    
    if (result == ValidationResult::SAFE_TO_OPERATE) {
        LOG_INFO(TAG, "Safety validation passed: %s", message);
    } else {
        LOG_ERROR(TAG, "Safety validation failed: %s%s%s", 
                 message,
                 details ? " - " : "",
                 details ? details : "");
        
        // Log to error handler for persistence
        ErrorHandler::logError(TAG, SystemError::RELAY_SAFETY_INTERLOCK, message);
        
        // Set error bit
        xEventGroupSetBits(SRP::getErrorNotificationEventGroup(), SystemEvents::Error::SAFETY);
    }
}