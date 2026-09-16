// include/modules/control/BurnerSafetyRules.h
#ifndef BURNER_SAFETY_RULES_H
#define BURNER_SAFETY_RULES_H

#include <cstdint>
#include "shared/Temperature.h"
#include "config/SystemConstants.h"

/**
 * @brief Layer-1 burner safety checks of BurnerSafetyValidator::validateBurnerOperation()
 *        (header-only, native-testable)
 *
 * The validator reads the emergency stop bit, the sensor readings and the runtime
 * sensorStaleMs into Inputs, calls evaluate() and logs the returned Check. Order,
 * comparisons and limits are defined only here.
 */
namespace BurnerSafetyRules {

    // Result codes (BurnerSafetyValidator::ValidationResult is an alias of this enum)
    enum class ValidationResult {
        SAFE_TO_OPERATE,
        SENSOR_FAILURE,
        TEMPERATURE_EXCEEDED,
        PUMP_FAILURE,
        WATER_FLOW_FAILURE,
        PRESSURE_EXCEEDED,
        FLAME_DETECTION_FAILURE,
        EMERGENCY_STOP_ACTIVE,
        INSUFFICIENT_SENSORS,
        HARDWARE_INTERLOCK_OPEN,
        THERMAL_SHOCK_RISK       // Boiler output - return temperature differential too high
    };

    // The check that decided the result, in evaluation order (the validator logs per check)
    enum class Check : uint8_t {
        EMERGENCY_STOP,
        INSUFFICIENT_SENSORS,     // fewer valid sensors than required, or stale MB8ART data
        BOILER_TEMP_HIGH,
        WATER_TEMP_HIGH,          // water mode only
        PRESSURE_LOW,
        PRESSURE_HIGH,
        PRESSURE_MISSING,         // no valid pressure reading, not allowed by the build
        HARDWARE_INTERLOCK_OPEN,
        THERMAL_SHOCK,
        PASSED
    };

    struct Inputs {
        bool emergencyStopActive = false;

        Temperature_t boilerOutput = 0;
        bool boilerOutputValid = false;
        Temperature_t boilerReturn = 0;
        bool boilerReturnValid = false;
        Temperature_t waterTank = 0;
        bool waterTankValid = false;

        int16_t systemPressure = 0;       // Pressure_t, hundredths of BAR
        bool systemPressureValid = false;
        bool allowMissingPressure = false;  // ALLOW_NO_PRESSURE_SENSOR build

        uint32_t lastBoilerTempUpdateMs = 0;  // MB8ART-only timestamp, 0 = never updated
        uint32_t nowMs = 0;
        uint32_t sensorStaleMs = 0;

        Temperature_t maxBoilerTemp = 0;
        Temperature_t maxWaterTemp = 0;
        uint8_t minRequiredSensors = 0;
    };

    inline bool boilerSensorInRange(Temperature_t t) {
        return t >= SystemConstants::Temperature::SensorRange::BOILER_SENSOR_MIN &&
               t <= SystemConstants::Temperature::SensorRange::BOILER_SENSOR_MAX;
    }

    inline bool tankSensorInRange(Temperature_t t) {
        return t >= SystemConstants::Temperature::SensorRange::WATER_TANK_SENSOR_MIN &&
               t <= SystemConstants::Temperature::SensorRange::WATER_TANK_SENSOR_MAX;
    }

    // Valid and in-range boiler output, boiler return and water tank sensors
    inline uint8_t countInRangeSensors(const Inputs& in) {
        uint8_t count = 0;
        if (in.boilerOutputValid && boilerSensorInRange(in.boilerOutput)) count++;
        if (in.boilerReturnValid && boilerSensorInRange(in.boilerReturn)) count++;
        if (in.waterTankValid && tankSensorInRange(in.waterTank)) count++;
        return count;
    }

    // Age handles millis() wraparound; exactly sensorStaleMs old is still fresh
    inline bool sensorDataStale(const Inputs& in) {
        return in.lastBoilerTempUpdateMs > 0 &&
               static_cast<uint32_t>(in.nowMs - in.lastBoilerTempUpdateMs) > in.sensorStaleMs;
    }

