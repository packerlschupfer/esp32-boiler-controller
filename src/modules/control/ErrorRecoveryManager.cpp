// src/modules/control/ErrorRecoveryManager.cpp
#include "modules/control/ErrorRecoveryManager.h"
#include "core/SystemResourceProvider.h"
#include "events/SystemEventsGenerated.h"
#include "utils/ErrorHandler.h"
#include "utils/MutexRetryHelper.h"
#include "config/SystemConstants.h"
#include "LoggingMacros.h"
#include <algorithm>

static const char* TAG = "ErrorRecoveryManager";

// Bounded mutex timeout - NEVER use MUTEX_TIMEOUT to prevent deadlock
static constexpr TickType_t MUTEX_TIMEOUT = pdMS_TO_TICKS(SystemConstants::Timing::MUTEX_DEFAULT_TIMEOUT_MS);

ErrorRecoveryManager::ErrorRecoveryManager() 
    : recoveryEnabled(true) {
    // Create mutex
    mutex = xSemaphoreCreateMutex();
    if (!mutex) {
        LOG_ERROR(TAG, "Failed to create mutex");
    }
    
    // Initialize default recovery policies
    registerRecoveryPolicy(SystemError::SENSOR_FAILURE, {
        RecoveryStrategy::RETRY_WITH_BACKOFF,
        3,      // maxAttempts
        1000,   // initialDelayMs
        10000,  // maxDelayMs
        2.0f,   // backoffMultiplier
        defaultSensorRecovery,
        RecoveryStrategy::DEGRADE_SERVICE
    });
    
    registerRecoveryPolicy(SystemError::NETWORK_ERROR, {
        RecoveryStrategy::RETRY_WITH_BACKOFF,
        5,      // maxAttempts
        2000,   // initialDelayMs
        30000,  // maxDelayMs
        1.5f,   // backoffMultiplier
        defaultNetworkRecovery,
        RecoveryStrategy::RESET_COMPONENT
    });
    
    registerRecoveryPolicy(SystemError::MODBUS_TIMEOUT, {
        RecoveryStrategy::RETRY_WITH_BACKOFF,
        3,      // maxAttempts
        500,    // initialDelayMs
        5000,   // maxDelayMs
        2.0f,   // backoffMultiplier
        defaultModbusRecovery,
        RecoveryStrategy::RESTART_TASK
    });
    
    registerRecoveryPolicy(SystemError::RELAY_FAULT, {
        RecoveryStrategy::RESET_COMPONENT,
        2,      // maxAttempts
        100,    // initialDelayMs
        1000,   // maxDelayMs
        1.0f,   // backoffMultiplier
        defaultRelayRecovery,
        RecoveryStrategy::EMERGENCY_STOP
    });
    
    // Start recovery monitoring task
    xTaskCreate(
        recoveryTask,
        "RecoveryMonitor",
        2048,
        this,
        tskIDLE_PRIORITY + 1,
        &recoveryTaskHandle
    );
}

ErrorRecoveryManager& ErrorRecoveryManager::getInstance() {
    static ErrorRecoveryManager instance;
    return instance;
}

void ErrorRecoveryManager::registerRecoveryPolicy(SystemError error, const RecoveryPolicy& policy) {
    auto guard = MutexRetryHelper::acquireGuard(mutex, "ErrorRecovery-RegisterPolicy");
    if (guard) {
        policies[error] = policy;
    }
}

