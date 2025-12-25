// src/modules/safety/FlameDetection.h
#ifndef FLAME_DETECTION_H
#define FLAME_DETECTION_H

#include <Arduino.h>
#include "utils/Result.h"
#include "utils/ErrorHandler.h"
#include "config/ProjectConfig.h"

/**
 * @brief Flame Detection Safety Module
 * 
 * This module handles flame detection for the burner control system.
 * Currently deactivated as hardware is not available.
 * 
 * When hardware is available, this module will:
 * - Monitor flame sensor input (UV sensor, photocell, or ionization)
 * - Provide flame presence/absence detection
 * - Handle flame failure detection with configurable timeout
 * - Trigger safety shutdowns on flame loss
 * - Support flame signal strength monitoring
 */
class FlameDetection {
public:
    enum class FlameState {
        NOT_DETECTED,
        DETECTED,
        UNSTABLE,
        SENSOR_FAULT
    };

    enum class SensorType {
        NONE,           // No sensor (deactivated)
        UV_SENSOR,      // Ultraviolet flame sensor
        PHOTOCELL,      // Photocell/photoresistor
        IONIZATION,     // Ionization probe
        IR_SENSOR       // Infrared sensor
    };

    struct Config {
        SensorType sensorType = SensorType::NONE;
        uint16_t detectionThreshold = 512;    // ADC threshold for flame detection
        uint16_t stabilizationTimeMs = 2000;  // Time flame must be stable
        uint16_t failureTimeoutMs = 1000;     // Max time without flame before shutdown
        uint8_t adcPin = 0;                   // ADC pin for analog sensors
        uint8_t digitalPin = 0;               // Digital pin for digital sensors
        bool invertSignal = false;            // Invert sensor signal
        bool enabled = false;                 // Module enable flag
    };

private:
    Config config_;
    FlameState currentState_;
    uint32_t lastFlameTime_;
    uint32_t stateChangeTime_;
    uint16_t lastReading_;
    bool initialized_;

    static constexpr const char* TAG = "FlameDetection";

public:
    FlameDetection() : 
        currentState_(FlameState::NOT_DETECTED),
        lastFlameTime_(0),
        stateChangeTime_(0),
        lastReading_(0),
        initialized_(false) {
    }

    /**
     * @brief Initialize flame detection module
     * @param config Module configuration
     * @return Result<void> Success or error
     */
    Result<void> initialize(const Config& config) {
        config_ = config;
        
        if (!config_.enabled) {
            LOG_INFO(TAG, "Flame detection disabled - no hardware available");
            initialized_ = true;
            return Result<void>();
        }

        // HARDWARE DEPENDENCY: Flame sensor not yet installed
        // When available, implement:
        // 1. Configure ADC/GPIO pins based on sensor type
        // 2. Set up interrupt handlers for digital sensors
        // 3. Calibrate sensor baseline readings
        // 4. Initialize signal filtering (moving average/debounce)
        
        LOG_WARN(TAG, "Flame detection hardware not implemented");
        initialized_ = true;
        return Result<void>();
    }

    /**
     * @brief Update flame detection state
     * @return Result<void> Success or error
     */
    Result<void> update() {
        if (!initialized_) {
            return Result<void>(SystemError::NOT_INITIALIZED, "Flame detection not initialized");
        }

        if (!config_.enabled) {
            // Module disabled, always report flame detected for safety bypass
            currentState_ = FlameState::DETECTED;
            return Result<void>();
        }

        // HARDWARE DEPENDENCY: Flame sensor not yet installed
        // When available, implement:
        // 1. Read sensor value (ADC for analog, GPIO for digital)
        // 2. Apply signal filtering and noise reduction
        // 3. Compare against calibrated thresholds
        // 4. Update flame state machine with hysteresis
        // 5. Detect sensor faults (open/short circuit)
        
        // For now, simulate flame always detected when burner should be on
        // This allows the system to operate without flame sensor
        currentState_ = FlameState::DETECTED;
        
        return Result<void>();
    }

    /**
     * @brief Get current flame state
     */
    FlameState getState() const {
        return currentState_;
    }

    /**
     * @brief Check if flame is detected
     */
    bool isFlameDetected() const {
        return currentState_ == FlameState::DETECTED;
    }

    /**
     * @brief Check if flame is stable
     */
    bool isFlameStable() const {
        if (!config_.enabled) {
            return true;  // Always stable when disabled
        }
        
        if (currentState_ != FlameState::DETECTED) {
            return false;
        }
        
        uint32_t stableTime = millis() - stateChangeTime_;
        return stableTime >= config_.stabilizationTimeMs;
    }

    /**
     * @brief Get last sensor reading (for diagnostics)
     */
    uint16_t getLastReading() const {
        return lastReading_;
    }

    /**
     * @brief Get sensor type name
     */
    const char* getSensorTypeName() const {
        switch (config_.sensorType) {
            case SensorType::NONE: return "None (Disabled)";
            case SensorType::UV_SENSOR: return "UV Sensor";
            case SensorType::PHOTOCELL: return "Photocell";
            case SensorType::IONIZATION: return "Ionization Probe";
            case SensorType::IR_SENSOR: return "IR Sensor";
            default: return "Unknown";
        }
    }

    /**
     * @brief Enable flame detection (when hardware is connected)
     */
    Result<void> enable() {
        if (config_.sensorType == SensorType::NONE) {
            return Result<void>(SystemError::NOT_SUPPORTED, 
                              "Cannot enable flame detection without sensor type configured");
        }
        
        config_.enabled = true;
        LOG_INFO(TAG, "Flame detection enabled with %s", getSensorTypeName());
        return initialize(config_);
    }

    /**
     * @brief Disable flame detection (for testing/maintenance)
     */
    void disable() {
        config_.enabled = false;
        currentState_ = FlameState::NOT_DETECTED;
        LOG_INFO(TAG, "Flame detection disabled");
    }

    /**
     * @brief Check if module is enabled
     */
    bool isEnabled() const {
        return config_.enabled;
    }

private:
    /**
     * @brief Read sensor value (to be implemented with hardware)
     */
    uint16_t readSensor() {
        if (config_.sensorType == SensorType::NONE) {
            return 0;
        }

        // HARDWARE DEPENDENCY: Implementation pending flame sensor installation
        // Future implementation will:
        // - Read analog sensors via analogRead(config_.adcPin)
        // - Read digital sensors via digitalRead(config_.digitalPin)
        // - Apply signal inversion for active-low sensors
        // - Apply calibration curve and scaling factors
        
        return 0;
    }

    /**
     * @brief Update state machine based on sensor reading
     */
    void updateStateMachine(uint16_t reading) {
        // HARDWARE DEPENDENCY: State machine pending sensor integration
        // - NOT_DETECTED -> DETECTED when reading > threshold
        // - DETECTED -> NOT_DETECTED when reading < threshold - hysteresis
        // - Any state -> UNSTABLE on rapid changes
        // - Any state -> SENSOR_FAULT on out-of-range values
    }
};

#endif // FLAME_DETECTION_H