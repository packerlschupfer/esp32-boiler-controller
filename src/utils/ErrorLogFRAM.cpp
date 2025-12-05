// src/utils/ErrorLogFRAM.cpp
// FRAM-based error logging - simplified implementation
#include "ErrorLogFRAM.h"
#include "LoggingMacros.h"
#include "core/SystemResourceProvider.h"
#include "utils/ErrorHandler.h"
#include <ArduinoJson.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Static members
rtstorage::RuntimeStorage* ErrorLogFRAM::storage_ = nullptr;
bool ErrorLogFRAM::initialized_ = false;
ErrorLogFRAM::ErrorStats ErrorLogFRAM::cachedStats_;

// Constants
static const char* TAG = "ErrorLogFRAM";

// Thread protection for static buffer
static SemaphoreHandle_t bufferMutex_ = nullptr;
static constexpr TickType_t MUTEX_TIMEOUT = pdMS_TO_TICKS(50);

// Temporary in-memory storage for error details (protected by bufferMutex_)
static ErrorLogFRAM::ErrorEntry tempErrors[10];
static size_t errorCount = 0;
static size_t errorWriteIndex = 0;

// Helper to initialize mutex
static void initMutex() {
    if (bufferMutex_ == nullptr) {
        bufferMutex_ = xSemaphoreCreateMutex();
    }
}

bool ErrorLogFRAM::begin(rtstorage::RuntimeStorage* storage) {
    if (!storage) {
        LOG_ERROR(TAG, "No storage provided");
        return false;
    }

    // Initialize mutex for thread safety
    initMutex();

    storage_ = storage;

    // Initialize cache (protected by mutex)
    if (bufferMutex_ && xSemaphoreTake(bufferMutex_, MUTEX_TIMEOUT) == pdTRUE) {
        memset(&cachedStats_, 0, sizeof(cachedStats_));
        memset(tempErrors, 0, sizeof(tempErrors));
        errorCount = 0;
        errorWriteIndex = 0;
        xSemaphoreGive(bufferMutex_);
    }

    initialized_ = true;
    LOG_INFO(TAG, "Initialized - using event log for error tracking");

    return true;
}

void ErrorLogFRAM::logError(SystemError error, const char* message, const char* context) {
    if (!initialized_ || !storage_) {
        return;
    }

    uint32_t errorCode = static_cast<uint32_t>(error);
    uint32_t timestamp = millis();

    // Log as event to FRAM (storage has its own thread safety)
    rtstorage::Event event;
    event.timestamp = timestamp;
    event.type = rtstorage::EVENT_ERROR;
    event.subtype = (errorCode >> 8) & 0xFF;  // High byte of error code
    event.data = errorCode & 0xFFFF;          // Low 16 bits of error code

    (void)storage_->logEvent(event);

    // Store in temporary buffer for retrieval (protected by mutex)
    if (bufferMutex_ && xSemaphoreTake(bufferMutex_, MUTEX_TIMEOUT) == pdTRUE) {
        ErrorEntry& entry = tempErrors[errorWriteIndex];
        entry.timestamp = timestamp;
        entry.errorCode = errorCode;
        entry.count = 1;

        if (message) {
            strncpy(entry.message, message, sizeof(entry.message) - 1);
            entry.message[sizeof(entry.message) - 1] = '\0';
        } else {
            entry.message[0] = '\0';
        }

        if (context) {
            strncpy(entry.context, context, sizeof(entry.context) - 1);
            entry.context[sizeof(entry.context) - 1] = '\0';
        } else {
            entry.context[0] = '\0';
        }

        errorWriteIndex = (errorWriteIndex + 1) % 10;
        if (errorCount < 10) errorCount++;

        // Update stats
        cachedStats_.totalErrors++;
        cachedStats_.lastErrorTime = timestamp;
        if (cachedStats_.oldestErrorTime == 0) {
            cachedStats_.oldestErrorTime = timestamp;
        }

        xSemaphoreGive(bufferMutex_);
    }

    // Log locally for debugging (outside mutex)
    LOG_ERROR(TAG, "Error %lu: %s (%s)",
              errorCode,
              message ? message : "No message",
              context ? context : "No context");
}

void ErrorLogFRAM::logCriticalError(SystemError error, const char* message, const char* context) {
    if (!initialized_ || !storage_) {
        return;
    }

    // Log as error first (handles its own locking)
    logError(error, message, context);

    // Update critical count (protected by mutex)
    if (bufferMutex_ && xSemaphoreTake(bufferMutex_, MUTEX_TIMEOUT) == pdTRUE) {
        cachedStats_.criticalErrors++;
        xSemaphoreGive(bufferMutex_);
    }

    uint32_t errorCode = static_cast<uint32_t>(error);

    // Log as system event too for critical errors
    rtstorage::Event event;
    event.timestamp = millis();
    event.type = rtstorage::EVENT_SYSTEM;
    event.subtype = 0xFF;  // Critical marker
    event.data = errorCode & 0xFFFF;

    (void)storage_->logEvent(event);

    // Log locally with high priority
    LOG_ERROR(TAG, "CRITICAL Error %lu: %s (%s)",
              errorCode,
              message ? message : "No message",
              context ? context : "No context");
}

