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
#include "utils/Utils.h"  // millis()
#include "modules/control/BurnerSafetyRules.h"

static const char* TAG = "BurnerSafetyValidator";

namespace {

// Sensor readings in the form BurnerSafetyRules evaluates (limits set by the caller)
BurnerSafetyRules::Inputs makeInputs(const SharedSensorReadings& readings) {
    BurnerSafetyRules::Inputs in;
    in.boilerOutput = readings.boilerTempOutput;
    in.boilerOutputValid = readings.isBoilerTempOutputValid;
    in.boilerReturn = readings.boilerTempReturn;
    in.boilerReturnValid = readings.isBoilerTempReturnValid;
    in.waterTank = readings.waterHeaterTempTank;
    in.waterTankValid = readings.isWaterHeaterTempTankValid;
    in.systemPressure = readings.systemPressure;
    in.systemPressureValid = readings.isSystemPressureValid;
    // Round 20 Issue #7: Explicit build flag required to allow no pressure sensor
#ifdef ALLOW_NO_PRESSURE_SENSOR
    in.allowMissingPressure = true;
#endif
    // F5: the boiler/tank sensors must use the MB8ART-only lastBoilerTempUpdateTimestamp;
    // the shared lastUpdateTimestamp is kept fresh by the ANDRTF3 room sensor and
    // would mask an MB8ART loss.
    in.lastBoilerTempUpdateMs = readings.lastBoilerTempUpdateTimestamp;
    in.nowMs = millis();
    in.sensorStaleMs = ::SafetyConfig::sensorStaleMs;
    return in;
}

// Sensor validation messages (step 2)
void logSensorChecks(const BurnerSafetyRules::Inputs& in) {
    if (in.boilerOutputValid && !BurnerSafetyRules::boilerSensorInRange(in.boilerOutput)) {
        char tempBuf[16];
        formatTemp(tempBuf, sizeof(tempBuf), in.boilerOutput);
        LOG_WARN(TAG, "Boiler output temp %s out of range", tempBuf);
    }

    if (BurnerSafetyRules::sensorDataStale(in)) {
        uint32_t sensorAge = in.nowMs - in.lastBoilerTempUpdateMs;
        LOG_ERROR(TAG, "Sensor data is stale: %lu ms old (threshold: %lu ms)",
                 sensorAge, in.sensorStaleMs);
    }
}

}  // namespace

