// include/modules/control/PIDControlModuleFixedPoint.h
#ifndef PID_CONTROL_MODULE_FIXED_POINT_H
#define PID_CONTROL_MODULE_FIXED_POINT_H

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "shared/Temperature.h"
#include <cstdint>

/**
 * @file PIDControlModuleFixedPoint.h
 * @brief Fixed-point PID controller implementation
 * 
 * This module provides a fixed-point PID controller that operates entirely
 * with integer arithmetic, eliminating float calculations for better performance
 * and deterministic behavior on embedded systems.
 * 
 * PID parameters use fixed-point representation with 3 decimal places (scale 1000)
 * For example: Kp = 2.5 is represented as 2500
 */

// Fixed-point type definitions
typedef int32_t PIDValue_t;     // Represents values * 1000 (3 decimal places)
typedef int64_t PIDProduct_t;   // For intermediate calculations to prevent overflow

class PIDControlModuleFixedPoint {
public:
    // PID parameter limits (in fixed-point format)
    static constexpr PIDValue_t KP_MIN = 0;        // 0.000
    static constexpr PIDValue_t KP_MAX = 100000;   // 100.000
    static constexpr PIDValue_t KI_MIN = 0;        // 0.000
    static constexpr PIDValue_t KI_MAX = 10000;    // 10.000
    static constexpr PIDValue_t KD_MIN = 0;        // 0.000
    static constexpr PIDValue_t KD_MAX = 50000;    // 50.000
    
    // Output limits (in temperature units 0.1°C)
    static constexpr Temperature_t OUTPUT_MIN = -1000;  // -100.0°C adjustment
    static constexpr Temperature_t OUTPUT_MAX = 1000;   // +100.0°C adjustment
    
    // Integral windup limits (scaled)
    static constexpr PIDValue_t INTEGRAL_MIN = -100000;  // -100.000
    static constexpr PIDValue_t INTEGRAL_MAX = 100000;   // +100.000

    PIDControlModuleFixedPoint();
    ~PIDControlModuleFixedPoint();

    /**
     * @brief Calculate PID adjustment using fixed-point arithmetic
     * @param setPoint Target temperature in 0.1°C units
     * @param currentTemp Current temperature in 0.1°C units
     * @param Kp Proportional gain (scaled by 1000)
     * @param Ki Integral gain (scaled by 1000)
     * @param Kd Derivative gain (scaled by 1000)
     * @param dtMs Time delta in milliseconds
     * @return Temperature adjustment in 0.1°C units
     */
    Temperature_t calculatePIDAdjustment(Temperature_t setPoint, Temperature_t currentTemp,
                                       PIDValue_t Kp, PIDValue_t Ki, PIDValue_t Kd,
                                       uint32_t dtMs);

    /**
     * @brief Reset PID controller state
     */
    void reset();

    /**
     * @brief Set integral limits for anti-windup
     * @param min Minimum integral value (scaled)
     * @param max Maximum integral value (scaled)
     */
    void setIntegralLimits(PIDValue_t min, PIDValue_t max);

    /**
     * @brief Set output limits
     * @param min Minimum output in 0.1°C units
     * @param max Maximum output in 0.1°C units
     */
    void setOutputLimits(Temperature_t min, Temperature_t max);

    /**
     * @brief Get current integral value (for debugging)
     * @return Current integral term (scaled)
     */
    PIDValue_t getIntegral() const { return integral; }

    /**
     * @brief Convert float PID parameters to fixed-point
     * @param value Float value
     * @return Fixed-point representation (scaled by 1000)
     */
    static PIDValue_t floatToFixed(float value) {
        return static_cast<PIDValue_t>(value * 1000.0f + 0.5f);
    }

    /**
     * @brief Convert fixed-point to float (for display/logging)
     * @param value Fixed-point value
     * @return Float representation
     */
    static float fixedToFloat(PIDValue_t value) {
        return static_cast<float>(value) / 1000.0f;
    }

    /**
     * @brief Save PID controller state to FRAM
     * @param controllerId Unique ID for this controller (0-255)
     * @return true if successfully saved
     */
    bool saveState(uint8_t controllerId);

    /**
     * @brief Restore PID controller state from FRAM
     * @param controllerId Unique ID for this controller (0-255)
     * @return true if successfully restored
     */
    bool restoreState(uint8_t controllerId);

    /**
     * @brief Get the last update time
     * @return Last update time in milliseconds
     */
    uint32_t getLastUpdateTime() const { return lastUpdateTime; }

private:
    // PID state variables (all in fixed-point)
    PIDValue_t integral;           // Accumulated integral (scaled)
    Temperature_t previousPV;      // Previous process variable (for derivative-on-PV)
    bool firstRun;                 // Skip derivative on first run after reset
    uint32_t lastUpdateTime;       // Last update time in millis
    
    // Limits
    PIDValue_t integralMin;
    PIDValue_t integralMax;
    Temperature_t outputMin;
    Temperature_t outputMax;
    
    // Thread safety
    SemaphoreHandle_t pidMutex;
    
    // Helper function to clamp values
    template<typename T>
    static T clamp(T value, T min, T max) {
        if (value < min) return min;
        if (value > max) return max;
        return value;
    }
};

#endif // PID_CONTROL_MODULE_FIXED_POINT_H