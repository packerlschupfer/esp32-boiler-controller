// src/utils/MutexRetryHelper.h
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "monitoring/HealthMonitor.h"
#include "core/SystemResourceProvider.h"
#include "events/SystemEventsGenerated.h"
#include "config/SystemConstants.h"
#include "LoggingMacros.h"

/**
 * @brief Helper class for mutex acquisition with retry logic and health escalation
 *
 * This class provides robust mutex acquisition that:
 * - Retries failed acquisitions with configurable delay
 * - Tracks consecutive failures per mutex
 * - Escalates to HealthMonitor after threshold failures
 * - Sets event bits for system-wide monitoring
 * - Provides useful logging for debugging deadlocks
 *
 * Usage:
 * @code
 * // Simple usage - returns true if acquired
 * if (MutexRetryHelper::acquire(myMutex, "SensorData", pdMS_TO_TICKS(100))) {
 *     // Critical section
 *     xSemaphoreGive(myMutex);
 * }
 *
 * // With RAII-style guard
 * {
 *     auto guard = MutexRetryHelper::acquireGuard(myMutex, "RelayControl");
 *     if (guard.acquired) {
 *         // Critical section - released automatically
 *     }
 * }
 * @endcode
 */
class MutexRetryHelper {
public:
    /**
     * @brief Result of a mutex acquisition attempt
     */
    struct AcquireResult {
        bool acquired = false;       ///< Whether mutex was acquired
        uint8_t attemptsUsed = 0;    ///< Number of attempts taken
        bool escalated = false;      ///< Whether failure was escalated to health monitor
    };

    /**
     * @brief RAII guard for automatic mutex release
     */
    struct Guard {
        SemaphoreHandle_t mutex;
        bool acquired;

        Guard(SemaphoreHandle_t m, bool acq) : mutex(m), acquired(acq) {}
        ~Guard() {
            if (acquired && mutex) {
                xSemaphoreGive(mutex);
            }
        }

        // Non-copyable, non-movable
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        Guard(Guard&&) = delete;
        Guard& operator=(Guard&&) = delete;

        operator bool() const { return acquired; }
    };

    /**
     * @brief Configuration for retry behavior
     */
    struct RetryConfig {
        uint8_t maxRetries;              ///< Maximum attempts
        TickType_t retryDelayTicks;      ///< Delay between retries
        uint8_t escalationThreshold;     ///< Consecutive failures before escalation
        bool logFailures;                ///< Whether to log individual failures

        // Default constructor with sensible defaults
        RetryConfig(
            uint8_t retries = 3,
            TickType_t delay = pdMS_TO_TICKS(10),
            uint8_t threshold = 5,
            bool log = true
        ) : maxRetries(retries),
            retryDelayTicks(delay),
            escalationThreshold(threshold),
            logFailures(log) {}
    };

    /**
     * @brief Acquire a mutex with retry logic
     *
     * @param mutex The mutex to acquire
     * @param name Human-readable name for logging (e.g., "SensorData", "RelayControl")
     * @param timeout Timeout per attempt
     * @param config Optional retry configuration
     * @return AcquireResult with acquisition details
     */
    static AcquireResult acquire(
        SemaphoreHandle_t mutex,
        const char* name,
        TickType_t timeout = pdMS_TO_TICKS(SystemConstants::Timing::MUTEX_DEFAULT_TIMEOUT_MS),
        const RetryConfig& config = RetryConfig()
    );

    /**
     * @brief Acquire a mutex and return an RAII guard
     *
     * @param mutex The mutex to acquire
     * @param name Human-readable name for logging
     * @param timeout Timeout per attempt
     * @param config Optional retry configuration
     * @return Guard that automatically releases mutex on destruction
     */
    static Guard acquireGuard(
        SemaphoreHandle_t mutex,
        const char* name,
        TickType_t timeout = pdMS_TO_TICKS(SystemConstants::Timing::MUTEX_DEFAULT_TIMEOUT_MS),
        const RetryConfig& config = RetryConfig()
    );

    /**
     * @brief Reset failure tracking for a mutex
     * Call this after successful operations to clear escalation state
     *
     * @param mutex The mutex to reset tracking for
     */
    static void resetFailureCount(SemaphoreHandle_t mutex);

    /**
     * @brief Get current consecutive failure count for a mutex
     *
     * @param mutex The mutex to check
     * @return Current failure count
     */
    static uint8_t getFailureCount(SemaphoreHandle_t mutex);

    /**
     * @brief Check if any mutex has exceeded escalation threshold
     *
     * @return true if system is experiencing mutex contention issues
     */
    static bool hasEscalatedFailures();

private:
    static const char* TAG;

    // Track failures per mutex (simple array for common mutexes)
    static constexpr size_t MAX_TRACKED_MUTEXES = 8;

    struct MutexTracker {
        SemaphoreHandle_t mutex = nullptr;
        uint8_t consecutiveFailures = 0;
        uint32_t lastFailureTime = 0;
        bool escalated = false;
    };

    static MutexTracker trackers[MAX_TRACKED_MUTEXES];
    static SemaphoreHandle_t trackerMutex;  // Protects tracker array
    static bool initialized;

    static void initialize();
    static MutexTracker* getTracker(SemaphoreHandle_t mutex, bool createIfMissing = true);
    static void escalateFailure(const char* name, uint8_t failures);
};
