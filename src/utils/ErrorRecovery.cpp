// src/utils/ErrorRecovery.cpp
#include "utils/ErrorRecovery.h"
#include "utils/MutexRetryHelper.h"
#include "LoggingMacros.h"
#include "core/SystemResourceProvider.h"
#include "events/SystemEventsGenerated.h"
#include <esp_system.h>
#include <algorithm>

static const char* TAG = "ErrorRecovery";

// Static member definitions
SemaphoreHandle_t ErrorRecovery::recoveryMutex_ = nullptr;
EventGroupHandle_t ErrorRecovery::recoveryEventGroup_ = nullptr;
std::vector<ErrorRecovery::ErrorRecord> ErrorRecovery::errorHistory_;
std::vector<std::pair<uint32_t, ErrorRecovery::RecoveryAction>> ErrorRecovery::customActions_;
ErrorRecovery::RecoveryState ErrorRecovery::recoveryState_ = {false, Category::SYSTEM, 0, 0, Strategy::NONE};
bool ErrorRecovery::initialized_ = false;

bool ErrorRecovery::initialize() {
    if (initialized_) {
        return true;
    }

    // Create mutex
    recoveryMutex_ = xSemaphoreCreateMutex();
    if (!recoveryMutex_) {
        LOG_ERROR(TAG, "Failed to create recovery mutex");
        return false;
    }

    // Create event group
    recoveryEventGroup_ = xEventGroupCreate();
    if (!recoveryEventGroup_) {
        LOG_ERROR(TAG, "Failed to create recovery event group");
        vSemaphoreDelete(recoveryMutex_);
        recoveryMutex_ = nullptr;
        return false;
    }

    // Reserve space for error history
    errorHistory_.reserve(20);
    customActions_.reserve(10);

    initialized_ = true;
    LOG_INFO(TAG, "Error recovery system initialized");
    return true;
}

ErrorRecovery::RecoveryPlan ErrorRecovery::reportError(
    Category category,
    Severity severity,
    uint32_t errorCode,
    const char* source,
    const char* description) {
    
    if (!initialized_) {
        initialize();
    }

    ErrorContext context = {
        .category = category,
        .severity = severity,
        .errorCode = errorCode,
        .source = source,
        .description = description,
        .timestamp = millis(),
        .occurrenceCount = 1
    };

    // Thread-safe error recording
    {
        auto guard = MutexRetryHelper::acquireGuard(recoveryMutex_, "ErrorRecovery-Report");
        if (guard) {
            // Check if this error already exists
            auto it = std::find_if(errorHistory_.begin(), errorHistory_.end(),
                [&](const ErrorRecord& record) {
                    return record.context.category == category &&
                           record.context.errorCode == errorCode;
                });

            if (it != errorHistory_.end()) {
                // Update existing error
                it->lastOccurrence = context.timestamp;
                it->totalCount++;
                it->recovered = false;
                context.occurrenceCount = it->totalCount;
            } else {
                // Add new error
                if (errorHistory_.size() >= 20) {
                    // Remove oldest recovered error
                    auto oldestIt = std::min_element(errorHistory_.begin(), errorHistory_.end(),
                        [](const ErrorRecord& a, const ErrorRecord& b) {
                            return a.recovered && a.lastOccurrence < b.lastOccurrence;
                        });
                    if (oldestIt != errorHistory_.end() && oldestIt->recovered) {
                        errorHistory_.erase(oldestIt);
                    }
                }

                ErrorRecord record = {
                    .context = context,
                    .firstOccurrence = context.timestamp,
                    .lastOccurrence = context.timestamp,
                    .totalCount = 1,
                    .recovered = false
                };
                errorHistory_.push_back(record);
            }
        }
    }

    // Log the error
    const char* severityStr = "UNKNOWN";
    switch (severity) {
        case Severity::INFO: severityStr = "INFO"; break;
        case Severity::WARNING: severityStr = "WARNING"; break;
        case Severity::ERROR: severityStr = "ERROR"; break;
        case Severity::CRITICAL: severityStr = "CRITICAL"; break;
        case Severity::FATAL: severityStr = "FATAL"; break;
    }
    
    LOG_ERROR(TAG, "[%s] %s: %s (code: 0x%04X, count: %lu)",
              severityStr, source, description, errorCode, context.occurrenceCount);

    // Update system error bits
    EventGroupHandle_t errorEventGroup = SRP::getErrorNotificationEventGroup();
    if (errorEventGroup) {
        switch (category) {
            case Category::SENSOR:
                xEventGroupSetBits(errorEventGroup, SystemEvents::Error::SENSOR_FAILURE);
                break;
            case Category::COMMUNICATION:
                xEventGroupSetBits(errorEventGroup, SystemEvents::Error::COMMUNICATION);
                break;
            case Category::HARDWARE:
                xEventGroupSetBits(errorEventGroup, SystemEvents::Error::RELAY);  // Hardware errors use relay bit
                break;
            case Category::NETWORK:
                xEventGroupSetBits(errorEventGroup, SystemEvents::Error::COMMUNICATION);
                break;
            case Category::MEMORY:
                xEventGroupSetBits(errorEventGroup, SystemEvents::Error::MEMORY);
                break;
            default:
                xEventGroupSetBits(errorEventGroup, SystemEvents::Error::GENERAL_SYSTEM);
                break;
        }
    }

    // Check for custom recovery action
    RecoveryPlan plan = getDefaultPlan(category, severity);
    
    auto customIt = std::find_if(customActions_.begin(), customActions_.end(),
        [errorCode](const std::pair<uint32_t, RecoveryAction>& action) {
            return action.first == errorCode;
        });
    
    if (customIt != customActions_.end()) {
        plan.customAction = customIt->second;
    }

    // Handle fatal errors immediately
    if (severity == Severity::FATAL) {
        emergencyShutdown(description);
    }

    return plan;
}

