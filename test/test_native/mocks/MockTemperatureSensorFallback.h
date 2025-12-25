/**
 * @file MockTemperatureSensorFallback.h
 * @brief Mock implementation of TemperatureSensorFallback for testing
 */

#ifndef MOCK_TEMPERATURE_SENSOR_FALLBACK_H
#define MOCK_TEMPERATURE_SENSOR_FALLBACK_H

#include "MockSharedSensorReadings.h"
#include "../../include/shared/Temperature.h"

class TemperatureSensorFallback {
public:
    enum class FallbackMode {
        NORMAL,      // All sensors working
        DEGRADED,    // One sensor failed
        MINIMAL,     // Critical sensors failed
        EMERGENCY,   // Most sensors failed
        SHUTDOWN     // All sensors failed
    };
    
    struct OperatingLimits {
        Temperature_t maxTemperature;
        uint8_t powerLimit;
        uint32_t maxRuntime;
    };
    
private:
    FallbackMode currentMode_;
    int sensorFailureCount_;
    
public:
    TemperatureSensorFallback() 
        : currentMode_(FallbackMode::NORMAL),
          sensorFailureCount_(0) {}
    
    FallbackMode evaluateMode(const SharedSensorReadings& readings) {
        // Count invalid sensors using validity flags
        int invalidCount = 0;

        if (!readings.isBoilerTempOutputValid) invalidCount++;
        if (!readings.isBoilerTempReturnValid) invalidCount++;
        if (!readings.isWaterHeaterTempTankValid) invalidCount++;
        if (!readings.isInsideTempValid) invalidCount++;
        
        // Check if sensors have recovered
        if (invalidCount == 0 && sensorFailureCount_ > 0) {
            // Sensors recovered, return to normal
            sensorFailureCount_ = 0;
            currentMode_ = FallbackMode::NORMAL;
            return currentMode_;
        }
        
        // Determine mode based on failures
        if (invalidCount == 0) {
            currentMode_ = FallbackMode::NORMAL;
        } else if (invalidCount == 1) {
            currentMode_ = FallbackMode::DEGRADED;
        } else if (invalidCount == 2) {
            currentMode_ = FallbackMode::MINIMAL;
        } else if (invalidCount == 3) {
            currentMode_ = FallbackMode::EMERGENCY;
        } else {
            currentMode_ = FallbackMode::SHUTDOWN;
        }
        
        sensorFailureCount_ = invalidCount;
        return currentMode_;
    }
    
    OperatingLimits getAdjustedLimits() const {
        switch (currentMode_) {
            case FallbackMode::NORMAL:
                return {tempFromFloat(85.0f), 100, 0}; // No limit
                
            case FallbackMode::DEGRADED:
                return {tempFromFloat(75.0f), 80, 0};  // Reduced
                
            case FallbackMode::MINIMAL:
                return {tempFromFloat(60.0f), 50, 1800000}; // 30 min
                
            case FallbackMode::EMERGENCY:
                return {tempFromFloat(50.0f), 30, 300000};  // 5 min
                
            case FallbackMode::SHUTDOWN:
            default:
                return {tempFromFloat(0.0f), 0, 0};
        }
    }
};

#endif // MOCK_TEMPERATURE_SENSOR_FALLBACK_H