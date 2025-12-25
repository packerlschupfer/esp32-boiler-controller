#ifndef TEMPERATURE_H
#define TEMPERATURE_H

#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "config/TemperatureConstants.h"

// Fixed-point temperature type (tenths of degrees Celsius)
// Range: -3276.8°C to +3276.7°C with 0.1°C precision
typedef int16_t Temperature_t;

// Special values
constexpr Temperature_t TEMP_INVALID = INT16_MIN;      // -32768
constexpr Temperature_t TEMP_UNKNOWN = INT16_MIN + 1;  // -32767

// Conversion functions
inline Temperature_t tempFromFloat(float f) {
    if (std::isnan(f) || std::isinf(f)) return TEMP_INVALID;
    if (f > TemperatureConstants::TEMP_MAX_FLOAT) return 32767;   // Clamp to max
    if (f < TemperatureConstants::TEMP_MIN_FLOAT) return -32768;  // Clamp to min
    return static_cast<Temperature_t>(
        f * TemperatureConstants::TEMP_SCALE_FACTOR + 
        (f >= 0 ? TemperatureConstants::TEMP_ROUNDING_POSITIVE 
                : TemperatureConstants::TEMP_ROUNDING_NEGATIVE)
    );
}

inline float tempToFloat(Temperature_t t) {
    if (t == TEMP_INVALID) return NAN;
    return t / TemperatureConstants::TEMP_SCALE_FACTOR;
}

// Formatting helper - returns number of characters written
inline int formatTemp(char* buf, size_t size, Temperature_t t) {
    if (t == TEMP_INVALID) {
        return snprintf(buf, size, "N/A");
    } else {
        int whole = t / 10;
        int frac = abs(t % 10);
        // Handle the special case where -1 < temp < 0
        if (t < 0 && whole == 0) {
            return snprintf(buf, size, "-0.%d", frac);
        }
        return snprintf(buf, size, "%d.%d", whole, frac);
    }
}

// Temperature math helpers
inline Temperature_t tempAdd(Temperature_t a, Temperature_t b) {
    if (a == TEMP_INVALID || b == TEMP_INVALID) return TEMP_INVALID;
    int32_t result = static_cast<int32_t>(a) + static_cast<int32_t>(b);
    if (result > 32767) return 32767;
    if (result < -32768) return -32768;
    return static_cast<Temperature_t>(result);
}

inline Temperature_t tempSub(Temperature_t a, Temperature_t b) {
    if (a == TEMP_INVALID || b == TEMP_INVALID) return TEMP_INVALID;
    int32_t result = static_cast<int32_t>(a) - static_cast<int32_t>(b);
    if (result > 32767) return 32767;
    if (result < -32768) return -32768;
    return static_cast<Temperature_t>(result);
}

inline Temperature_t tempAbs(Temperature_t t) {
    if (t == TEMP_INVALID) return TEMP_INVALID;
    // Handle INT16_MIN specially to avoid overflow (abs(-32768) doesn't fit in int16_t)
    if (t == -32768) return 32767;  // Clamp to max representable value
    return t < 0 ? static_cast<Temperature_t>(-t) : t;
}

// Comparison helpers (for readability)
inline bool tempIsValid(Temperature_t t) {
    return t != TEMP_INVALID && t != TEMP_UNKNOWN;
}

inline bool tempGreater(Temperature_t a, Temperature_t b) {
    if (!tempIsValid(a) || !tempIsValid(b)) return false;
    return a > b;
}

inline bool tempLess(Temperature_t a, Temperature_t b) {
    if (!tempIsValid(a) || !tempIsValid(b)) return false;
    return a < b;
}

inline bool tempGreaterOrEqual(Temperature_t a, Temperature_t b) {
    if (!tempIsValid(a) || !tempIsValid(b)) return false;
    return a >= b;
}

inline bool tempLessOrEqual(Temperature_t a, Temperature_t b) {
    if (!tempIsValid(a) || !tempIsValid(b)) return false;
    return a <= b;
}

// Utility to create temperature from whole degrees
inline Temperature_t tempFromWhole(int degrees) {
    return static_cast<Temperature_t>(degrees * 10);
}

// Logging helper macro
#define LOG_TEMP(buf, temp) formatTemp(buf, sizeof(buf), temp)

#endif // TEMPERATURE_H