// include/modules/control/BoilerTempController.h
#pragma once

#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "shared/Temperature.h"
#include "modules/control/PIDControlModuleFixedPoint.h"
#include "modules/control/PIDAutoTuner.h"

/**
 * @brief Boiler Temperature Controller for cascade control
 *
 * Supports two control modes:
 * - TWO_STAGE (bang-bang): Simple hysteresis-based OFF/HALF/FULL control
 * - MODULATING (PID): Smoother PID-based control with output mapped to power levels
 *
 * Control Flow:
 *   Heating Curve → Target Boiler Temp → BoilerTempController → Power Level → Burner
 *
 * PID Mode (default):
 *   - PID calculates 0-100% output based on temperature error
 *   - Output mapped to power levels with hysteresis:
 *     * 0-25% → OFF, 25-75% → HALF, 75-100% → FULL
 *   - Smoother transitions, supports auto-tuning
 *
 * Bang-Bang Mode:
 *   - Simple threshold-based control
 *   - FULL: error > fullPowerThreshold
 *   - HALF: error > onHysteresis
 *   - OFF: error < -offHysteresis
 */
class BoilerTempController {
public:
    /**
     * @brief Burner type for control strategy selection
     */
    enum class BurnerType {
        TWO_STAGE,    // OFF/HALF/FULL bang-bang control
        MODULATING    // 0-100% PID control (future)
    };

    /**
     * @brief Power level output for two-stage burners
     */
    enum class PowerLevel {
        OFF = 0,
        HALF = 1,
        FULL = 2
    };

    /**
     * @brief Control output from the controller
     */
    struct ControlOutput {
        bool burnerOn;              // Whether burner should be running
        PowerLevel powerLevel;      // Power level for two-stage
        uint8_t modulationPercent;  // 0-100% for modulating (future)
        bool changed;               // Whether output changed from last calculation
    };

    /**
     * @brief Configuration for the controller
     */
    struct Config {
        BurnerType burnerType = BurnerType::MODULATING;  // Default to PID mode

        // Hysteresis bands for bang-bang mode (Temperature_t = tenths of degrees)
        Temperature_t offHysteresis = 50;       // +5.0°C above target → OFF
        Temperature_t onHysteresis = 30;        // -3.0°C below target → ON (HALF)
        Temperature_t fullPowerThreshold = 100; // -10.0°C below target → FULL

        // Minimum valid target temperature (safety)
        Temperature_t minTargetTemp = 200;      // 20.0°C minimum target

        // PID gains for space heating (modulating mode)
        float modKp = 5.0f;     // Proportional gain (higher for fast response)
        float modKi = 0.02f;    // Integral gain (lower to prevent overshoot)
        float modKd = 1.0f;     // Derivative gain (damping)

        // PID gains for water heating (separate from space heating)
        float waterKp = 5.0f;   // Water heating proportional gain
        float waterKi = 0.02f;  // Water heating integral gain
        float waterKd = 1.0f;   // Water heating derivative gain

        // PID output thresholds for mapping to power levels (0-100%)
        // Pure PID mode - output centered at 50% when at target
        // With Kp=5.0: ~10% shift per 2°C error
        // Wide bands minimize burner cycling
        uint8_t offThreshold = 35;      // Below this → OFF (well above target)
        uint8_t halfThreshold = 45;     // Above this → at least HALF
        uint8_t fullThreshold = 75;     // Above this → FULL (significantly below target)
        uint8_t thresholdHysteresis = 10; // Wide hysteresis prevents oscillation
    };

    /**
     * @brief Initialize the controller
     * @return true if initialization successful
     */
    bool initialize();

    /**
     * @brief Set controller configuration
     * @param config New configuration
     */
    void setConfig(const Config& config);

    /**
     * @brief Get current configuration
     * @return Current configuration
     */
    Config getConfig() const;

    /**
     * @brief Calculate control output based on temperatures
     * @param targetTemp Target boiler temperature (from Room/Water PID)
     * @param currentTemp Current boiler output temperature
     * @return Control output (power level and burner state)
     */
    ControlOutput calculate(Temperature_t targetTemp, Temperature_t currentTemp);

    /**
     * @brief Reset controller state
     */
    void reset();

    /**
     * @brief Get the last calculated output
     * @return Last output
     */
    ControlOutput getLastOutput() const;

    /**
     * @brief Check if controller is initialized
     * @return true if initialized
     */
    bool isInitialized() const { return initialized_; }