ErrorRecoveryManager::RecoveryResult ErrorRecoveryManager::handleError(
    SystemError error,
    const char* component,
    void* customData) {

    if (!recoveryEnabled) {
        LOG_WARN(TAG, "Recovery disabled - ignoring error %d from %s", (int)error, component);
        return RecoveryResult::ABANDONED;
    }

    ErrorContext context = {
        .error = error,
        .component = component,
        .timestamp = millis(),
        .occurrenceCount = 0,
        .recoveryAttempts = 0,
        .customData = customData
    };

    RecoveryPolicy policy;

    // First critical section: check state and get policy
    {
        auto guard = MutexRetryHelper::acquireGuard(mutex, "ErrorRecovery-HandleError1");
        if (!guard) {
            return RecoveryResult::FAILED;
        }

        // Update error history
        updateErrorHistory(context);

        // Check if component is already in recovery
        if (activeRecoveries[component]) {
            LOG_WARN(TAG, "Component %s already in recovery", component);
            return RecoveryResult::IN_PROGRESS;
        }

        // Mark as in recovery
        activeRecoveries[component] = true;

        // Get recovery policy
        auto it = policies.find(error);
        if (it == policies.end()) {
            LOG_ERROR(TAG, "No recovery policy for error %d", (int)error);
            activeRecoveries[component] = false;
            return RecoveryResult::FAILED;
        }

        policy = it->second;

        // Get error count for this component
        auto& history = errorHistory[component];
        context.occurrenceCount = std::count_if(history.begin(), history.end(),
            [error, context](const ErrorContext& e) {
                return e.error == error &&
                       (context.timestamp - e.timestamp) < ERROR_HISTORY_WINDOW_MS;
            });
    } // guard released here

    // Check if we should escalate (no mutex needed)
    if (shouldEscalate(context)) {
        LOG_WARN(TAG, "Escalating recovery for %s - too many errors", component);
        policy.strategy = policy.escalationStrategy;
    }

    // Execute recovery (no mutex needed)
    RecoveryResult result = executeRecovery(context, policy);

    // Second critical section: update statistics
    {
        auto guard = MutexRetryHelper::acquireGuard(mutex, "ErrorRecovery-HandleError2");
        if (guard) {
            stats.totalErrors++;
            if (result == RecoveryResult::SUCCESS) {
                stats.successfulRecoveries++;
            } else if (result == RecoveryResult::FAILED) {
                stats.failedRecoveries++;
            } else if (result == RecoveryResult::ESCALATED) {
                stats.escalations++;
            }
            stats.errorCounts[error]++;

            // Clear active recovery flag
            activeRecoveries[component] = false;
        }
    }

    return result;
}

void ErrorRecoveryManager::registerTask(
    const char* taskName,
    TaskFunction_t taskFunction,
    uint32_t stackSize,
    UBaseType_t priority) {

    auto guard = MutexRetryHelper::acquireGuard(mutex, "ErrorRecovery-RegisterTask");
    if (guard) {
        taskRegistry[taskName] = {
            .function = taskFunction,
            .stackSize = stackSize,
            .priority = priority,
            .handle = nullptr,
            .restartCount = 0,
            .lastRestartTime = 0
        };
    }
}

bool ErrorRecoveryManager::isRecovering(const char* component) {
    auto guard = MutexRetryHelper::acquireGuard(mutex, "ErrorRecovery-IsRecovering");
    if (guard) {
        return activeRecoveries[component];
    }
    return false;
}

void ErrorRecoveryManager::clearErrorHistory(const char* component) {
    auto guard = MutexRetryHelper::acquireGuard(mutex, "ErrorRecovery-ClearHistory");
    if (guard) {
        errorHistory[component].clear();
    }
}

ErrorRecoveryManager::RecoveryResult ErrorRecoveryManager::executeRecovery(
    const ErrorContext& context,
    const RecoveryPolicy& policy) {
    
    LOG_INFO(TAG, "Executing recovery strategy %d for %s", 
             (int)policy.strategy, context.component);
    
    switch (policy.strategy) {
        case RecoveryStrategy::RETRY:
        case RecoveryStrategy::RETRY_WITH_BACKOFF:
            return retryWithBackoff(context, policy);
            
        case RecoveryStrategy::RESET_COMPONENT:
            if (policy.customAction) {
                return policy.customAction(context);
            }
            LOG_WARN(TAG, "No custom action for component reset");
            return RecoveryResult::FAILED;
            
        case RecoveryStrategy::RESTART_TASK:
            return restartTask(context.component);
            
        case RecoveryStrategy::DEGRADE_SERVICE:
            // Set degraded mode bit
            xEventGroupSetBits(SRP::getSystemStateEventGroup(), SystemEvents::SystemState::DEGRADED_MODE);
            LOG_WARN(TAG, "System entering degraded mode");
            return RecoveryResult::SUCCESS;
            
        case RecoveryStrategy::FAILOVER:
            if (policy.customAction) {
                return policy.customAction(context);
            }
            LOG_WARN(TAG, "No failover action defined");
            return RecoveryResult::FAILED;
            
        case RecoveryStrategy::EMERGENCY_STOP:
            xEventGroupSetBits(SRP::getSystemStateEventGroup(), SystemEvents::SystemState::EMERGENCY_STOP);
            LOG_ERROR(TAG, "Emergency stop triggered");
            return RecoveryResult::ESCALATED;
            
        case RecoveryStrategy::SYSTEM_RESET:
            LOG_ERROR(TAG, "System reset requested - restarting in 5 seconds");
            vTaskDelay(pdMS_TO_TICKS(SystemConstants::Timing::RECOVERY_WAIT_MS));
            esp_restart();
            return RecoveryResult::ESCALATED;
            
        default:
            return RecoveryResult::FAILED;
    }
}

