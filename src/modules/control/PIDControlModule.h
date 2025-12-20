// src/modules/control/PIDControlModule.h
#ifndef PID_CONTROL_MODULE_H
#define PID_CONTROL_MODULE_H

#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "PIDAutoTuner.h"

/**
 * @brief Thread-safe PID Control Module
 * 
 * Provides reusable methods for calculating PID adjustments and determining adjustment levels.
 * This class is now instance-based with proper mutex protection for thread safety.
 */
class PIDControlModule {
private:
    // Instance variables for PID state
    float integral;
    float previousError;
    
    // Mutex for thread safety
    mutable SemaphoreHandle_t pidMutex;
    
    // Auto-tuning
    PIDAutoTuner* autoTuner;
    bool autoTuningActive;
    float autoTuneSetpoint;
    
    // Current PID parameters
    float currentKp;
    float currentKi;
    float currentKd;
    
    // Anti-windup limits are now in SystemConstants::PID
    
 public:
    /**
     * @brief Constructor - initializes PID state and mutex
     */
    PIDControlModule();
    
    /**
     * @brief Destructor - cleans up mutex
     */
    ~PIDControlModule();
    
    // Delete copy constructor and assignment operator
    PIDControlModule(const PIDControlModule&) = delete;
    PIDControlModule& operator=(const PIDControlModule&) = delete;
    
    /**
     * @brief Calculate the PID adjustment (thread-safe).
     * 
     * This function calculates the adjustment required to minimize the error 
     * between the set point and the current temperature.
     * 
     * @param setPoint The target temperature.
     * @param currentTemp The current temperature.
     * @param Kp Proportional gain.
     * @param Ki Integral gain.
     * @param Kd Derivative gain.
     * @return The adjustment value.
     */
    float calculatePIDAdjustment(float setPoint, float currentTemp, float Kp, float Ki, float Kd);

    /**
     * @brief Reset the PID controller state (thread-safe)
     */
    void reset();
    
    /**
     * @brief Determine the adjustment level.
     * 
     * This function converts the calculated adjustment into a discrete adjustment level.
     * 
     * @param adjustment The calculated adjustment.
     * @return A discrete adjustment level (e.g., 0 = no adjustment, 1 = low, 2 = medium, 3 = high).
     */
    static uint32_t determineAdjustmentLevel(float adjustment);
    
    /**
     * @brief Start auto-tuning process
     * 
     * @param setpoint Target temperature for tuning
     * @param method Tuning method to use
     * @return true if auto-tuning started successfully
     */
    bool startAutoTuning(float setpoint, 
                        PIDAutoTuner::TuningMethod method = PIDAutoTuner::TuningMethod::ZIEGLER_NICHOLS_PI);
    
    /**
     * @brief Stop auto-tuning process
     */
    void stopAutoTuning();
    
    /**
     * @brief Check if auto-tuning is active
     */
    bool isAutoTuningActive() const { return autoTuningActive; }
    
    /**
     * @brief Get auto-tuning status
     */
    PIDAutoTuner::TuningState getAutoTuningState() const;
    
    /**
     * @brief Get auto-tuning progress (0-100%)
     */
    uint8_t getAutoTuningProgress() const;
    
    /**
     * @brief Update auto-tuning (call periodically when auto-tuning is active)
     * 
     * @param currentTemp Current temperature
     * @param currentTime Current time in seconds
     * @return Control output for auto-tuning (-100 to 100)
     */
    float updateAutoTuning(float currentTemp, float currentTime);
    
    /**
     * @brief Apply auto-tuning results
     * 
     * @return true if results were successfully applied
     */
    bool applyAutoTuningResults();
    
    /**
     * @brief Get current PID parameters
     */
    void getCurrentParameters(float& Kp, float& Ki, float& Kd) const;
    
    /**
     * @brief Set PID parameters
     */
    void setParameters(float Kp, float Ki, float Kd);
};

#endif // PID_CONTROL_MODULE_H