    /**
     * @brief Convert PowerLevel to string for logging
     * @param level Power level
     * @return String representation
     */
    static const char* powerLevelToString(PowerLevel level);

    /**
     * @brief Set PID gains (thread-safe)
     * @param kp Proportional gain
     * @param ki Integral gain
     * @param kd Derivative gain
     */
    void setPIDGains(float kp, float ki, float kd);

    /**
     * @brief Get current PID gains (thread-safe)
     * @param kp Output: Proportional gain
     * @param ki Output: Integral gain
     * @param kd Output: Derivative gain
     */
    void getPIDGains(float& kp, float& ki, float& kd) const;

    /**
     * @brief Reset PID controller state (clears integral accumulator)
     */
    void resetPID();

    /**
     * @brief Get current PID output percentage (0-100)
     * @return Last calculated PID output
     */
    uint8_t getPIDOutput() const { return lastPIDOutput_; }

    /**
     * @brief Update mode based on current burner request event bits
     * Switches PID gains between space heating and water heating
     */
    void updateMode();

    /**
     * @brief Check if currently in water heating mode
     * @return true if water heating mode active
     */
    bool isWaterMode() const { return isWaterMode_; }

    // ===== Auto-Tuning Support =====

    /**
     * @brief Set the tuning method for auto-tuning
     * Available methods: zn_pi, zn_pid, tyreus, cohen, lambda
     * @param method Method name string
     * @return true if method is valid
     */
    bool setTuningMethod(const char* method);

    /**
     * @brief Start PID auto-tuning
     * @param setpoint Target temperature for tuning
     * @return true if tuning started successfully
     */
    bool startAutoTuning(Temperature_t setpoint);

    /**
     * @brief Stop PID auto-tuning
     */
    void stopAutoTuning();

    /**
     * @brief Check if auto-tuning is active
     * @return true if tuning in progress
     */
    bool isAutoTuning() const { return autoTuningActive_; }

    /**
     * @brief Update auto-tuning with current temperature
     * Call this periodically during tuning
     * @param currentTemp Current boiler temperature
     * @return Control output to apply (for relay feedback test)
     */
    ControlOutput updateAutoTuning(Temperature_t currentTemp);

    /**
     * @brief Apply auto-tuning results to PID gains
     * @return true if results applied successfully
     */
    bool applyAutoTuningResults();

    /**
     * @brief Get auto-tuning progress (0-100%)
     * @return Progress percentage
     */
    uint8_t getAutoTuningProgress() const;

    /**
     * @brief Get the tuned PID gains after auto-tuning
     * @param kp Output: Proportional gain
     * @param ki Output: Integral gain
     * @param kd Output: Derivative gain
     * @return true if valid results are available
     */
    bool getTunedGains(float& kp, float& ki, float& kd) const;

private:
    Config config_;
    ControlOutput lastOutput_;
    bool initialized_ = false;
    mutable SemaphoreHandle_t mutex_ = nullptr;
    static constexpr TickType_t MUTEX_TIMEOUT = pdMS_TO_TICKS(50);

    // PID controller for modulating mode
    PIDControlModuleFixedPoint* pidController_ = nullptr;
    uint8_t lastPIDOutput_ = 0;
    uint32_t lastPIDTime_ = 0;

    // Mode tracking for gain switching
    bool isWaterMode_ = false;

    // Auto-tuning support
    PIDAutoTuner* autoTuner_ = nullptr;
    bool autoTuningActive_ = false;
    Temperature_t autoTuneSetpoint_ = 0;
    PIDAutoTuner::TuningMethod tuningMethod_ = PIDAutoTuner::TuningMethod::ZIEGLER_NICHOLS_PID;

    static const char* TAG;

    /**
     * @brief Calculate output using bang-bang control for two-stage burner
     * @param target Target temperature
     * @param current Current temperature
     * @return Control output
     */
    ControlOutput calculateBangBang(Temperature_t target, Temperature_t current);

    /**
     * @brief Calculate output using PID for modulating burner (future)
     * @param target Target temperature
     * @param current Current temperature
     * @return Control output
     */
    ControlOutput calculateModulating(Temperature_t target, Temperature_t current);

    /**
     * @brief Check if target temperature is valid
     * @param target Target temperature to check
     * @return true if valid
     */
    bool isValidTarget(Temperature_t target) const;
};
