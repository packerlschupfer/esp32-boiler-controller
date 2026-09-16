#ifndef BURNER_SAFETY_VALIDATOR_H
#define BURNER_SAFETY_VALIDATOR_H

#include <cstdint>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "shared/SharedSensorReadings.h"
#include "shared/Temperature.h"
#include "config/SystemConstants.h"
#include "modules/control/BurnerSafetyRules.h"

/**
 * @brief Comprehensive safety validation before burner operations
 * 
 * This class implements multiple layers of safety checks that must
 * pass before any burner operation is allowed. It provides defense
 * in depth against sensor failures, software bugs, and hardware issues.
 */
class BurnerSafetyValidator {
public:
    // Safety validation results (defined with the check rules in BurnerSafetyRules.h)
    using ValidationResult = BurnerSafetyRules::ValidationResult;

    // Configuration for safety limits
    struct SafetyConfig {
        // Temperature limits
        Temperature_t maxBoilerTemp;
        Temperature_t maxWaterTemp;

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
            : maxBoilerTemp(SystemConstants::Temperature::MAX_BOILER_TEMP_C)  // 110.0°C, as burner start check and SafetyInterlocks
            , maxWaterTemp(tempFromWhole(65))      // 65.0°C
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
     * @param isWaterMode true if in water heating mode (checks water tank temp)
     * @return Validation result with specific failure reason
     */
    static ValidationResult validateBurnerOperation(
        const SharedSensorReadings& readings,
        const SafetyConfig& config,
        bool isWaterMode
    );

    /**
     * @brief Check if specific pump is operating correctly
     * @param pumpId Pump identifier (heating or water)
     * @param requireFlow Whether to check for actual flow
     * @return true if pump is operating correctly
     */
    static bool validatePumpOperation(uint8_t pumpId, bool requireFlow = true);

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
    static constexpr TickType_t MUTEX_TIMEOUT = pdMS_TO_TICKS(100);

};

#endif // BURNER_SAFETY_VALIDATOR_H