    // Stale MB8ART data invalidates all sensors, so the count is 0
    inline uint8_t validSensorCount(const Inputs& in) {
        return sensorDataStale(in) ? 0 : countInRangeSensors(in);
    }

    /**
     * @param hardwareInterlocksClosed called only when all earlier checks passed (not null)
     */
    inline Check evaluate(const Inputs& in, bool isWaterMode, bool (*hardwareInterlocksClosed)()) {
        // 1. Emergency stop first
        if (in.emergencyStopActive) {
            return Check::EMERGENCY_STOP;
        }

        // 2. Sufficient sensors working
        if (validSensorCount(in) < in.minRequiredSensors) {
            return Check::INSUFFICIENT_SENSORS;
        }

        // 3. Temperature limits (>= ensures limit itself triggers protection)
        if (in.boilerOutputValid && in.boilerOutput >= in.maxBoilerTemp) {
            return Check::BOILER_TEMP_HIGH;
        }

        // Water tank limit only during water heating; irrelevant for space heating
        if (isWaterMode && in.waterTankValid && in.waterTank >= in.maxWaterTemp) {
            return Check::WATER_TEMP_HIGH;
        }

        // 4. System pressure
        if (in.systemPressureValid) {
            if (in.systemPressure < SystemConstants::Safety::Pressure::MIN_OPERATING) {
                return Check::PRESSURE_LOW;
            }
            if (in.systemPressure > SystemConstants::Safety::Pressure::MAX_OPERATING) {
                return Check::PRESSURE_HIGH;
            }
        } else if (!in.allowMissingPressure) {
            return Check::PRESSURE_MISSING;
        }

        // 5. Hardware interlocks
        if (!hardwareInterlocksClosed()) {
            return Check::HARDWARE_INTERLOCK_OPEN;
        }

        // 6. Thermal shock (boiler output vs return differential)
        if (in.boilerOutputValid && in.boilerReturnValid) {
            const Temperature_t differential = tempSub(in.boilerOutput, in.boilerReturn);
            if (differential > SystemConstants::Safety::ReturnPreheat::MAX_DIFFERENTIAL) {
                return Check::THERMAL_SHOCK;
            }
        }

        return Check::PASSED;
    }

    // Evaluation got past the pressure check (a missing reading was allowed or not needed)
    inline bool pressureCheckPassed(Check check) {
        return check == Check::HARDWARE_INTERLOCK_OPEN ||
               check == Check::THERMAL_SHOCK ||
               check == Check::PASSED;
    }

    inline ValidationResult toResult(Check check) {
        switch (check) {
            case Check::EMERGENCY_STOP:          return ValidationResult::EMERGENCY_STOP_ACTIVE;
            case Check::INSUFFICIENT_SENSORS:    return ValidationResult::INSUFFICIENT_SENSORS;
            case Check::BOILER_TEMP_HIGH:        return ValidationResult::TEMPERATURE_EXCEEDED;
            case Check::WATER_TEMP_HIGH:         return ValidationResult::TEMPERATURE_EXCEEDED;
            case Check::PRESSURE_LOW:            return ValidationResult::PRESSURE_EXCEEDED;  // also covers low pressure
            case Check::PRESSURE_HIGH:           return ValidationResult::PRESSURE_EXCEEDED;
            case Check::PRESSURE_MISSING:        return ValidationResult::SENSOR_FAILURE;
            case Check::HARDWARE_INTERLOCK_OPEN: return ValidationResult::HARDWARE_INTERLOCK_OPEN;
            case Check::THERMAL_SHOCK:           return ValidationResult::THERMAL_SHOCK_RISK;
            case Check::PASSED:                  return ValidationResult::SAFE_TO_OPERATE;
        }
        return ValidationResult::SAFE_TO_OPERATE;  // unreachable
    }

}  // namespace BurnerSafetyRules

#endif  // BURNER_SAFETY_RULES_H