bool ErrorRecovery::executeRecovery(const RecoveryPlan& plan, const ErrorContext& context) {
    if (!initialized_) {
        return false;
    }

    LOG_INFO(TAG, "Executing recovery plan for %s", context.description);

    // Set recovery in progress
    xEventGroupSetBits(recoveryEventGroup_, RECOVERY_IN_PROGRESS_BIT);

    {
        auto guard = MutexRetryHelper::acquireGuard(recoveryMutex_, "ErrorRecovery-Execute");
        if (guard) {
            recoveryState_.inRecovery = true;
            recoveryState_.activeCategory = context.category;
            recoveryState_.recoveryStartTime = millis();
            recoveryState_.currentStrategy = plan.primaryStrategy;
            recoveryState_.retryCount = 0;
        }
    }

    bool recovered = false;
    uint32_t retryCount = 0;
    Strategy currentStrategy = plan.primaryStrategy;

    while (!recovered && retryCount < plan.maxRetries) {
        // Calculate delay with exponential backoff
        uint32_t delay = calculateBackoff(plan.retryDelayMs, retryCount, plan.backoffMultiplier);
        
        if (retryCount > 0) {
            LOG_INFO(TAG, "Retry %lu/%lu after %lu ms delay", retryCount, plan.maxRetries, delay);
            vTaskDelay(pdMS_TO_TICKS(delay));
        }

        // Execute recovery strategy
        switch (currentStrategy) {
            case Strategy::RETRY:
                // If custom action provided, use it
                if (plan.customAction) {
                    recovered = plan.customAction();
                } else {
                    // Generic retry - just wait and return
                    recovered = false;
                }
                break;

            case Strategy::FALLBACK:
                LOG_INFO(TAG, "Using fallback values/mode");
                // Set system to use fallback values
                xEventGroupSetBits(SRP::getSystemStateEventGroup(), 
                                  SystemEvents::SystemState::WARNING);  // Use warning bit for fallback mode
                recovered = true;
                break;

            case Strategy::RESTART_TASK:
                LOG_WARN(TAG, "Task restart requested for %s", context.source);
                // Task restart would be handled by TaskManager
                // For now, just log
                recovered = false;
                break;

            case Strategy::RESTART_MODULE:
                LOG_WARN(TAG, "Module restart requested");
                // Module-specific restart logic would go here
                recovered = false;
                break;

            case Strategy::SAFE_MODE:
                LOG_WARN(TAG, "Entering safe mode");
                xEventGroupSetBits(recoveryEventGroup_, SAFE_MODE_ACTIVE_BIT);
                xEventGroupSetBits(SRP::getSystemStateEventGroup(), 
                                  SystemEvents::SystemState::DEGRADED_MODE);  // Use degraded mode for safe mode
                // Disable non-essential functions
                xEventGroupClearBits(SRP::getSystemStateEventGroup(),
                                    SystemEvents::SystemState::HEATING_ENABLED | 
                                    SystemEvents::SystemState::WATER_ENABLED);
                recovered = true;
                break;

            case Strategy::REBOOT:
                LOG_ERROR(TAG, "System reboot requested");
                emergencyShutdown("Recovery reboot");
                break;

            default:
                recovered = true;
                break;
        }

        if (!recovered && retryCount >= plan.maxRetries / 2 && plan.fallbackStrategy != Strategy::NONE) {
            // Switch to fallback strategy
            LOG_WARN(TAG, "Switching to fallback strategy");
            currentStrategy = plan.fallbackStrategy;
        }

        retryCount++;

        {
            auto guard = MutexRetryHelper::acquireGuard(recoveryMutex_, "ErrorRecovery-Retry");
            if (guard) {
                recoveryState_.retryCount = retryCount;
                recoveryState_.currentStrategy = currentStrategy;
            }
        }
    }

    // Clear recovery state
    {
        auto guard = MutexRetryHelper::acquireGuard(recoveryMutex_, "ErrorRecovery-Clear");
        if (guard) {
            recoveryState_.inRecovery = false;

            // Mark error as recovered in history
            auto it = std::find_if(errorHistory_.begin(), errorHistory_.end(),
                [&](ErrorRecord& record) {
                    return record.context.category == context.category &&
                           record.context.errorCode == context.errorCode;
                });
            if (it != errorHistory_.end()) {
                it->recovered = recovered;
            }
        }
    }

    xEventGroupClearBits(recoveryEventGroup_, RECOVERY_IN_PROGRESS_BIT);

    if (recovered) {
        LOG_INFO(TAG, "Recovery successful for %s", context.description);
    } else {
        LOG_ERROR(TAG, "Recovery failed for %s after %lu attempts", 
                  context.description, retryCount);
    }

    return recovered;
}

