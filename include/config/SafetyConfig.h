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
        // 30 s, tightened from 60 s on 2026-10-25 (user decision, "tighten the ship").
        // 60 s was set when the room sensor was the unreliable part; with the replaced ANDRTF3
        // on its own firmware that margin is no longer needed. The MB8ART is read every 2.5 s,
        // so 30 s is still 12 missed reads before anything is called stale, and the room
        // sensor's own driver flags a reading invalid after 3 failed reads (~15 s) regardless.
        // NOT the single threshold for "too old to act on" - that claim was wrong (2026-10-26
        // review). There are TWO, and the split is deliberate:
        //   - SystemConstants::Safety::SENSOR_STALE_THRESHOLD_MS = 15 s, used by
        //     StateManager::isSensorStale() and thus by continuousSafetyMonitor()'s stale-sensor
        //     EMERGENCY STOP. Tight, because it only applies while the burner is firing.
        //   - this one, 30 s, used by TemperatureSensorFallback (mark sensors invalid) and the
        //     frost guard (is a reading usable). Looser, because it governs whether to START.
        // A burn is held to a stricter freshness bar than a decision to begin one, which is the
        // right way round. Change one and check whether the other should follow.
        constexpr uint32_t SENSOR_STALE_MS = 30000;         // 30s
        constexpr uint32_t POST_PURGE_MS = 90000;           // 90s
        constexpr uint32_t ERROR_RECOVERY_MS = 300000;      // 5min (M4: configurable error recovery delay)
        constexpr uint32_t THERMAL_SHOCK_DIFFERENTIAL_C = 450; // 45.0°C (tenths of °C) - cast iron boiler, raised from 30°C

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

        constexpr uint32_t THERMAL_SHOCK_MIN_C = 100;       // 10.0°C minimum (tenths of °C)
        constexpr uint32_t THERMAL_SHOCK_MAX_C = 600;       // 60.0°C maximum (tenths of °C)

        // M1: PID anti-windup limit ranges (fixed-point scaled by 1000)
        constexpr int32_t PID_INTEGRAL_MIN_LIMIT = -500000; // -500.0°C (absolute minimum)
        constexpr int32_t PID_INTEGRAL_MAX_LIMIT = 500000;  // +500.0°C (absolute maximum)
    }

    // Runtime values (loaded from NVS, modifiable via MQTT)
    extern uint32_t pumpProtectionMs;
    extern uint32_t sensorStaleMs;
    extern uint32_t postPurgeMs;
    extern uint32_t errorRecoveryMs;          // M4: configurable error recovery delay
    extern int32_t pidIntegralMin;            // M1: PID anti-windup lower limit
    extern int32_t pidIntegralMax;            // M1: PID anti-windup upper limit
    extern uint32_t thermalShockDifferentialC; // tenths of °C, e.g. 450 = 45.0°C

    // Initialize from NVS (call at startup)
    void loadFromNVS();

    // Save to NVS (call after MQTT update)
    void saveToNVS();

    // Validate and set (returns false if out of range)
    bool setPumpProtection(uint32_t ms);
    bool setSensorStale(uint32_t ms);
    bool setPostPurge(uint32_t ms);
    bool setErrorRecovery(uint32_t ms);        // M4: configurable error recovery delay
    bool setPIDIntegralLimits(int32_t min, int32_t max);  // M1: PID anti-windup limits
    bool setThermalShock(uint32_t tenthsC);    // tenths of °C, e.g. 450 = 45.0°C

    // Compile-time validation of defaults
    static_assert(Defaults::ERROR_RECOVERY_MS >= Limits::ERROR_RECOVERY_MIN_MS,
                  "Default error recovery must be >= minimum");
    static_assert(Limits::ERROR_RECOVERY_MIN_MS >= 20000,
                  "Error recovery minimum must be >= burner anti-flapping (20s)");
    static_assert(Defaults::PUMP_PROTECTION_MS <= Defaults::ERROR_RECOVERY_MS,
                  "Pump protection should be < error recovery for faster restart");
}
