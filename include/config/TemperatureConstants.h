// include/config/TemperatureConstants.h
#ifndef TEMPERATURE_CONSTANTS_H
#define TEMPERATURE_CONSTANTS_H

// Temperature conversion constants that can be included by Temperature.h
// These are separated to avoid circular dependencies with SystemConstants.h

namespace TemperatureConstants {
    // Temperature conversion constants
    constexpr float TEMP_SCALE_FACTOR = 10.0f;
    constexpr float TEMP_ROUNDING_POSITIVE = 0.5f;
    constexpr float TEMP_ROUNDING_NEGATIVE = -0.5f;
    constexpr float TEMP_MAX_FLOAT = 3276.7f;
    constexpr float TEMP_MIN_FLOAT = -3276.8f;
}

#endif // TEMPERATURE_CONSTANTS_H