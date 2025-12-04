#ifndef BURNER_SAFETY_VALIDATOR_H
#define BURNER_SAFETY_VALIDATOR_H

#include <cstdint>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "shared/SharedSensorReadings.h"
#include "shared/Temperature.h"
#include "config/SystemConstants.h"

/**
 * @brief Comprehensive safety validation before burner operations
 * 
 * This class implements multiple layers of safety checks that must
 * pass before any burner operation is allowed. It provides defense
 * in depth against sensor failures, software bugs, and hardware issues.
 */
class BurnerSafetyValidator {
public:
    // Safety validation results
    enum class ValidationResult {
        SAFE_TO_OPERATE,
        SENSOR_FAILURE,
        TEMPERATURE_EXCEEDED,
        PUMP_FAILURE,
        WATER_FLOW_FAILURE,
        PRESSURE_EXCEEDED,
        FLAME_DETECTION_FAILURE,
        THERMAL_RUNAWAY,
        RUNTIME_EXCEEDED,
        EMERGENCY_STOP_ACTIVE,
        INSUFFICIENT_SENSORS,
        RATE_OF_CHANGE_EXCEEDED,
        CROSS_VALIDATION_FAILED,
        HARDWARE_INTERLOCK_OPEN
    };

    // Configuration for safety limits
    struct SafetyConfig {
        // Temperature limits
        Temperature_t maxBoilerTemp;
        Temperature_t maxWaterTemp;
        Temperature_t maxTempRateOfChange;  // Rate in tenths of degrees per minute
        Temperature_t sensorCrossValidationTolerance;
        
        // Runtime limits
        uint32_t maxContinuousRuntimeMs;
        uint32_t maxDailyRuntimeMs;
        
        // Sensor requirements
        uint8_t minRequiredSensors;
        uint32_t sensorTimeoutMs;
        
        // Flow requirements
        uint32_t pumpStartupTimeMs;
        float minFlowRate;
        
        // Grace periods (should be minimal)
        uint32_t startupGracePeriodMs;
        
        // Constructor with defaults
        SafetyConfig() 
            : maxBoilerTemp(tempFromWhole(85))     // 85.0°C
            , maxWaterTemp(tempFromWhole(65))      // 65.0°C
            , maxTempRateOfChange(tempFromWhole(5)) // 5.0°C/min
            , sensorCrossValidationTolerance(tempFromWhole(10)) // 10.0°C
            , maxContinuousRuntimeMs(3600000)
            , maxDailyRuntimeMs(14400000)
            , minRequiredSensors(2)
            , sensorTimeoutMs(30000)
            , pumpStartupTimeMs(5000)
            , minFlowRate(0.5f)
            , startupGracePeriodMs(0) {}
    };

    /**
     * @brief Perform comprehensive safety validation
     * @param readings Current sensor readings
     * @param config Safety configuration
     * @return Validation result with specific failure reason
     */
    static ValidationResult validateBurnerOperation(
        const SharedSensorReadings& readings,
        const SafetyConfig& config
    );

    /**
     * @brief Check if specific pump is operating correctly
     * @param pumpId Pump identifier (heating or water)
     * @param requireFlow Whether to check for actual flow
     * @return true if pump is operating correctly
     */
    static bool validatePumpOperation(uint8_t pumpId, bool requireFlow = true);

    /**
     * @brief Validate temperature sensors
     * @param readings Current sensor readings
     * @param config Safety configuration
     * @return Number of valid sensors
     */
    static uint8_t validateTemperatureSensors(
        const SharedSensorReadings& readings,
        const SafetyConfig& config
    );

    // Note: detectThermalRunaway() removed - inline version in validateBurnerOperation() is used instead

    /**
     * @brief Cross-validate redundant sensors
     * @param sensor1 First sensor reading
     * @param sensor2 Second sensor reading
     * @param tolerance Maximum allowed difference
     * @return true if sensors agree within tolerance
     */
    static bool crossValidateSensors(
        Temperature_t sensor1,
        Temperature_t sensor2,
        Temperature_t tolerance
    );

    /**
     * @brief Calculate temperature rate of change
     * @param history Temperature history (newest first)
     * @param timeSpanMs Time span of history in milliseconds
     * @return Rate of change in tenths of °C/minute
     */
    static Temperature_t calculateRateOfChange(
        const std::vector<Temperature_t>& history,
        uint32_t timeSpanMs
    );

    /**
     * @brief Check hardware interlocks
     * @return true if all hardware interlocks are closed
     */
    static bool checkHardwareInterlocks();

    /**
     * @brief Get human-readable error message
     * @param result Validation result
     * @return Error description
     */
    static const char* getValidationErrorMessage(ValidationResult result);

    /**
     * @brief Record safety validation event
     * @param result Validation result
     * @param details Additional details
     */
    static void logSafetyEvent(ValidationResult result, const char* details = nullptr);

private:
    // Thread protection for static members
    static SemaphoreHandle_t stateMutex_;
    static constexpr TickType_t MUTEX_TIMEOUT = pdMS_TO_TICKS(100);
    static void initMutex();

    // Track runtime for limits
    static uint32_t lastBurnerStartTime;
    static uint32_t totalRuntimeToday;
    static uint32_t lastDayReset;

    // Temperature history for rate calculation
    static std::vector<Temperature_t> temperatureHistory;
    static std::vector<uint32_t> temperatureTimestamps;
    static const size_t MAX_HISTORY_SIZE = 60;  // 1 minute at 1Hz

    // Update temperature history
    static void updateTemperatureHistory(Temperature_t temp);

    // Reset daily runtime counter
    static void checkDailyReset();
};

#endif // BURNER_SAFETY_VALIDATOR_H