#pragma once
#include <cstdint>

/**
 * @brief Runtime-configurable safety parameters
 *
 * These parameters can be configured via MQTT and are persisted in NVS.
 * This allows tuning safety behavior without firmware reflashing.
 */
namespace SafetyConfig {
    // Compile-time defaults
    namespace Defaults {
        constexpr uint32_t PUMP_PROTECTION_MS = 15000;      // 15s
        constexpr uint32_t SENSOR_STALE_MS = 60000;         // 60s
        constexpr uint32_t POST_PURGE_MS = 90000;           // 90s
        constexpr uint32_t ERROR_RECOVERY_MS = 300000;      // 5min (M4: configurable error recovery delay)

        // M1: PID anti-windup limits (fixed-point scaled by 1000)
        // Values represent temperature adjustment limits in °C
        constexpr int32_t PID_INTEGRAL_MIN = -100000;       // -100.0°C
        constexpr int32_t PID_INTEGRAL_MAX = 100000;        // +100.0°C
    }

    // Valid ranges
    namespace Limits {
        constexpr uint32_t PUMP_PROTECTION_MIN_MS = 5000;   // 5s
        constexpr uint32_t PUMP_PROTECTION_MAX_MS = 60000;  // 60s

        constexpr uint32_t SENSOR_STALE_MIN_MS = 30000;     // 30s
        constexpr uint32_t SENSOR_STALE_MAX_MS = 300000;    // 5min

        constexpr uint32_t POST_PURGE_MIN_MS = 30000;       // 30s
        constexpr uint32_t POST_PURGE_MAX_MS = 180000;      // 3min

        constexpr uint32_t ERROR_RECOVERY_MIN_MS = 60000;   // 1min minimum
        constexpr uint32_t ERROR_RECOVERY_MAX_MS = 1800000; // 30min maximum

        // M1: PID anti-windup limit ranges (fixed-point scaled by 1000)
        constexpr int32_t PID_INTEGRAL_MIN_LIMIT = -500000; // -500.0°C (absolute minimum)
        constexpr int32_t PID_INTEGRAL_MAX_LIMIT = 500000;  // +500.0°C (absolute maximum)
    }

    // Runtime values (loaded from NVS, modifiable via MQTT)
    extern uint32_t pumpProtectionMs;
    extern uint32_t sensorStaleMs;
    extern uint32_t postPurgeMs;
    extern uint32_t errorRecoveryMs;  // M4: configurable error recovery delay
    extern int32_t pidIntegralMin;    // M1: PID anti-windup lower limit
    extern int32_t pidIntegralMax;    // M1: PID anti-windup upper limit

    // Initialize from NVS (call at startup)
    void loadFromNVS();

    // Save to NVS (call after MQTT update)
    void saveToNVS();

    // Validate and set (returns false if out of range)
    bool setPumpProtection(uint32_t ms);
    bool setSensorStale(uint32_t ms);
    bool setPostPurge(uint32_t ms);
    bool setErrorRecovery(uint32_t ms);  // M4: configurable error recovery delay
    bool setPIDIntegralLimits(int32_t min, int32_t max);  // M1: PID anti-windup limits
}