bool ErrorLogFRAM::getError(size_t index, ErrorEntry& entry) {
    if (!initialized_) {
        return false;
    }

    bool result = false;
    if (bufferMutex_ && xSemaphoreTake(bufferMutex_, MUTEX_TIMEOUT) == pdTRUE) {
        if (index < errorCount) {
            // Get from temporary buffer
            size_t actualIndex = (errorWriteIndex - 1 - index + 10) % 10;
            entry = tempErrors[actualIndex];
            result = true;
        }
        xSemaphoreGive(bufferMutex_);
    }

    return result;
}

ErrorLogFRAM::ErrorStats ErrorLogFRAM::getStats() {
    if (!initialized_) {
        return ErrorStats();
    }

    ErrorStats stats;
    if (bufferMutex_ && xSemaphoreTake(bufferMutex_, MUTEX_TIMEOUT) == pdTRUE) {
        stats = cachedStats_;
        xSemaphoreGive(bufferMutex_);
    }

    return stats;
}

void ErrorLogFRAM::clear() {
    if (!initialized_ || !storage_) {
        return;
    }

    // Clear event log
    (void)storage_->clearEvents();

    // Reset local cache (protected by mutex)
    if (bufferMutex_ && xSemaphoreTake(bufferMutex_, MUTEX_TIMEOUT) == pdTRUE) {
        memset(tempErrors, 0, sizeof(tempErrors));
        memset(&cachedStats_, 0, sizeof(cachedStats_));
        errorCount = 0;
        errorWriteIndex = 0;
        xSemaphoreGive(bufferMutex_);
    }

    LOG_INFO(TAG, "All errors cleared");
}

void ErrorLogFRAM::clearOldErrors(uint32_t daysOld) {
    if (!initialized_) {
        return;
    }
    
    LOG_WARN(TAG, "clearOldErrors not implemented");
}

size_t ErrorLogFRAM::getErrorCount() {
    if (!initialized_ || !storage_) {
        return 0;
    }
    
    // Return count from event log
    return storage_->getEventCount();
}

size_t ErrorLogFRAM::getCriticalErrors(ErrorEntry* buffer, size_t maxCount) {
    if (!initialized_ || !buffer) {
        return 0;
    }
    
    // For now, just return the most recent errors marked as critical
    size_t count = 0;
    for (size_t i = 0; i < errorCount && count < maxCount; i++) {
        ErrorEntry entry;
        if (getError(i, entry)) {
            // Check if it's a critical error type
            SystemError err = static_cast<SystemError>(entry.errorCode);
            if (err == SystemError::SYSTEM_OVERHEATED ||
                err == SystemError::SYSTEM_FAILSAFE_TRIGGERED ||
                err == SystemError::TEMPERATURE_CRITICAL ||
                err == SystemError::EMERGENCY_STOP ||
                err == SystemError::IGNITION_FAILURE) {
                buffer[count++] = entry;
            }
        }
    }
    
    return count;
}

bool ErrorLogFRAM::exportToJson(char* buffer, size_t bufferSize, size_t maxErrors) {
    if (!initialized_ || !buffer || bufferSize == 0) {
        return false;
    }
    
    // Create JSON document
    JsonDocument doc;  // ArduinoJson v7
    JsonArray errors = doc["errors"].to<JsonArray>();

    // Get error count
    size_t exportCount = min(errorCount, maxErrors);

    // Add stats
    JsonObject stats = doc["stats"].to<JsonObject>();
    stats["total"] = cachedStats_.totalErrors;
    stats["critical"] = cachedStats_.criticalErrors;
    stats["oldest"] = cachedStats_.oldestErrorTime;
    stats["latest"] = cachedStats_.lastErrorTime;

    // Add recent errors
    for (size_t i = 0; i < exportCount; i++) {
        ErrorEntry entry;
        if (getError(i, entry)) {
            JsonObject errorObj = errors.add<JsonObject>();
            errorObj["time"] = entry.timestamp;
            errorObj["code"] = entry.errorCode;
            errorObj["count"] = entry.count;
            errorObj["msg"] = entry.message;
            if (strlen(entry.context) > 0) {
                errorObj["ctx"] = entry.context;
            }
        }
    }
    
    // Serialize to buffer
    size_t written = serializeJson(doc, buffer, bufferSize);
    
    return (written > 0 && written < bufferSize);
}

void ErrorLogFRAM::updateCachedStats() {
    // Stats are updated inline in this implementation
}