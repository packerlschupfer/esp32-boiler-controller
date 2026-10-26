// src/utils/MutexRetryHelper.cpp
#include "utils/MutexRetryHelper.h"
#include "config/SystemConstants.h"
#include <Arduino.h>

const char* MutexRetryHelper::TAG = "MutexRetry";

// Static member initialization
MutexRetryHelper::MutexTracker MutexRetryHelper::trackers[MAX_TRACKED_MUTEXES] = {};
SemaphoreHandle_t MutexRetryHelper::trackerMutex = nullptr;
bool MutexRetryHelper::initialized = false;

void MutexRetryHelper::initialize() {
    if (initialized) return;

    trackerMutex = xSemaphoreCreateMutex();
    if (trackerMutex) {
        initialized = true;
    }
}

MutexRetryHelper::MutexTracker* MutexRetryHelper::getTracker(
    SemaphoreHandle_t mutex, bool createIfMissing
) {
    if (!initialized) {
        initialize();
    }

    // Find existing tracker
    for (size_t i = 0; i < MAX_TRACKED_MUTEXES; i++) {
        if (trackers[i].mutex == mutex) {
            return &trackers[i];
        }
    }

    // Create new tracker if requested
    if (createIfMissing) {
        for (size_t i = 0; i < MAX_TRACKED_MUTEXES; i++) {
            if (trackers[i].mutex == nullptr) {
                trackers[i].mutex = mutex;
                trackers[i].consecutiveFailures = 0;
                trackers[i].lastFailureTime = 0;
                trackers[i].escalated = false;
                return &trackers[i];
            }
        }
    }

    return nullptr;  // No space available
}

MutexRetryHelper::AcquireResult MutexRetryHelper::acquire(
    SemaphoreHandle_t mutex,
    const char* name,
    TickType_t timeout,
    const RetryConfig& config
) {
    AcquireResult result;

    if (!mutex) {
        LOG_ERROR(TAG, "Null mutex for '%s'", name);
        return result;
    }

    // Try to acquire with retries
    for (uint8_t attempt = 0; attempt <= config.maxRetries; attempt++) {
        result.attemptsUsed = attempt + 1;

        if (xSemaphoreTake(mutex, timeout) == pdTRUE) {
            result.acquired = true;

            // Success - reset failure tracking
            if (attempt > 0) {
                // Log if we needed retries
                LOG_DEBUG(TAG, "Mutex '%s' acquired after %d attempts", name, attempt + 1);
            }

            // Reset consecutive failure count on success
            if (trackerMutex && xSemaphoreTake(trackerMutex, pdMS_TO_TICKS(SystemConstants::Timing::MUTEX_TRACKER_TIMEOUT_MS)) == pdTRUE) {
                MutexTracker* tracker = getTracker(mutex, false);
                if (tracker) {
                    tracker->consecutiveFailures = 0;
                    tracker->escalated = false;
                }
                xSemaphoreGive(trackerMutex);
            }

            return result;
        }

        // Failed this attempt
        if (attempt < config.maxRetries) {
            // Will retry - small delay to let other task release
            vTaskDelay(config.retryDelayTicks);

            if (config.logFailures && attempt > 0) {
                LOG_WARN(TAG, "Mutex '%s' retry %d/%d", name, attempt + 1, config.maxRetries);
            }
        }
    }

    // All retries exhausted - track this failure
    if (config.logFailures) {
        LOG_WARN(TAG, "Mutex '%s' acquisition FAILED after %d attempts",
                 name, result.attemptsUsed);
    }

    // Update failure tracking
    if (trackerMutex && xSemaphoreTake(trackerMutex, pdMS_TO_TICKS(SystemConstants::Timing::MUTEX_TRACKER_TIMEOUT_MS)) == pdTRUE) {
        MutexTracker* tracker = getTracker(mutex, true);
        if (tracker) {
            tracker->consecutiveFailures++;
            tracker->lastFailureTime = millis();

            // Check for escalation
            if (tracker->consecutiveFailures >= config.escalationThreshold && !tracker->escalated) {
                tracker->escalated = true;
                result.escalated = true;
                escalateFailure(name, tracker->consecutiveFailures);
            }
        }
        xSemaphoreGive(trackerMutex);
    }

    return result;
}

MutexRetryHelper::Guard MutexRetryHelper::acquireGuard(
    SemaphoreHandle_t mutex,
    const char* name,
    TickType_t timeout,
    const RetryConfig& config
) {
    AcquireResult result = acquire(mutex, name, timeout, config);
    return Guard(mutex, result.acquired);
}

void MutexRetryHelper::resetFailureCount(SemaphoreHandle_t mutex) {
    if (!initialized || !trackerMutex) return;

    if (xSemaphoreTake(trackerMutex, pdMS_TO_TICKS(SystemConstants::Timing::MUTEX_TRACKER_TIMEOUT_MS)) == pdTRUE) {
        MutexTracker* tracker = getTracker(mutex, false);
        if (tracker) {
            tracker->consecutiveFailures = 0;
            tracker->escalated = false;
        }
        xSemaphoreGive(trackerMutex);
    }
}

uint8_t MutexRetryHelper::getFailureCount(SemaphoreHandle_t mutex) {
    if (!initialized || !trackerMutex) return 0;

    uint8_t count = 0;
    if (xSemaphoreTake(trackerMutex, pdMS_TO_TICKS(SystemConstants::Timing::MUTEX_TRACKER_TIMEOUT_MS)) == pdTRUE) {
        MutexTracker* tracker = getTracker(mutex, false);
        if (tracker) {
            count = tracker->consecutiveFailures;
        }
        xSemaphoreGive(trackerMutex);
    }
    return count;
}

bool MutexRetryHelper::hasEscalatedFailures() {
    if (!initialized || !trackerMutex) return false;

    bool hasEscalated = false;
    if (xSemaphoreTake(trackerMutex, pdMS_TO_TICKS(SystemConstants::Timing::MUTEX_TRACKER_TIMEOUT_MS)) == pdTRUE) {
        for (size_t i = 0; i < MAX_TRACKED_MUTEXES; i++) {
            if (trackers[i].mutex != nullptr && trackers[i].escalated) {
                hasEscalated = true;
                break;
            }
        }
        xSemaphoreGive(trackerMutex);
    }
    return hasEscalated;
}

void MutexRetryHelper::escalateFailure(const char* name, uint8_t failures) {
    // Log the escalation
    LOG_ERROR(TAG, "MUTEX CONTENTION: '%s' - %d consecutive failures, escalating to HealthMonitor",
              name, failures);

    // Set event bit for system-wide monitoring
    EventGroupHandle_t errorGroup = SRP::getErrorNotificationEventGroup();
    if (errorGroup) {
        xEventGroupSetBits(errorGroup, SystemEvents::GeneralSystem::MUTEX_CONTENTION);
    }

    // Report to HealthMonitor
    HealthMonitor* healthMonitor = SRP::getHealthMonitor();
    if (healthMonitor) {
        // Use CONTROL subsystem as mutex issues usually indicate control loop problems
        healthMonitor->recordError(
            HealthMonitor::Subsystem::CONTROL,
            SystemError::MUTEX_TIMEOUT
        );
    }
}
