/**
 * @file MockTime.h
 * @brief Common mock time functions for testing
 */

#ifndef MOCK_TIME_H
#define MOCK_TIME_H

#include <cstdint>

// Global mock time variable
extern uint32_t g_mockMillis;

// Mock millis function
inline uint32_t millis() {
    return g_mockMillis;
}

// Helper to set mock time
inline void setMockMillis(uint32_t time) {
    g_mockMillis = time;
}

// Helper to advance mock time
inline void advanceMockMillis(uint32_t delta) {
    g_mockMillis += delta;
}

#endif // MOCK_TIME_H