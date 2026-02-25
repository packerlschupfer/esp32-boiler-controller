// src/utils/FailOpenMonitor.h
#ifndef FAIL_OPEN_MONITOR_H
#define FAIL_OPEN_MONITOR_H

#include <cstdint>
#include <atomic>

/**
 * @brief Monitors fail-open safety checks (passing without sensors)
 *
 * IMPROVEMENT 3 (simplified): Silent Failure Detection
 * Tracks and alerts on fail-open events without enforcing hard time limits.
 *
 * Fail-open patterns occur when safety checks pass by design (assuming safe)
 * when sensors are unavailable. Examples:
 * - Water flow detection: Assumes OK if temp sensors invalid
 * - Pressure monitoring: Skipped if pressure sensor unavailable
 * - Hardware interlocks: Stub always returns true
 *
 * This monitor provides visibility into degraded operation without blocking.
 */
class FailOpenMonitor {
public:
    enum class CheckType {
        WATER_FLOW,
        PRESSURE_SENSOR,
        HARDWARE_INTERLOCKS,
        FLAME_SENSOR
    };

    /**
     * @brief Initialize monitoring
     */
    static void initialize();

    /**
     * @brief Record fail-open event (safety check passed without sensor)
     * @param type Type of check that failed open
     * @param reason Human-readable reason for fail-open
     */
    static void recordFailOpen(CheckType type, const char* reason);

    /**
     * @brief Record normal operation (sensor available)
     * @param type Type of check operating normally
     */
    static void recordNormalOperation(CheckType type);

    /**
     * @brief Publish degraded operation status to MQTT
     */
    static void publishDegradedStatus();

    /**
     * @brief Get consecutive fail-open count for diagnostics
     * @param type Type of check to query
     * @return Number of consecutive fail-opens
     */
    static uint32_t getConsecutiveCount(CheckType type);

private:
    struct FailOpenStats {
        std::atomic<uint32_t> consecutiveFailOpens{0};
        std::atomic<uint32_t> totalFailOpens{0};
        std::atomic<uint32_t> firstFailOpenTime{0};
        std::atomic<bool> mqttAlertSent{false};
    };

    static constexpr uint32_t CONSECUTIVE_ALERT_THRESHOLD = 10;  // Alert after 10 consecutive

    static FailOpenStats stats[4];  // One per CheckType
    static const char* checkTypeNames[4];

    /**
     * @brief Get stats for a check type
     */
    static FailOpenStats& getStats(CheckType type);

    /**
     * @brief Convert CheckType to string for logging
     */
    static const char* checkTypeToString(CheckType type);
};

#endif // FAIL_OPEN_MONITOR_H