ErrorRecoveryManager::RecoveryResult ErrorRecoveryManager::retryWithBackoff(
    const ErrorContext& context,
    const RecoveryPolicy& policy) {
    
    uint32_t delay = policy.initialDelayMs;
    
    for (uint8_t attempt = 0; attempt < policy.maxAttempts; attempt++) {
        LOG_INFO(TAG, "Recovery attempt %d/%d for %s", 
                 attempt + 1, policy.maxAttempts, context.component);
        
        // Wait with backoff
        vTaskDelay(pdMS_TO_TICKS(delay));
        
        // Execute custom recovery action if provided
        if (policy.customAction) {
            RecoveryResult result = policy.customAction(context);
            if (result == RecoveryResult::SUCCESS) {
                LOG_INFO(TAG, "Recovery successful on attempt %d", attempt + 1);
                return RecoveryResult::SUCCESS;
            }
        }
        
        // Calculate next delay with backoff
        delay = std::min((uint32_t)(delay * policy.backoffMultiplier), policy.maxDelayMs);
    }
    
    LOG_ERROR(TAG, "Recovery failed after %d attempts", policy.maxAttempts);
    return RecoveryResult::FAILED;
}

ErrorRecoveryManager::RecoveryResult ErrorRecoveryManager::restartTask(const char* taskName) {
    auto guard = MutexRetryHelper::acquireGuard(mutex, "ErrorRecovery-RestartTask");
    if (!guard) {
        return RecoveryResult::FAILED;
    }

    auto it = taskRegistry.find(taskName);
    if (it == taskRegistry.end()) {
        LOG_ERROR(TAG, "Task %s not registered", taskName);
        return RecoveryResult::FAILED;
    }

    TaskInfo& taskInfo = it->second;

    // Check restart rate limit
    uint32_t now = millis();
    if (taskInfo.lastRestartTime != 0 &&
        (now - taskInfo.lastRestartTime) < MIN_RESTART_INTERVAL_MS) {
        LOG_WARN(TAG, "Task %s restart rate limited", taskName);
        return RecoveryResult::FAILED;
    }

    // Delete existing task if running
    if (taskInfo.handle != nullptr) {
        vTaskDelete(taskInfo.handle);
        taskInfo.handle = nullptr;
        vTaskDelay(pdMS_TO_TICKS(SystemConstants::Timing::FINAL_CLEANUP_DELAY_MS)); // Allow cleanup
    }

    // Create new task
    BaseType_t result = xTaskCreate(
        taskInfo.function,
        taskName,
        taskInfo.stackSize,
        nullptr,
        taskInfo.priority,
        &taskInfo.handle
    );

    if (result == pdPASS) {
        taskInfo.restartCount++;
        taskInfo.lastRestartTime = now;
        LOG_INFO(TAG, "Task %s restarted successfully (count: %d)",
                 taskName, taskInfo.restartCount);
        return RecoveryResult::SUCCESS;
    } else {
        LOG_ERROR(TAG, "Failed to restart task %s", taskName);
        return RecoveryResult::FAILED;
    }
}

void ErrorRecoveryManager::updateErrorHistory(const ErrorContext& context) {
    auto& history = errorHistory[context.component];
    
    // Add new error
    history.push_back(context);
    
    // Clean old entries
    uint32_t cutoffTime = context.timestamp - ERROR_HISTORY_WINDOW_MS;
    history.erase(
        std::remove_if(history.begin(), history.end(),
            [cutoffTime](const ErrorContext& e) {
                return e.timestamp < cutoffTime;
            }),
        history.end()
    );
}

bool ErrorRecoveryManager::shouldEscalate(const ErrorContext& context) {
    return context.occurrenceCount >= MAX_ERRORS_PER_WINDOW;
}

