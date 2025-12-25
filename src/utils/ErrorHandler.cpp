// src/utils/ErrorHandler.cpp
#include "ErrorHandler.h"
#include "ErrorLogFRAM.h"
#include "modules/control/CentralizedFailsafe.h"
#include "modules/control/BurnerSystemController.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <esp_system.h>
#include <SemaphoreGuard.h>
#include <MQTTManager.h>
#include "core/SystemResourceProvider.h"
#include "config/SystemConstants.h"
#include <algorithm>  // For std::min

#include "events/SystemEventsGenerated.h"

// Rate limiting data structure - shared between logError and clearErrorRateLimit
namespace {
    struct ErrorRateLimit {
        SystemError error;
        uint32_t lastLogTime;
        uint32_t logInterval;
        uint32_t count;
    };

    static ErrorRateLimit rateLimits[5] = {};  // Track up to 5 different errors
    static constexpr uint32_t INITIAL_INTERVAL = SystemConstants::ErrorLogging::RATE_LIMIT_INITIAL_INTERVAL_MS;
    static constexpr uint32_t MAX_INTERVAL = SystemConstants::ErrorLogging::RATE_LIMIT_MAX_INTERVAL_MS;
}

void ErrorHandler::enterFailsafeMode(SystemError reason) {
    LOG_ERROR("FAILSAFE", "Entering failsafe mode due to: %s", errorToString(reason));
    
    // Use centralized failsafe system
    CentralizedFailsafe::triggerFailsafe(CentralizedFailsafe::FailsafeLevel::CRITICAL, reason);
    
    // Legacy code for backward compatibility
    // 1. Emergency shutdown of critical systems
    auto* burnerController = SRP::getBurnerSystemController();
    if (burnerController != nullptr) {
        LOG_INFO("FAILSAFE", "Shutting down burner");
        auto result = burnerController->emergencyShutdown("Failsafe mode triggered");
        if (result.isError()) {
            LOG_ERROR("FAILSAFE", "emergencyShutdown() failed - proceeding with relay control");
        }
    }
    
    // 2. Set all relays to safe position
    LOG_INFO("FAILSAFE", "Setting relays to safe position");
    // Clear all relay control bits to ensure safe state
    // Note: FreeRTOS event groups only support 24 bits (0x00FFFFFF)
    SRP::clearRelayEventBits(0x00FFFFFF);
    
    // 3. Set system state to failsafe
    // Set system state to degraded mode (failsafe)
    SRP::setSystemStateEventBits(SystemEvents::SystemState::DEGRADED_MODE);
    
    // 4. Send alert via MQTT if possible
    MQTTManager* mqttManager = SRP::getMQTTManager();
    if (mqttManager != nullptr && mqttManager->isConnected()) {
        SemaphoreGuard guard(SRP::getMQTTMutex(), pdMS_TO_TICKS(100));
        if (guard.hasLock()) {
            char message[128];
            snprintf(message, sizeof(message), 
                    "{\"event\":\"failsafe\",\"reason\":\"%s\",\"code\":%lu}",
                    errorToString(reason), static_cast<unsigned long>(reason));
            (void)mqttManager->publish("alert/critical", message, 1, true);
        }
    }
    
    // 5. Log to persistent storage
    ErrorLogFRAM::logCriticalError(reason, "System entered failsafe mode");
    
    // 6. Disable non-critical operations
    LOG_INFO("FAILSAFE", "Failsafe mode active - manual intervention required");
    
    // Note: System should remain in failsafe until manual reset
}

bool ErrorHandler::attemptMemoryRecovery() {
    LOG_WARN("MEMORY", "Attempting memory recovery");

    // 1. Log current memory status (before recovery)
    size_t freeHeapBefore = ESP.getFreeHeap();
    size_t minFreeHeap = ESP.getMinFreeHeap();
    size_t maxAllocHeap = ESP.getMaxAllocHeap();

    LOG_INFO("MEMORY", "Before: Free=%d, MinFree=%d, MaxAlloc=%d",
             freeHeapBefore, minFreeHeap, maxAllocHeap);

    // 2. Request garbage collection from tasks
    // Signal memory error event for tasks to clean up non-critical buffers
    xEventGroupSetBits(SRP::getErrorNotificationEventGroup(),
                      SystemEvents::Error::MEMORY);

    // 3. Reduce logging verbosity
    #if defined(LOG_MODE_DEBUG_FULL) || defined(LOG_MODE_DEBUG_SELECTIVE)
    LOG_WARN("MEMORY", "Reducing log verbosity to save memory");
    esp_log_level_set("*", ESP_LOG_WARN);
    #endif

    // 4. Allow time for tasks to respond to cleanup request
    vTaskDelay(pdMS_TO_TICKS(100));

    // 5. Re-check memory after recovery actions
    size_t freeHeapAfter = ESP.getFreeHeap();
    int32_t recovered = static_cast<int32_t>(freeHeapAfter) - static_cast<int32_t>(freeHeapBefore);

    LOG_INFO("MEMORY", "After: Free=%d, Recovered=%d bytes", freeHeapAfter, recovered);

    // 6. Determine recovery success
    bool success = (freeHeapAfter >= SystemConstants::System::MIN_FREE_HEAP_WARNING);

    if (!success) {
        LOG_ERROR("MEMORY", "Memory recovery FAILED - still at %d bytes (need %d)",
                 freeHeapAfter, SystemConstants::System::MIN_FREE_HEAP_WARNING);

        // If critically low, enter reduced functionality mode
        if (freeHeapAfter < SystemConstants::System::MIN_FREE_HEAP_CRITICAL) {
            LOG_ERROR("MEMORY", "Critical memory level - entering reduced mode");
            // Trigger degraded mode for safety
            CentralizedFailsafe::triggerFailsafe(
                CentralizedFailsafe::FailsafeLevel::DEGRADED,
                SystemError::SYSTEM_LOW_MEMORY,
                "Memory recovery failed - system degraded"
            );
        }
    } else {
        LOG_INFO("MEMORY", "Memory recovery successful - %d bytes free", freeHeapAfter);
        // Clear memory error event bit
        xEventGroupClearBits(SRP::getErrorNotificationEventGroup(),
                            SystemEvents::Error::MEMORY);
    }

    // 7. Send memory alert via MQTT
    MQTTManager* mqttManager = SRP::getMQTTManager();
    if (mqttManager != nullptr && mqttManager->isConnected()) {
        SemaphoreGuard guard(SRP::getMQTTMutex(), pdMS_TO_TICKS(100));
        if (guard.hasLock()) {
            char message[160];
            snprintf(message, sizeof(message),
                    "{\"event\":\"memory_recovery\",\"success\":%s,\"free\":%d,\"recovered\":%ld}",
                    success ? "true" : "false", freeHeapAfter, (long)recovered);
            (void)mqttManager->publish("alert/warning", message, 0, false);
        }
    }

    return success;
}

