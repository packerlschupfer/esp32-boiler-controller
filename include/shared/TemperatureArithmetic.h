// include/shared/TemperatureArithmetic.h
#ifndef TEMPERATURE_ARITHMETIC_H
#define TEMPERATURE_ARITHMETIC_H

#include "Temperature.h"
#include <cstdint>
#include <algorithm>

/**
 * @file TemperatureArithmetic.h
 * @brief Extended fixed-point arithmetic operations for Temperature_t values
 * 
 * This file provides additional efficient fixed-point arithmetic operations for 
 * temperature calculations beyond those in Temperature.h, eliminating the need 
 * for float conversions. All operations work directly with Temperature_t (int16_t) 
 * values representing 0.1°C units.
 * 
 * Note: tempAdd() and tempSub() are already defined in Temperature.h
 */

// Additional temperature arithmetic functions

/**
 * @brief Multiply temperature by integer factor
 * @param temp Temperature value
 * @param factor Multiplication factor
 * @return Product (with overflow protection)
 */
inline Temperature_t tempMulInt(Temperature_t temp, int16_t factor) {
    int32_t result = static_cast<int32_t>(temp) * static_cast<int32_t>(factor);
    // Clamp to int16_t range
    return static_cast<Temperature_t>(std::max<int32_t>(INT16_MIN, std::min<int32_t>(INT16_MAX, result)));
}

/**
 * @brief Divide temperature by integer divisor
 * @param temp Temperature value
 * @param divisor Division factor (must not be zero)
 * @return Quotient (rounded to nearest)
 */
inline Temperature_t tempDivInt(Temperature_t temp, int16_t divisor) {
    if (divisor == 0) return temp; // Avoid division by zero
    
    // Round to nearest instead of truncating
    int32_t dividend = static_cast<int32_t>(temp);
    int32_t result = (dividend + divisor / 2) / divisor;
    
    return static_cast<Temperature_t>(result);
}

/**
 * @brief Calculate average of two temperatures
 * @param a First temperature
 * @param b Second temperature
 * @return Average temperature
 */
inline Temperature_t tempAverage(Temperature_t a, Temperature_t b) {
    // Avoid overflow by dividing first, then adding the remainder
    return static_cast<Temperature_t>((static_cast<int32_t>(a) + static_cast<int32_t>(b)) / 2);
}

/**
 * @brief Calculate temperature difference (absolute value)
 * @param a First temperature
 * @param b Second temperature
 * @return Absolute difference
 */
inline Temperature_t tempDiff(Temperature_t a, Temperature_t b) {
    int32_t diff = static_cast<int32_t>(a) - static_cast<int32_t>(b);
    return static_cast<Temperature_t>(diff < 0 ? -diff : diff);
}

/**
 * @brief Scale temperature by percentage (0-100)
 * @param temp Temperature value
 * @param percent Percentage (0-100)
 * @return Scaled temperature
 */
inline Temperature_t tempScalePercent(Temperature_t temp, uint8_t percent) {
    if (percent > 100) percent = 100;
    
    // Multiply by percent and divide by 100, with rounding
    int32_t result = (static_cast<int32_t>(temp) * percent + 50) / 100;
    
    return static_cast<Temperature_t>(result);
}

/**
 * @brief Clamp temperature to range
 * @param temp Temperature value
 * @param min Minimum allowed value
 * @param max Maximum allowed value
 * @return Clamped temperature
 */
inline Temperature_t tempClamp(Temperature_t temp, Temperature_t min, Temperature_t max) {
    if (temp < min) return min;
    if (temp > max) return max;
    return temp;
}

/**
 * @brief Linear interpolation between two temperatures
 * @param temp1 First temperature
 * @param temp2 Second temperature
 * @param fraction Interpolation fraction (0-1000, representing 0.0-1.0)
 * @return Interpolated temperature
 */
inline Temperature_t tempInterpolate(Temperature_t temp1, Temperature_t temp2, uint16_t fraction) {
    if (fraction >= 1000) return temp2;
    if (fraction == 0) return temp1;
    
    int32_t diff = static_cast<int32_t>(temp2) - static_cast<int32_t>(temp1);
    int32_t result = static_cast<int32_t>(temp1) + (diff * fraction + 500) / 1000;
    
    return static_cast<Temperature_t>(result);
}

#endif // TEMPERATURE_ARITHMETIC_H