void ErrorRecoveryManager::recoveryTask(void* pvParameters) {
    ErrorRecoveryManager* manager = static_cast<ErrorRecoveryManager*>(pvParameters);

    while (true) {
        // Periodic cleanup of error history and check for stuck recoveries
        {
            auto guard = MutexRetryHelper::acquireGuard(manager->mutex, "ErrorRecovery-MonitorTask");
            if (guard) {
                manager->cleanErrorHistory();

                // Check for stuck recoveries
                for (auto& [component, active] : manager->activeRecoveries) {
                    if (active) {
                        LOG_WARN(TAG, "Component %s stuck in recovery", component.c_str());
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(SystemConstants::Timing::RECOVERY_MONITOR_INTERVAL_MS));
    }
}

void ErrorRecoveryManager::cleanErrorHistory() {
    uint32_t cutoffTime = millis() - ERROR_HISTORY_WINDOW_MS;
    
    for (auto& [component, history] : errorHistory) {
        history.erase(
            std::remove_if(history.begin(), history.end(),
                [cutoffTime](const ErrorContext& e) {
                    return e.timestamp < cutoffTime;
                }),
            history.end()
        );
    }
}

// Default recovery actions
ErrorRecoveryManager::RecoveryResult ErrorRecoveryManager::defaultSensorRecovery(const ErrorContext& context) {
    LOG_INFO(TAG, "Attempting sensor recovery for %s", context.component);
    
    // Clear sensor error bits
    xEventGroupClearBits(SRP::getErrorNotificationEventGroup(), SystemEvents::Error::SENSOR_FAILURE);
    
    // Small delay to allow sensor to recover
    vTaskDelay(pdMS_TO_TICKS(SystemConstants::Timing::RECOVERY_STABILIZATION_MS));
    
    // Check if sensor is back online
    EventBits_t errorBits = xEventGroupGetBits(SRP::getErrorNotificationEventGroup());
    if (!(errorBits & SystemEvents::Error::SENSOR_FAILURE)) {
        LOG_INFO(TAG, "Sensor recovery successful");
        return RecoveryResult::SUCCESS;
    }
    
    return RecoveryResult::FAILED;
}

ErrorRecoveryManager::RecoveryResult ErrorRecoveryManager::defaultNetworkRecovery(const ErrorContext& context) {
    LOG_INFO(TAG, "Attempting network recovery");
    
    // Clear network error bits
    xEventGroupClearBits(SRP::getErrorNotificationEventGroup(), SystemEvents::Error::NETWORK);
    
    // Wait for network to recover
    vTaskDelay(pdMS_TO_TICKS(SystemConstants::Timing::SYSTEM_STABILIZATION_MS));
    
    // Check if network is back
    EventBits_t errorBits = xEventGroupGetBits(SRP::getErrorNotificationEventGroup());
    if (!(errorBits & SystemEvents::Error::NETWORK)) {
        LOG_INFO(TAG, "Network recovery successful");
        return RecoveryResult::SUCCESS;
    }
    
    return RecoveryResult::FAILED;
}

ErrorRecoveryManager::RecoveryResult ErrorRecoveryManager::defaultModbusRecovery(const ErrorContext& context) {
    LOG_INFO(TAG, "Attempting Modbus recovery");
    
    // Clear Modbus error bits
    xEventGroupClearBits(SRP::getErrorNotificationEventGroup(), SystemEvents::Error::MODBUS);
    
    // Small delay for bus recovery
    vTaskDelay(pdMS_TO_TICKS(SystemConstants::Timing::CLEANUP_DELAY_MS));
    
    return RecoveryResult::SUCCESS;
}

ErrorRecoveryManager::RecoveryResult ErrorRecoveryManager::defaultRelayRecovery(const ErrorContext& context) {
    LOG_INFO(TAG, "Attempting relay recovery");
    
    // Clear relay error bits
    xEventGroupClearBits(SRP::getErrorNotificationEventGroup(), SystemEvents::Error::RELAY);
    
    // Reset relay states (this would trigger relay re-initialization)
    // The actual relay reset would be handled by the RelayControlTask

    vTaskDelay(pdMS_TO_TICKS(SystemConstants::Timing::FINAL_CLEANUP_DELAY_MS));
    
    return RecoveryResult::SUCCESS;
}