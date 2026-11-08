/**
 * @file MockHeatingControl.h
 * @brief Mock implementation of HeatingControlModule for testing
 */

#ifndef MOCK_HEATING_CONTROL_H
#define MOCK_HEATING_CONTROL_H

#include "../../include/shared/Temperature.h"
#include "../../include/modules/control/SpaceHeatingPolicy.h"  // the rule the FIRMWARE runs
#include "MockSharedSensorReadings.h"
#include "MockSystemSettings.h"

class HeatingControlModule {
private:
    float pidKp_, pidKi_, pidKd_;
    float integral_;
    float lastError_;
    bool pidEnabled_;
    bool currentlyHeating_ = false;
    
public:
    HeatingControlModule() 
        : pidKp_(2.0f), pidKi_(0.1f), pidKd_(0.5f),
          integral_(0.0f), lastError_(0.0f), pidEnabled_(false) {}
    
    void initializePID(const SystemSettings& settings) {
        pidKp_ = settings.spaceHeatingKp;
        pidKi_ = settings.spaceHeatingKi;
        pidKd_ = settings.spaceHeatingKd;
        pidEnabled_ = settings.useBoilerTempPID;
        integral_ = 0.0f;
        lastError_ = 0.0f;
    }

    Temperature_t calculateSpaceHeatingTargetTemp(
        const SharedSensorReadings& readings,
        const SystemSettings& settings) const {

        if (!settings.useWeatherCompensatedControl) {
            return settings.targetTemperatureInside;
        }

        // Simple weather compensation
        float outsideTemp = tempToFloat(readings.outsideTemp);
        float targetBase = tempToFloat(settings.targetTemperatureInside);
        float curve = settings.heating_curve_coeff * (20.0f - outsideTemp);
        float target = targetBase + curve + settings.heating_curve_shift;
        
        // Clamp to reasonable range
        if (target > 85.0f) target = 85.0f;
        if (target < 20.0f) target = 20.0f;
        
        return tempFromFloat(target);
    }
    
    /**
     * @brief Does the room thermostat want heat?
     *
     * REWIRED 2026-11-08 (S2). This used to REIMPLEMENT the rule, and it reimplemented it
     * WRONGLY - it turned off at `target` while the firmware turns off at
     * `setpoint + hysteresis`, and it ignored the current heating state entirely, which is
     * the whole point of asymmetric hysteresis. So every test that called it was asserting
     * the behaviour of the mock, not of the controller, and would have passed no matter what
     * the firmware did.
     *
     * There were three copies of this rule in the tree and all three disagreed. The dead one
     * in HeatingControlModule has been deleted; this now DELEGATES to the single live
     * predicate so these tests exercise the real thing.
     *
     * Validity is the caller's business, exactly as in the firmware: HeatingControlTask
     * checks isInsideTempValid before calling the predicate, so the mock does too rather
     * than hiding a dead sensor behind a plausible answer.
     */
    bool checkHeatingConditions(
        const SharedSensorReadings& readings,
        Temperature_t targetTemperature,
        Temperature_t hysteresis) const {

        if (!readings.isInsideTempValid) {
            return false;  // no measurement, no demand - fail safe, as the live path does
        }
        return SpaceHeatingPolicy::roomThermostatWantsHeat(
            currentlyHeating_, readings.insideTemp, targetTemperature, hysteresis);
    }

    /// The state the asymmetric hysteresis depends on. Tests that care about the dead band
    /// must set it; the firmware threads the real heating state into the same argument.
    void setCurrentlyHeating(bool on) { currentlyHeating_ = on; }

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