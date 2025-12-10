/**
 * @file MockHeatingControl.h
 * @brief Mock implementation of HeatingControlModule for testing
 */

#ifndef MOCK_HEATING_CONTROL_H
#define MOCK_HEATING_CONTROL_H

#include "../../include/shared/Temperature.h"
#include "MockSharedSensorReadings.h"
#include "MockSystemSettings.h"

class HeatingControlModule {
private:
    float pidKp_, pidKi_, pidKd_;
    float integral_;
    float lastError_;
    bool pidEnabled_;
    
public:
    HeatingControlModule() 
        : pidKp_(2.0f), pidKi_(0.1f), pidKd_(0.5f),
          integral_(0.0f), lastError_(0.0f), pidEnabled_(false) {}
    
    void initializePID(const SystemSettings& settings) {
        pidKp_ = settings.pid_kp;
        pidKi_ = settings.pid_ki;
        pidKd_ = settings.pid_kd;
        pidEnabled_ = settings.pid_enable;
        integral_ = 0.0f;
        lastError_ = 0.0f;
    }
    
    Temperature_t calculateSpaceHeatingTargetTemp(
        const SharedSensorReadings& readings, 
        const SystemSettings& settings) const {
        
        if (!settings.heating_curve_enable) {
            return settings.heating_target_temperature;
        }
        
        // Simple weather compensation
        float outsideTemp = tempToFloat(readings.outsideTemp);
        float targetBase = tempToFloat(settings.heating_target_temperature);
        float curve = settings.heating_curve_coeff * (20.0f - outsideTemp);
        float target = targetBase + curve + settings.heating_curve_shift;
        
        // Clamp to reasonable range
        if (target > 85.0f) target = 85.0f;
        if (target < 20.0f) target = 20.0f;
        
        return tempFromFloat(target);
    }
    
    bool checkHeatingConditions(
        const SharedSensorReadings& readings,
        Temperature_t targetTemperature,
        Temperature_t hysteresis) const {
        
        Temperature_t currentTemp = readings.insideTemp;
        Temperature_t threshold = targetTemperature - hysteresis;
        
        // Simple hysteresis logic
        if (currentTemp < threshold) {
            return true;  // Turn on
        } else if (currentTemp >= targetTemperature) {
            return false; // Turn off
        }
        
        // In dead band - maintain current state (simplified)
        return false;
    }
    
    float calculatePIDOutput(float setpoint, float current, float dt) {
        if (!pidEnabled_) {
            return (setpoint > current) ? 100.0f : 0.0f;
        }
        
        float error = setpoint - current;
        
        // Proportional term
        float pTerm = pidKp_ * error;
        
        // Integral term
        integral_ += error * dt;
        float iTerm = pidKi_ * integral_;
        
        // Derivative term
        float dTerm = pidKd_ * (error - lastError_) / dt;
        
        lastError_ = error;
        
        // Calculate output
        float output = pTerm + iTerm + dTerm;
        
        // Clamp output
        if (output > 100.0f) output = 100.0f;
        if (output < 0.0f) output = 0.0f;
        
        return output;
    }
};

#endif // MOCK_HEATING_CONTROL_H