BurnerSafetyValidator::ValidationResult BurnerSafetyValidator::validateBurnerOperation(
    const SharedSensorReadings& readings,
    const SafetyConfig& config,
    bool isWaterMode) {

    // Order, comparisons and limits in BurnerSafetyRules::evaluate() (native-tested):
    // emergency stop, sensors, temperature limits, pressure, hardware interlocks,
    // thermal shock. This function only collects the inputs and logs.
    EventBits_t systemBits = xEventGroupGetBits(SRP::getSystemStateEventGroup());

    BurnerSafetyRules::Inputs in = makeInputs(readings);
    in.emergencyStopActive = (systemBits & SystemEvents::SystemState::EMERGENCY_STOP) != 0;
    in.maxBoilerTemp = config.maxBoilerTemp;
    in.maxWaterTemp = config.maxWaterTemp;
    in.minRequiredSensors = config.minRequiredSensors;

    const BurnerSafetyRules::Check check =
        BurnerSafetyRules::evaluate(in, isWaterMode, &BurnerSafetyValidator::checkHardwareInterlocks);

    if (check == BurnerSafetyRules::Check::EMERGENCY_STOP) {
        LOG_ERROR(TAG, "Emergency stop is active");
        return BurnerSafetyRules::toResult(check);
    }

    logSensorChecks(in);

#ifdef ALLOW_NO_PRESSURE_SENSOR
    if (!in.systemPressureValid && BurnerSafetyRules::pressureCheckPassed(check)) {
        // Pressure sensor not valid - allow operation with warning (development/testing only)
        LOG_WARN(TAG, "Pressure sensor not available - operating in degraded mode (ALLOW_NO_PRESSURE_SENSOR enabled)");
    }
#endif

    switch (check) {
        case BurnerSafetyRules::Check::INSUFFICIENT_SENSORS:
            LOG_ERROR(TAG, "Insufficient sensors: %d valid, %d required",
                     BurnerSafetyRules::validSensorCount(in), config.minRequiredSensors);
            break;

        case BurnerSafetyRules::Check::BOILER_TEMP_HIGH: {
            char tempBuf[16], limitBuf[16];
            formatTemp(tempBuf, sizeof(tempBuf), readings.boilerTempOutput);
            formatTemp(limitBuf, sizeof(limitBuf), config.maxBoilerTemp);
            LOG_ERROR(TAG, "Boiler temp %s exceeds limit %s", tempBuf, limitBuf);
            break;
        }

        case BurnerSafetyRules::Check::WATER_TEMP_HIGH: {
            char tempBuf[16], limitBuf[16];
            formatTemp(tempBuf, sizeof(tempBuf), readings.waterHeaterTempTank);
            formatTemp(limitBuf, sizeof(limitBuf), config.maxWaterTemp);
            LOG_ERROR(TAG, "Water temp %s exceeds limit %s", tempBuf, limitBuf);
            break;
        }

        case BurnerSafetyRules::Check::PRESSURE_LOW: {
            using namespace SystemConstants::Safety::Pressure;
            LOG_ERROR(TAG, "System pressure %d.%02d BAR below minimum %d.%02d BAR",
                     readings.systemPressure / 100, abs(readings.systemPressure % 100),
                     MIN_OPERATING / 100, abs(MIN_OPERATING % 100));
            break;
        }

        case BurnerSafetyRules::Check::PRESSURE_HIGH: {
            using namespace SystemConstants::Safety::Pressure;
            LOG_ERROR(TAG, "System pressure %d.%02d BAR exceeds maximum %d.%02d BAR",
                     readings.systemPressure / 100, abs(readings.systemPressure % 100),
                     MAX_OPERATING / 100, abs(MAX_OPERATING % 100));
            break;
        }

        case BurnerSafetyRules::Check::PRESSURE_MISSING:
            // Production: No pressure sensor = block burner operation
            LOG_ERROR(TAG, "Pressure sensor not available - burner operation blocked (production safety)");
            break;

        case BurnerSafetyRules::Check::HARDWARE_INTERLOCK_OPEN:
            LOG_ERROR(TAG, "Hardware interlock is open");
            break;

        case BurnerSafetyRules::Check::THERMAL_SHOCK: {
            // Cold return water hitting a hot boiler causes thermal stress
            Temperature_t differential = tempSub(readings.boilerTempOutput, readings.boilerTempReturn);
            char outBuf[16], retBuf[16], diffBuf[16];
            formatTemp(outBuf, sizeof(outBuf), readings.boilerTempOutput);
            formatTemp(retBuf, sizeof(retBuf), readings.boilerTempReturn);
            char limitBuf[16];
            formatTemp(diffBuf, sizeof(diffBuf), differential);
            formatTemp(limitBuf, sizeof(limitBuf), SystemConstants::Safety::ReturnPreheat::MAX_DIFFERENTIAL);
            LOG_WARN(TAG, "Thermal shock risk: output=%s return=%s diff=%s (max %s°C)",
                     outBuf, retBuf, diffBuf, limitBuf);
            break;
        }

        case BurnerSafetyRules::Check::PASSED:
            // Pump verification REMOVED (Round 18) - by design the burner never checks or
            // commands pumps. BurnerSystemController switches burner relays only; pumps follow
            // the HEATING_ON/WATER_ON mode bits via PumpControlModule (ReturnPreheater cycles
            // the heating pump). The burner instead requires an active mode + request
            // (BurnerSafetyChecks::hasActiveModeDemand). Physical pump failure is detected via
            // temperature sensors (no heat transfer = no temp change).
            LOG_DEBUG(TAG, "All safety validations passed");
            break;

        case BurnerSafetyRules::Check::EMERGENCY_STOP:
        default:
            break;
    }

    return BurnerSafetyRules::toResult(check);
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
        LOG_DEBUG(TAG, "STUB: Hardware interlocks not implemented - assuming safe (fail-open)");
    }

    return true;  // Always returns true - no hardware interlocks wired
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