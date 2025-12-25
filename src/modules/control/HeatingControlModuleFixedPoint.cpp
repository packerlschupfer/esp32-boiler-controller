// src/modules/control/HeatingControlModuleFixedPoint.cpp
#include "modules/control/HeatingControlModuleFixedPoint.h"
#include "config/SystemConstants.h"
#include "LoggingMacros.h"
#include <algorithm>

[[maybe_unused]] static const char* TAG = "HeatingFixedPoint";

Temperature_t HeatingControlModuleFixedPoint::calculateHeatingCurveTarget(
    Temperature_t insideTemp,
    Temperature_t outsideTemp,
    int16_t curveCoeff,
    Temperature_t curveShift,
    Temperature_t lowerLimit,
    Temperature_t upperLimit) {
    
    // Calculate temperature difference using function from Temperature.h
    Temperature_t tempDiff = tempSub(outsideTemp, insideTemp);
    
    // Original formula (with floats):
    // target = inside + shift - coeff * diff * (1.4347 + 0.021 * diff + 0.000248 * diff^2)
    
    // Fixed-point implementation:
    // We'll use different scales for different parts to maintain precision
    
    // First, calculate the polynomial part: (1.4347 + 0.021 * diff + 0.000248 * diff^2)
    // Scale factors: 1.4347 -> 14347 (x10000)
    //                0.021 -> 210 (x10000)  
    //                0.000248 -> 248 (x1000000)
    
    int32_t diff = static_cast<int32_t>(tempDiff); // in 0.1°C units
    
    // Calculate diff^2
    int32_t diffSquared = (diff * diff) / 10; // Scale back to avoid overflow
    
    // Polynomial calculation (scaled by 10000)
    int32_t polynomial = SystemConstants::FixedPoint::HEATING_CURVE_COEFF_1;
    polynomial += (SystemConstants::FixedPoint::HEATING_CURVE_COEFF_2 * diff) / 10;
    polynomial += (SystemConstants::FixedPoint::HEATING_CURVE_COEFF_3 * diffSquared) / 1000;
    
    // Now calculate: coeff * diff * polynomial
    // coeff is scaled by 100, diff is in 0.1°C units, polynomial is scaled by 10000
    int64_t adjustment = static_cast<int64_t>(curveCoeff) * diff * polynomial;
    
    // Scale back: adjustment / (100 * 10 * 10000) = adjustment / 10000000
    adjustment = adjustment / SystemConstants::FixedPoint::ADJUSTMENT_SCALE;
    
    // Calculate final target temperature
    Temperature_t target = insideTemp;
    target = tempAdd(target, curveShift);
    target = tempSub(target, static_cast<Temperature_t>(adjustment));
    
    // Apply limits
    if (target < lowerLimit) {
        target = lowerLimit;
    } else if (target > upperLimit) {
        target = upperLimit;
    }
    
    LOG_DEBUG(TAG, "Heating curve: inside=%d, outside=%d, diff=%d, target=%d (0.1°C units)",
              insideTemp, outsideTemp, tempDiff, target);
    
    return target;
}

uint8_t HeatingControlModuleFixedPoint::calculatePowerLevel(
    Temperature_t targetTemp,
    Temperature_t currentTemp,
    PIDControlModuleFixedPoint& pidController,
    PIDValue_t kp,
    PIDValue_t ki,
    PIDValue_t kd,
    uint32_t dtMs) {
    
    // Get PID adjustment
    Temperature_t adjustment = pidController.calculatePIDAdjustment(
        targetTemp, currentTemp, kp, ki, kd, dtMs
    );
    
    // Convert adjustment to power level (0-10 scale)
    // Positive adjustment means we need heating
    // Map adjustment range to power levels
    
    if (adjustment <= 0) {
        return 0;  // No heating needed
    }
    
    // Map adjustment to power level
    // Assume max adjustment of 100°C (1000 in 0.1°C units)
    // Linear mapping: 0-100°C -> 0-10 power level
    uint8_t powerLevel = static_cast<uint8_t>((adjustment * 10) / 1000);
    
    // Clamp to valid range
    if (powerLevel > 10) {
        powerLevel = 10;
    }
    
    LOG_DEBUG(TAG, "Power calculation: target=%d, current=%d, adj=%d, power=%d",
              targetTemp, currentTemp, adjustment, powerLevel);
    
    return powerLevel;
}

void HeatingControlModuleFixedPoint::convertPIDParameters(
    float floatKp, float floatKi, float floatKd,
    PIDValue_t& fixedKp, PIDValue_t& fixedKi, PIDValue_t& fixedKd) {
    
    fixedKp = PIDControlModuleFixedPoint::floatToFixed(floatKp);
    fixedKi = PIDControlModuleFixedPoint::floatToFixed(floatKi);
    fixedKd = PIDControlModuleFixedPoint::floatToFixed(floatKd);
    
    LOG_DEBUG(TAG, "Converted PID params: Kp=%.3f->%d, Ki=%.3f->%d, Kd=%.3f->%d",
              floatKp, fixedKp, floatKi, fixedKi, floatKd, fixedKd);
}

int32_t HeatingControlModuleFixedPoint::fixedMultiply(int32_t a, int32_t b, int32_t scale) {
    // Perform multiplication with proper scaling to avoid overflow
    int64_t product = static_cast<int64_t>(a) * b;
    return static_cast<int32_t>(product / scale);
}