void ErrorHandler::logError(const char* tag, SystemError error, const char* context) {
    // Rate limiting for repetitive errors (uses file-scope rateLimits array)

    // Find or create rate limit entry for this error
    ErrorRateLimit* rateLimit = nullptr;
    for (int i = 0; i < 5; i++) {
        if (rateLimits[i].error == error || rateLimits[i].error == SystemError::SUCCESS) {
            rateLimit = &rateLimits[i];
            if (rateLimit->error == SystemError::SUCCESS) {
                rateLimit->error = error;
                rateLimit->logInterval = INITIAL_INTERVAL;
                rateLimit->lastLogTime = 0;
                rateLimit->count = 0;
            }
            break;
        }
    }
    
    // If no slot available, always log (shouldn't happen with 5 slots)
    if (!rateLimit) {
        if (context) {
            LOG_ERROR(tag, "%s: %s (code: %d)", context, errorToString(error), 
                     static_cast<uint32_t>(error));
        } else {
            LOG_ERROR(tag, "Error: %s (code: %d)", errorToString(error), 
                     static_cast<uint32_t>(error));
        }
        ErrorLogFRAM::logError(error, nullptr, context);
        return;
    }
    
    // Check if enough time has passed
    uint32_t now = millis();
    if (now - rateLimit->lastLogTime >= rateLimit->logInterval) {
        rateLimit->count++;
        
        // Log with occurrence count if repeating
        if (rateLimit->count > 1) {
            if (context) {
                LOG_ERROR(tag, "%s: %s (code: %d) [occurrence %lu, interval %lu ms]", 
                         context, errorToString(error), static_cast<uint32_t>(error),
                         rateLimit->count, rateLimit->logInterval);
            } else {
                LOG_ERROR(tag, "Error: %s (code: %d) [occurrence %lu, interval %lu ms]", 
                         errorToString(error), static_cast<uint32_t>(error),
                         rateLimit->count, rateLimit->logInterval);
            }
        } else {
            // First occurrence - log normally
            if (context) {
                LOG_ERROR(tag, "%s: %s (code: %d)", context, errorToString(error), 
                         static_cast<uint32_t>(error));
            } else {
                LOG_ERROR(tag, "Error: %s (code: %d)", errorToString(error), 
                         static_cast<uint32_t>(error));
            }
        }
        
        rateLimit->lastLogTime = now;
        // Exponential backoff
        rateLimit->logInterval = std::min(rateLimit->logInterval * 2, MAX_INTERVAL);
        
        // Log to persistent storage only on first occurrence and then periodically
        if (rateLimit->count == 1 || rateLimit->logInterval >= 60000) {
            ErrorLogFRAM::logError(error, nullptr, context);
        }
    }
}

void ErrorHandler::clearErrorRateLimit(SystemError error) {
    // Clear rate limit for this error (uses file-scope rateLimits array)
    for (int i = 0; i < 5; i++) {
        if (rateLimits[i].error == error) {
            rateLimits[i].error = SystemError::SUCCESS;
            rateLimits[i].lastLogTime = 0;
            rateLimits[i].logInterval = 1000;
            rateLimits[i].count = 0;
            break;
        }
    }
}

void ErrorHandler::logCriticalError(SystemError error, const char* details) {
    if (details) {
        LOG_ERROR("CRITICAL", "Critical error: %s - %s (code: %d)", 
                 errorToString(error), details, static_cast<uint32_t>(error));
    } else {
        LOG_ERROR("CRITICAL", "Critical error: %s (code: %d)", 
                 errorToString(error), static_cast<uint32_t>(error));
    }
    
    // Log to persistent storage
    ErrorLogFRAM::logCriticalError(error, details);
}