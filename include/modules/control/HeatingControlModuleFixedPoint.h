// include/modules/control/HeatingControlModuleFixedPoint.h
#ifndef HEATING_CONTROL_MODULE_FIXED_POINT_H
#define HEATING_CONTROL_MODULE_FIXED_POINT_H

#include "modules/control/PIDControlModuleFixedPoint.h"
#include "shared/TemperatureArithmetic.h"
#include "shared/Temperature.h"
#include <cstdint>

/**
 * @file HeatingControlModuleFixedPoint.h
 * @brief Demonstrates fixed-point heating control calculations
 * 
 * This module shows how to use the fixed-point PID controller and
 * temperature arithmetic for heating control without float operations.
 */

class HeatingControlModuleFixedPoint {
public:
    /**
     * @brief Calculate heating curve target temperature using fixed-point math
     * @param insideTemp Current inside temperature (0.1°C units)
     * @param outsideTemp Current outside temperature (0.1°C units)
     * @param curveCoeff Heating curve coefficient (scaled by 100)
     * @param curveShift Heating curve shift (0.1°C units)
     * @param lowerLimit Lower temperature limit (0.1°C units)
     * @param upperLimit Upper temperature limit (0.1°C units)
     * @return Target temperature (0.1°C units)
     */
    static Temperature_t calculateHeatingCurveTarget(
        Temperature_t insideTemp,
        Temperature_t outsideTemp,
        int16_t curveCoeff,    // Coefficient * 100 (e.g., 150 = 1.5)
        Temperature_t curveShift,
        Temperature_t lowerLimit,
        Temperature_t upperLimit
    );

    /**
     * @brief Example of complete fixed-point PID control loop
     * @param targetTemp Target temperature (0.1°C units)
     * @param currentTemp Current temperature (0.1°C units)
     * @param pidController Fixed-point PID controller instance
     * @param kp Proportional gain (scaled by 1000)
     * @param ki Integral gain (scaled by 1000)
     * @param kd Derivative gain (scaled by 1000)
     * @param dtMs Time delta in milliseconds
     * @return Power level (0-10 scale)
     */
    static uint8_t calculatePowerLevel(
        Temperature_t targetTemp,
        Temperature_t currentTemp,
        PIDControlModuleFixedPoint& pidController,
        PIDValue_t kp,
        PIDValue_t ki,
        PIDValue_t kd,
        uint32_t dtMs
    );

    /**
     * @brief Convert existing float PID parameters to fixed-point
     * @param floatKp Float Kp value
     * @param floatKi Float Ki value
     * @param floatKd Float Kd value
     * @param fixedKp Output: Fixed-point Kp
     * @param fixedKi Output: Fixed-point Ki
     * @param fixedKd Output: Fixed-point Kd
     */
    static void convertPIDParameters(
        float floatKp, float floatKi, float floatKd,
        PIDValue_t& fixedKp, PIDValue_t& fixedKi, PIDValue_t& fixedKd
    );

private:
    // Helper for fixed-point multiplication with scaling
    static int32_t fixedMultiply(int32_t a, int32_t b, int32_t scale);
};

#endif // HEATING_CONTROL_MODULE_FIXED_POINT_H