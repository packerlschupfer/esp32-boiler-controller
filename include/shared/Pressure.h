// include/shared/Pressure.h
#ifndef SHARED_PRESSURE_H
#define SHARED_PRESSURE_H

#include <Arduino.h>
#include <cmath>
#include <limits.h>

// Fixed-point pressure type (hundredths of BAR)
// Using int16_t for consistency with Temperature_t
// Range: -327.68 BAR to +327.67 BAR with 0.01 BAR precision
typedef int16_t Pressure_t;

// Special values
constexpr Pressure_t PRESSURE_INVALID = INT16_MIN;      // -32768
constexpr Pressure_t PRESSURE_UNKNOWN = INT16_MIN + 1;  // -32767

// Pressure constants
namespace PressureConstants {
    constexpr float PRESSURE_SCALE_FACTOR = 100.0f;     // 100 hundredths per BAR
    constexpr float PRESSURE_MAX_FLOAT = 327.67f;       // Maximum representable pressure
    constexpr float PRESSURE_MIN_FLOAT = -327.68f;      // Minimum representable pressure
    constexpr float PRESSURE_ROUNDING_POSITIVE = 0.5f;  // For positive values
    constexpr float PRESSURE_ROUNDING_NEGATIVE = -0.5f; // For negative values
    
    // Common pressure values in hundredths of BAR
    constexpr Pressure_t PRESSURE_0_BAR = 0;
    constexpr Pressure_t PRESSURE_1_BAR = 100;
    constexpr Pressure_t PRESSURE_1_5_BAR = 150;
    constexpr Pressure_t PRESSURE_2_BAR = 200;
    constexpr Pressure_t PRESSURE_3_BAR = 300;
    
    // Safety limits for boiler systems (in hundredths of BAR)
    constexpr Pressure_t PRESSURE_MIN_SAFE = 50;   // 0.5 BAR minimum
    constexpr Pressure_t PRESSURE_MAX_SAFE = 250;  // 2.5 BAR maximum
    constexpr Pressure_t PRESSURE_NORMAL = 150;    // 1.5 BAR normal operating
}

// Conversion functions
inline Pressure_t pressureFromFloat(float bar) {
    if (std::isnan(bar) || std::isinf(bar)) return PRESSURE_INVALID;
    if (bar > PressureConstants::PRESSURE_MAX_FLOAT) return 32767;   // Clamp to max
    if (bar < PressureConstants::PRESSURE_MIN_FLOAT) return -32768;  // Clamp to min
    return static_cast<Pressure_t>(
        bar * PressureConstants::PRESSURE_SCALE_FACTOR + 
        (bar >= 0 ? PressureConstants::PRESSURE_ROUNDING_POSITIVE 
                  : PressureConstants::PRESSURE_ROUNDING_NEGATIVE)
    );
}

inline float pressureToFloat(Pressure_t p) {
    if (p == PRESSURE_INVALID) return NAN;
    return p / PressureConstants::PRESSURE_SCALE_FACTOR;
}

// Formatting helper - returns number of characters written
inline int formatPressure(char* buf, size_t size, Pressure_t p) {
    if (p == PRESSURE_INVALID) {
        return snprintf(buf, size, "N/A");
    } else {
        int whole = p / 100;
        int fraction = abs(p % 100);
        return snprintf(buf, size, "%d.%02d", whole, fraction);
    }
}

// Pressure math helpers
inline Pressure_t pressureAdd(Pressure_t a, Pressure_t b) {
    if (a == PRESSURE_INVALID || b == PRESSURE_INVALID) return PRESSURE_INVALID;
    int32_t result = static_cast<int32_t>(a) + static_cast<int32_t>(b);
    if (result > 32767) return 32767;
    if (result < -32768) return -32768;
    return static_cast<Pressure_t>(result);
}

inline Pressure_t pressureSub(Pressure_t a, Pressure_t b) {
    if (a == PRESSURE_INVALID || b == PRESSURE_INVALID) return PRESSURE_INVALID;
    int32_t result = static_cast<int32_t>(a) - static_cast<int32_t>(b);
    if (result > 32767) return 32767;
    if (result < -32768) return -32768;
    return static_cast<Pressure_t>(result);
}

inline Pressure_t pressureAbs(Pressure_t p) {
    if (p == PRESSURE_INVALID) return PRESSURE_INVALID;
    return static_cast<Pressure_t>(abs(static_cast<int>(p)));
}

// Comparison helpers (for readability)
inline bool pressureIsValid(Pressure_t p) {
    return p != PRESSURE_INVALID && p != PRESSURE_UNKNOWN;
}

inline bool pressureGreater(Pressure_t a, Pressure_t b) {
    if (!pressureIsValid(a) || !pressureIsValid(b)) return false;
    return a > b;
}

inline bool pressureLess(Pressure_t a, Pressure_t b) {
    if (!pressureIsValid(a) || !pressureIsValid(b)) return false;
    return a < b;
}

inline bool pressureGreaterOrEqual(Pressure_t a, Pressure_t b) {
    if (!pressureIsValid(a) || !pressureIsValid(b)) return false;
    return a >= b;
}

inline bool pressureLessOrEqual(Pressure_t a, Pressure_t b) {
    if (!pressureIsValid(a) || !pressureIsValid(b)) return false;
    return a <= b;
}

// Check if pressure is in safe operating range
inline bool pressureInSafeRange(Pressure_t p) {
    if (!pressureIsValid(p)) return false;
    return p >= PressureConstants::PRESSURE_MIN_SAFE && 
           p <= PressureConstants::PRESSURE_MAX_SAFE;
}

// Utility to create pressure from whole BAR
inline Pressure_t pressureFromWhole(int bar) {
    return static_cast<Pressure_t>(bar * 100);
}

// Logging helper macro
#define LOG_PRESSURE(level, tag, prefix, pressure) \
    do { \
        char _buf[16]; \
        formatPressure(_buf, sizeof(_buf), pressure); \
        LOG_##level(tag, "%s%s BAR", prefix, _buf); \
    } while(0)

#endif // SHARED_PRESSURE_H