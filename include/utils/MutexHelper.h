// include/utils/MutexHelper.h
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "LoggingMacros.h"

/**
 * @brief Result of a mutex operation
 */
enum class MutexResult {
    SUCCESS,
    TIMEOUT,
    INVALID
};

/**
 * @brief RAII wrapper for safe mutex handling with timeout
 */
class ScopedMutexLock {
private:
    SemaphoreHandle_t mutex_;
    bool acquired_;
    const char* tag_;
    
public:
    ScopedMutexLock(SemaphoreHandle_t mutex, TickType_t timeout, const char* tag) 
        : mutex_(mutex), acquired_(false), tag_(tag) {
        if (mutex_ != nullptr) {
            acquired_ = (xSemaphoreTake(mutex_, timeout) == pdTRUE);
            if (!acquired_) {
                LOG_WARN(tag_, "Failed to acquire mutex within timeout");
            }
        } else {
            LOG_ERROR(tag_, "Attempted to lock null mutex");
        }
    }
    
    ~ScopedMutexLock() {
        if (acquired_ && mutex_ != nullptr) {
            xSemaphoreGive(mutex_);
        }
    }
    
    bool isAcquired() const { return acquired_; }
    
    MutexResult getResult() const {
        if (mutex_ == nullptr) return MutexResult::INVALID;
        return acquired_ ? MutexResult::SUCCESS : MutexResult::TIMEOUT;
    }
    
    // Prevent copying
    ScopedMutexLock(const ScopedMutexLock&) = delete;
    ScopedMutexLock& operator=(const ScopedMutexLock&) = delete;
};

/**
 * @brief Helper macro for consistent mutex handling with error handling
 * 
 * Usage:
 * SCOPED_MUTEX_LOCK(myMutex, pdMS_TO_TICKS(100), "MyTag") {
 *     // Critical section code
 * } else {
 *     // Handle timeout
 * }
 */
#define SCOPED_MUTEX_LOCK(mutex, timeout, tag) \
    if (ScopedMutexLock _lock(mutex, timeout, tag); _lock.isAcquired())

/**
 * @brief Take mutex with consistent error handling
 * @return true if mutex was acquired, false on timeout or error
 */
inline bool takeMutexSafe(SemaphoreHandle_t mutex, TickType_t timeout, const char* tag) {
    if (mutex == nullptr) {
        LOG_ERROR(tag, "Attempted to take null mutex");
        return false;
    }
    
    if (xSemaphoreTake(mutex, timeout) != pdTRUE) {
        LOG_WARN(tag, "Failed to acquire mutex within %lu ms", timeout / portTICK_PERIOD_MS);
        return false;
    }
    
    return true;
}

/**
 * @brief Give mutex with null check
 */
inline void giveMutexSafe(SemaphoreHandle_t mutex, const char* tag) {
    if (mutex == nullptr) {
        LOG_ERROR(tag, "Attempted to give null mutex");
        return;
    }
    xSemaphoreGive(mutex);
}