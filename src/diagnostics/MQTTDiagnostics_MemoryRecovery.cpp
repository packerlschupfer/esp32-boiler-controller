// src/diagnostics/MQTTDiagnostics_MemoryRecovery.cpp
#include "MQTTDiagnostics.h"
#include "DiagnosticsRecoveryTimer.h"
#include "LoggingMacros.h"
#include "SemaphoreGuard.h"
#include <esp_log.h>

[[maybe_unused]] static const char* TAG = "MQTTDiagnostics";

/**
 * @brief Clear diagnostics buffers and reduce memory usage during emergency
 * 
 * This function is called by MemoryGuard when system memory is critically low.
 * It performs several actions to free memory:
 * 1. Disables diagnostics temporarily
 * 2. Clears any pending updates
 * 3. Reduces update frequencies
 * 4. Estimates memory freed
 * 
 * @return Estimated bytes of memory freed
 */
size_t MQTTDiagnostics::clearDiagnosticsBuffers() {
    size_t memoryFreed = 0;
    
    // Get instance - if not created, nothing to clear
    MQTTDiagnostics* diag = getInstance();
    if (diag == nullptr) {
        return 0;
    }
    
    LOG_WARN(TAG, "Emergency memory recovery - clearing MQTT diagnostics");
    
    // Lock mutex with timeout
    SemaphoreGuard guard(diag->mutex, pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "Failed to acquire mutex for emergency clear");
        return 0;
    }
    
    // 1. Temporarily disable diagnostics
    bool wasEnabled = diag->enabled;
    diag->enabled = false;
    
    // 2. Reset last publish times to prevent immediate republishing
    diag->lastPublish.health = xTaskGetTickCount();
    diag->lastPublish.memory = xTaskGetTickCount();
    diag->lastPublish.tasks = xTaskGetTickCount();
    diag->lastPublish.sensors = xTaskGetTickCount();
    diag->lastPublish.relays = xTaskGetTickCount();
    diag->lastPublish.network = xTaskGetTickCount();
    diag->lastPublish.performance = xTaskGetTickCount();
    diag->lastPublish.pid = xTaskGetTickCount();
    diag->lastPublish.burner = xTaskGetTickCount();
    diag->lastPublish.maintenance = xTaskGetTickCount();
    
    // 3. Increase update intervals to reduce memory pressure
    // Double all intervals temporarily
    diag->intervals.health *= 2;        // 2 minutes instead of 1
    diag->intervals.memory *= 2;        // 10 minutes instead of 5
    diag->intervals.tasks *= 2;         // 10 minutes instead of 5
    diag->intervals.sensors *= 2;       // 1 minute instead of 30s
    diag->intervals.relays *= 2;        // 20 seconds instead of 10s
    diag->intervals.network *= 2;       // 2 minutes instead of 1
    diag->intervals.performance *= 2;   // 2 minutes instead of 1
    diag->intervals.pid *= 2;          // 10 seconds instead of 5s
    diag->intervals.burner *= 2;       // 10 seconds instead of 5s
    diag->intervals.maintenance *= 2;   // 2 hours instead of 1
    
    // 4. Clear performance metrics to free any accumulated data
    diag->metrics.loopCount = 0;
    diag->metrics.maxLoopTime = 0;
    diag->metrics.avgLoopTime = 0;
    diag->metrics.publishCount = 0;
    diag->metrics.publishFailures = 0;
    
    // 5. If task is suspended, we save its stack space
    if (diag->taskHandle != nullptr) {
        // Get task state
        eTaskState taskState = eTaskGetState(diag->taskHandle);
        
        // If task is not already suspended, suspend it temporarily
        if (taskState != eSuspended) {
            vTaskSuspend(diag->taskHandle);
            memoryFreed += 8192; // Approximate stack size from initialization
            
            // Schedule re-enable after 30 seconds
            scheduleDiagnosticsRecovery(SystemConstants::Diagnostics::RECOVERY_DELAY_MS);
            LOG_WARN(TAG, "MQTT Diagnostics task suspended - will resume in 30s");
        }
    }
    
    // 6. Clear any string buffers by resetting base topic
    // This forces reallocation when needed, freeing current memory
    size_t topicSize = diag->baseTopic.capacity() * sizeof(char);
    diag->baseTopic.clear();
    diag->baseTopic.shrink_to_fit();
    memoryFreed += topicSize;
    
    // Re-enable if it was enabled (but with reduced frequency)
    if (wasEnabled) {
        diag->enabled = true;
    }
    
    // Estimate total memory freed
    // - JSON document allocations (temporary): ~2KB per publish
    // - String operations: ~1KB
    // - Task suspension (if done): 8KB
    // - Base topic string: variable
    memoryFreed += 3072; // Conservative estimate for temporary allocations
    
    LOG_WARN(TAG, "Cleared MQTT diagnostics - freed approximately %d bytes", memoryFreed);
    
    return memoryFreed;
}

/**
 * @brief Restore normal diagnostic operation after memory recovery
 * 
 * This should be called when memory pressure has been relieved
 */
void MQTTDiagnostics::restoreNormalOperation() {
    MQTTDiagnostics* diag = getInstance();
    if (diag == nullptr) {
        return;
    }
    
    SemaphoreGuard guard(diag->mutex, pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        return;
    }

    LOG_INFO(TAG, "Restoring normal diagnostic operation");

    // Restore original intervals
    diag->intervals.health = SystemConstants::Diagnostics::HEALTH_INTERVAL_MS;
    diag->intervals.memory = SystemConstants::Diagnostics::MEMORY_INTERVAL_MS;
    diag->intervals.tasks = SystemConstants::Diagnostics::TASKS_INTERVAL_MS;
    diag->intervals.sensors = SystemConstants::Diagnostics::SENSORS_INTERVAL_MS;
    diag->intervals.relays = SystemConstants::Diagnostics::RELAYS_INTERVAL_MS;
    diag->intervals.network = SystemConstants::Diagnostics::NETWORK_INTERVAL_MS;
    diag->intervals.performance = SystemConstants::Diagnostics::PERFORMANCE_INTERVAL_MS;
    diag->intervals.pid = SystemConstants::Diagnostics::PID_INTERVAL_MS;
    diag->intervals.burner = SystemConstants::Diagnostics::BURNER_INTERVAL_MS;
    diag->intervals.maintenance = SystemConstants::Diagnostics::MAINTENANCE_INTERVAL_MS;
    
    // Resume task if suspended
    if (diag->taskHandle != nullptr) {
        eTaskState taskState = eTaskGetState(diag->taskHandle);
        if (taskState == eSuspended) {
            vTaskResume(diag->taskHandle);
            LOG_INFO(TAG, "Diagnostics task resumed");
        }
    }
}

/**
 * @brief Get current memory usage by diagnostics
 * 
 * @return Estimated memory currently used by diagnostics
 */
size_t MQTTDiagnostics::getMemoryUsage() {
    size_t usage = sizeof(MQTTDiagnostics); // Object size
    
    MQTTDiagnostics* diag = getInstance();
    if (diag != nullptr) {
        // Add string capacity
        usage += diag->baseTopic.capacity();
        
        // Add task stack if running
        if (diag->taskHandle != nullptr) {
            usage += 8192; // Stack size from initialization
        }
        
        // Add mutex overhead
        usage += sizeof(SemaphoreHandle_t);
        
        // Estimate temporary allocations during publishing
        if (diag->enabled) {
            usage += 4096; // Typical JSON document size
        }
    }
    
    return usage;
}