#ifndef UTILS_H
#define UTILS_H

#include <Arduino.h>      // For millis()
#include <cstdint>
#include <cmath>          // For round function

// Namespace for utility functions
namespace Utils {

    inline float roundF(float value, uint8_t precision) {
        float pow10 = pow(10.0f, precision);
        return round(value * pow10) / pow10;
    }

    /**
     * @brief Calculate elapsed time since a start time, handling millis() overflow
     *
     * millis() overflows after ~49.7 days (2^32 milliseconds).
     * Simple subtraction (now - start) handles overflow correctly due to
     * unsigned integer wraparound behavior in C/C++.
     *
     * Example: If start=0xFFFFFFF0 and now=0x00000010,
     *          now - start = 0x00000020 (32 ms elapsed) - correct!
     *
     * @param startTime The start time captured from millis()
     * @return Elapsed time in milliseconds (always positive, max ~49.7 days)
     */
    inline uint32_t elapsedMs(uint32_t startTime) {
        return millis() - startTime;
    }

    /**
     * @brief Check if a timeout has elapsed since start time
     *
     * Handles millis() overflow correctly.
     *
     * @param startTime The start time captured from millis()
     * @param timeoutMs The timeout duration in milliseconds
     * @return true if timeout has elapsed
     */
    inline bool hasTimedOut(uint32_t startTime, uint32_t timeoutMs) {
        return elapsedMs(startTime) >= timeoutMs;
    }

}

#endif // UTILS_H
