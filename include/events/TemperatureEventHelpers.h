// Temperature_t-aware helper functions for event system
// Provides type-safe temperature encoding/decoding for event bits

#pragma once

#include "events/SystemEventsGenerated.h"
#include "shared/Temperature.h"

namespace SystemEvents {
namespace BurnerRequest {

/**
 * Encode a Temperature_t value into event bits
 * Converts from Temperature_t (tenths of degrees) to whole degrees for encoding
 * 
 * @param temp Temperature in Temperature_t format (tenths)
 * @return Encoded event bits ready for OR'ing into request bits
 */
inline constexpr EventBits_t encode_temperature_t(Temperature_t temp) {
    // Convert from tenths to whole degrees, then encode
    // M14: Use rounding instead of truncation for better precision
    // 234 (23.4°C) → 23°C, 235 (23.5°C) → 24°C
    // Clamp negative values to 0 (burner targets should always be positive)
    if (temp < 0) temp = 0;
    uint32_t wholeDegrees = static_cast<uint32_t>((temp + 5) / 10);  // Round to nearest
    return encode_temperature(wholeDegrees);
}

/**
 * Decode temperature from event bits to Temperature_t
 * Converts from encoded whole degrees to Temperature_t (tenths)
 * 
 * @param bits Event bits containing encoded temperature
 * @return Temperature in Temperature_t format (tenths)
 */
inline Temperature_t decode_temperature_t(EventBits_t bits) {
    // Decode to whole degrees, then convert to Temperature_t (tenths)
    uint32_t wholeDegrees = decode_temperature(bits);
    return tempFromWhole(static_cast<int>(wholeDegrees));
}

/**
 * Helper to set temperature in existing event bits
 * Clears old temperature and sets new one
 * 
 * @param bits Existing event bits
 * @param temp New temperature to encode
 * @return Updated event bits with new temperature
 */
inline EventBits_t set_temperature_t(EventBits_t bits, Temperature_t temp) {
    // Clear old temperature bits and set new ones
    return (bits & ~TEMPERATURE_MASK) | encode_temperature_t(temp);
}

/**
 * Helper to extract just the temperature from event bits
 * 
 * @param bits Event bits to extract temperature from
 * @return Temperature in Temperature_t format, or TEMP_INVALID if none set
 */
inline Temperature_t get_temperature_t(EventBits_t bits) {
    uint32_t wholeDegrees = decode_temperature(bits);
    if (wholeDegrees == 0) {
        // Check if this is actually 0°C or just unset
        // If no request bits are set, assume unset
        if (!(bits & ANY_REQUEST)) {
            return TEMP_INVALID;
        }
    }
    return tempFromWhole(static_cast<int>(wholeDegrees));
}

} // namespace BurnerRequest
} // namespace SystemEvents