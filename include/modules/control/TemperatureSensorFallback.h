#ifndef TEMPERATURE_SENSOR_FALLBACK_H
#define TEMPERATURE_SENSOR_FALLBACK_H

#include <cstdint>
#include "shared/Temperature.h"

/**
 * @brief Temperature sensor fallback and validation system
 * 
 * Provides ultra-simple, fail-safe sensor validation with context-aware
 * requirements based on active operation mode. No partial operation allowed.
 */
class TemperatureSensorFallback {
public:
    enum class FallbackMode {
        STARTUP,          // Initial startup period - waiting for sensors
        NORMAL,           // All required sensors working
        SHUTDOWN          // Any required sensor missing - must stop
    };
    
    enum class OperationMode {
        NONE,             // No operation active
        SPACE_HEATING,    // Space heating only
        WATER_HEATING,    // Water heating only
        BOTH              // Both heating modes active
    };
    
    struct SensorStatus {
        bool boilerOutputValid = false;
        bool boilerReturnValid = false;
        bool waterTempValid = false;
        bool roomTempValid = false;
        bool outsideTempValid = false;
        
        FallbackMode currentMode = FallbackMode::STARTUP;
        OperationMode currentOperationMode = OperationMode::NONE;
        
        // Track which specific sensors are missing for clear error reporting
        bool missingBoilerOutput = false;
        bool missingBoilerReturn = false;
        bool missingWaterTemp = false;
        bool missingRoomTemp = false;
    };
    
    // Initialize the fallback system
    static void initialize();

    // Cleanup for partial init recovery
    static void cleanup();

    // Set current operation mode (called by control tasks)
    static void setOperationMode(OperationMode mode);
    
    // Update sensor status and determine fallback mode
    static FallbackMode updateSensorStatus();
    
    // Get current fallback mode
    static FallbackMode getCurrentMode() { return currentStatus.currentMode; }
    
    // Get current operation mode
    static OperationMode getOperationMode() { return currentStatus.currentOperationMode; }
    
    // Get sensor status
    static const SensorStatus& getStatus() { return currentStatus; }
    
    // Check if operation should continue
    static bool canContinueOperation();
    
    // Get safe operating parameters for current mode
    static void getSafeOperatingParams(Temperature_t& maxTemp, float& maxPower, uint32_t& runTime);
    
    // Check if all required sensors are available for current operation mode
    static bool hasRequiredSensors();
    
private:
    static SensorStatus currentStatus;
    static uint32_t initializationTime;
    // Use SENSOR_TIMEOUT_MS from SystemConstants (30 seconds)
    // Startup period must be long enough for all Modbus sensors to complete their first read
    // ANDRTF3 needs ~200-500ms per read, and may wait for Modbus bus mutex
    // Reduced from 10s to 5s - sensors typically ready within 5s, 10s was overly conservative
    static constexpr uint32_t STARTUP_PERIOD_MS = 5000; // 5 seconds startup grace period
    static constexpr Temperature_t DEFAULT_OUTSIDE_TEMP = -50; // -5.0°C in tenths

    // Hysteresis counters to prevent mode chattering on flaky sensors
    static uint8_t consecutiveValidCount;    // Count of consecutive valid sensor checks
    static uint8_t consecutiveInvalidCount;  // Count of consecutive invalid sensor checks

    // Hysteresis thresholds (different for entering vs leaving NORMAL mode)
    // NOTE: Reduced to 1 (disabled) - sensors are stable and accurate in this installation
    // Original values (Round 4): VALID=3, INVALID=2 for flaky sensor protection
    // Archived reasoning: Prevent mode oscillation from intermittent Modbus errors
    static constexpr uint8_t VALID_COUNT_TO_ENTER_NORMAL = 1;   // Was 3 - reduced for faster startup
    static constexpr uint8_t INVALID_COUNT_TO_SHUTDOWN = 1;     // Was 2 - instant shutdown on sensor fail
    
    // Validate sensor readings
    static bool validateSensorReading(Temperature_t temp, Temperature_t minValid = -500, Temperature_t maxValid = 1500);
    
    // Check sensor age
    static bool isSensorDataFresh(uint32_t lastUpdateTime, uint32_t maxAge = 30000);  // Default to SystemConstants::Timing::SENSOR_TIMEOUT_MS
    
    // Get specific error message for missing sensors
    static const char* getMissingSensorMessage();
};

#endif // TEMPERATURE_SENSOR_FALLBACK_H