void ErrorRecovery::registerRecoveryAction(Category category, uint32_t errorCode, RecoveryAction action) {
    if (!initialized_) {
        initialize();
    }

    auto guard = MutexRetryHelper::acquireGuard(recoveryMutex_, "ErrorRecovery-Register");
    if (guard) {
        customActions_.push_back({errorCode, action});
    }
}

bool ErrorRecovery::isInRecovery() {
    if (!initialized_) {
        return false;
    }

    EventBits_t bits = xEventGroupGetBits(recoveryEventGroup_);
    return (bits & RECOVERY_IN_PROGRESS_BIT) != 0;
}

uint8_t ErrorRecovery::getHealthScore() {
    if (!initialized_) {
        return 100;
    }

    uint8_t score = 100;

    auto guard = MutexRetryHelper::acquireGuard(recoveryMutex_, "ErrorRecovery-HealthScore");
    if (guard) {
        uint32_t now = millis();

        // Deduct points for recent errors
        for (const auto& record : errorHistory_) {
            if (!record.recovered && (now - record.lastOccurrence) < 300000) { // 5 minutes
                switch (record.context.severity) {
                    case Severity::WARNING:
                        score = (score > 5) ? score - 5 : 0;
                        break;
                    case Severity::ERROR:
                        score = (score > 10) ? score - 10 : 0;
                        break;
                    case Severity::CRITICAL:
                        score = (score > 20) ? score - 20 : 0;
                        break;
                    case Severity::FATAL:
                        score = 0;
                        break;
                    default:
                        break;
                }
            }
        }

        // Additional deductions for system state
        EventBits_t errorBits = xEventGroupGetBits(SRP::getErrorNotificationEventGroup());
        if (errorBits & SystemEvents::Error::ANY_ACTIVE) {
            score = (score > 10) ? score - 10 : 0;
        }

        if (recoveryState_.inRecovery) {
            score = (score > 15) ? score - 15 : 0;
        }
    }

    return score;
}

void ErrorRecovery::clearErrors(Category category) {
    if (!initialized_) {
        return;
    }

    {
        auto guard = MutexRetryHelper::acquireGuard(recoveryMutex_, "ErrorRecovery-ClearErrors");
        if (guard) {
            errorHistory_.erase(
                std::remove_if(errorHistory_.begin(), errorHistory_.end(),
                    [category](const ErrorRecord& record) {
                        return record.context.category == category;
                    }),
                errorHistory_.end()
            );
        }
    }

    // Clear corresponding error bits
    EventGroupHandle_t errorEventGroup = SRP::getErrorNotificationEventGroup();
    if (errorEventGroup) {
        switch (category) {
            case Category::SENSOR:
                xEventGroupClearBits(errorEventGroup, SystemEvents::Error::SENSOR_FAILURE);
                break;
            case Category::COMMUNICATION:
                xEventGroupClearBits(errorEventGroup, SystemEvents::Error::COMMUNICATION);
                break;
            case Category::HARDWARE:
                xEventGroupClearBits(errorEventGroup, SystemEvents::Error::RELAY);  // Hardware errors use relay bit
                break;
            case Category::MEMORY:
                xEventGroupClearBits(errorEventGroup, SystemEvents::Error::MEMORY);
                break;
            default:
                break;
        }
    }
}

void ErrorRecovery::emergencyShutdown(const char* reason) {
    LOG_ERROR(TAG, "EMERGENCY SHUTDOWN: %s", reason);
    
    // Set emergency shutdown bit
    if (recoveryEventGroup_) {
        xEventGroupSetBits(recoveryEventGroup_, EMERGENCY_SHUTDOWN_BIT);
    }

    // Turn off all dangerous systems
    EventGroupHandle_t systemEventGroup = SRP::getSystemStateEventGroup();
    if (systemEventGroup) {
        xEventGroupClearBits(systemEventGroup, 
            SystemEvents::SystemState::BOILER_ENABLED |
            SystemEvents::SystemState::HEATING_ENABLED |
            SystemEvents::SystemState::WATER_ENABLED |
            SystemEvents::SystemState::HEATING_ON |
            SystemEvents::SystemState::WATER_ON);
    }

    // Clear all burner requests
    SRP::clearBurnerRequestEventBits(0xFFFFFFFF);

    // Wait a bit for systems to shut down
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Reboot
    LOG_ERROR(TAG, "Rebooting system...");
    esp_restart();
}

ErrorRecovery::RecoveryPlan ErrorRecovery::getDefaultPlan(Category category, Severity severity) {
    RecoveryPlan plan = {
        .primaryStrategy = Strategy::NONE,
        .fallbackStrategy = Strategy::NONE,
        .maxRetries = 3,
        .retryDelayMs = 1000,
        .backoffMultiplier = 2.0f,
        .customAction = nullptr
    };

    // Set strategy based on category and severity
    switch (category) {
        case Category::SENSOR:
            plan.primaryStrategy = Strategy::RETRY;
            plan.fallbackStrategy = Strategy::FALLBACK;
            plan.maxRetries = 5;
            break;

        case Category::COMMUNICATION:
            plan.primaryStrategy = Strategy::RETRY;
            plan.maxRetries = 10;
            plan.retryDelayMs = 2000;
            break;

        case Category::HARDWARE:
            if (severity >= Severity::CRITICAL) {
                plan.primaryStrategy = Strategy::SAFE_MODE;
            } else {
                plan.primaryStrategy = Strategy::RETRY;
                plan.maxRetries = 3;
            }
            break;

        case Category::NETWORK:
            plan.primaryStrategy = Strategy::RETRY;
            plan.maxRetries = 20;
            plan.retryDelayMs = 5000;
            break;

        case Category::MEMORY:
            if (severity >= Severity::ERROR) {
                plan.primaryStrategy = Strategy::RESTART_MODULE;
                plan.fallbackStrategy = Strategy::REBOOT;
            }
            break;

        case Category::SYSTEM:
            if (severity >= Severity::CRITICAL) {
                plan.primaryStrategy = Strategy::REBOOT;
            } else {
                plan.primaryStrategy = Strategy::RESTART_TASK;
            }
            break;
    }

    // Override for fatal errors
    if (severity == Severity::FATAL) {
        plan.primaryStrategy = Strategy::REBOOT;
        plan.maxRetries = 1;
    }

    return plan;
}

uint32_t ErrorRecovery::calculateBackoff(uint32_t baseDelay, uint32_t retryCount, float multiplier) {
    uint32_t delay = baseDelay;
    for (uint32_t i = 0; i < retryCount; i++) {
        delay = static_cast<uint32_t>(delay * multiplier);
        // Cap at 5 minutes
        if (delay > 300000) {
            delay = 300000;
            break;
        }
    }
    return delay;
}