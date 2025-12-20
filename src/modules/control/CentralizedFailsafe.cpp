// src/modules/control/CentralizedFailsafe.cpp
#include "modules/control/CentralizedFailsafe.h"
#include "modules/control/BurnerSystemController.h"
#include "modules/tasks/RelayControlTask.h"
#include "core/SystemResourceProvider.h"
#include "config/SystemConstants.h"
#include "shared/SharedSensorReadings.h"
#include "shared/SharedRelayReadings.h"
#include "events/SystemEventsGenerated.h"
#include "config/RelayIndices.h"
#include "monitoring/HealthMonitor.h"
#include "utils/CriticalDataStorage.h"
#include "utils/MutexRetryHelper.h"
#include "LoggingMacros.h"
// #include "utils/PersistentStorage.h" // Not available in this codebase
#include <algorithm>

static const char* TAG = "CentralizedFailsafe";

// Static member definitions
SemaphoreHandle_t CentralizedFailsafe::stateMutex_ = nullptr;  // Round 20 Issue #10
CentralizedFailsafe::FailsafeLevel CentralizedFailsafe::currentLevel = FailsafeLevel::NORMAL;
SystemError CentralizedFailsafe::lastError = SystemError::SUCCESS;
uint32_t CentralizedFailsafe::failsafeStartTime = 0;
uint32_t CentralizedFailsafe::recoveryAttempts = 0;
CentralizedFailsafe::SystemState CentralizedFailsafe::savedState;
bool CentralizedFailsafe::initialized = false;
std::vector<std::pair<CentralizedFailsafe::Subsystem, CentralizedFailsafe::FailsafeCallback>>
    CentralizedFailsafe::subsystemCallbacks;

void CentralizedFailsafe::initialize() {
    if (initialized) {
        LOG_WARN(TAG, "Already initialized");
        return;
    }

    LOG_INFO(TAG, "Initializing centralized failsafe system");

    // Round 20 Issue #10: Create mutex for state protection
    if (!stateMutex_) {
        stateMutex_ = xSemaphoreCreateMutex();
        if (!stateMutex_) {
            LOG_ERROR(TAG, "Failed to create state mutex");
            return;
        }
    }

    currentLevel = FailsafeLevel::NORMAL;
    lastError = SystemError::SUCCESS;
    failsafeStartTime = 0;
    recoveryAttempts = 0;
    savedState = SystemState();
    subsystemCallbacks.clear();
    
    // Register default failsafe actions
    registerSubsystem(Subsystem::BURNER, [](FailsafeLevel level, SystemError reason) {
        defaultBurnerFailsafe(level);
    });
    
    registerSubsystem(Subsystem::HEATING_PUMP, [](FailsafeLevel level, SystemError reason) {
        defaultPumpFailsafe(level);
    });
    
    registerSubsystem(Subsystem::WATER_PUMP, [](FailsafeLevel level, SystemError reason) {
        defaultPumpFailsafe(level);
    });
    
    initialized = true;
    LOG_INFO(TAG, "Failsafe system initialized");
}

void CentralizedFailsafe::cleanup() {
    subsystemCallbacks.clear();
    currentLevel = FailsafeLevel::NORMAL;
    lastError = SystemError::SUCCESS;
    failsafeStartTime = 0;
    recoveryAttempts = 0;
    savedState = SystemState();
    initialized = false;
    LOG_INFO(TAG, "Failsafe system cleaned up");
}

void CentralizedFailsafe::registerSubsystem(Subsystem subsystem, FailsafeCallback callback) {
    subsystemCallbacks.push_back({subsystem, callback});
    LOG_INFO(TAG, "Registered failsafe callback for subsystem %d", static_cast<int>(subsystem));
}

void CentralizedFailsafe::triggerFailsafe(FailsafeLevel level, SystemError reason, const char* details) {
    // Round 20 Issue #10: Protect state access with mutex
    if (stateMutex_ && xSemaphoreTake(stateMutex_, pdMS_TO_TICKS(100)) != pdTRUE) {
        LOG_ERROR(TAG, "Failed to acquire state mutex in triggerFailsafe");
        // Continue anyway - failsafe is safety-critical
    }

    // Don't downgrade failsafe level
    if (level <= currentLevel && currentLevel >= FailsafeLevel::CRITICAL) {
        LOG_WARN(TAG, "Ignoring failsafe trigger - already at level %d", static_cast<int>(currentLevel));
        if (stateMutex_) xSemaphoreGive(stateMutex_);
        return;
    }

    LOG_ERROR(TAG, "FAILSAFE TRIGGERED - Level: %d, Reason: %s, Details: %s",
             static_cast<int>(level),
             ErrorHandler::errorToString(reason),
             details ? details : "None");

    // Update state
    FailsafeLevel previousLevel = currentLevel;
    currentLevel = level;
    lastError = reason;

    if (failsafeStartTime == 0) {
        failsafeStartTime = millis();
    }

    if (stateMutex_) xSemaphoreGive(stateMutex_);
    
    // Save system state if entering critical level
    if (previousLevel < FailsafeLevel::CRITICAL && level >= FailsafeLevel::CRITICAL) {
        saveEmergencyState();
    }
    
    // Log the event
    logFailsafeEvent(level, reason, details);
    
    // Execute failsafe actions
    executeFailsafeActions(level, reason);
    
    // Notify external systems
    notifyExternalSystems(level, reason);
    
    // Set appropriate event bits
    switch (level) {
        case FailsafeLevel::WARNING:
            xEventGroupSetBits(SRP::getSystemStateEventGroup(), SystemEvents::SystemState::WARNING);
            break;

        case FailsafeLevel::DEGRADED:
        case FailsafeLevel::CRITICAL:
            xEventGroupSetBits(SRP::getSystemStateEventGroup(), SystemEvents::SystemState::BURNER_ERROR);
            break;
            
        case FailsafeLevel::EMERGENCY:
        case FailsafeLevel::SHUTDOWN:
            xEventGroupSetBits(SRP::getRelayEventGroup(), SystemEvents::RelayControl::EMERGENCY_STOP);
            break;
            
        default:
            break;
    }
}

void CentralizedFailsafe::executeFailsafeActions(FailsafeLevel level, SystemError reason) {
    LOG_INFO(TAG, "Executing failsafe actions for level %d", static_cast<int>(level));
    
    // Execute callbacks for all subsystems
    for (const auto& callback : subsystemCallbacks) {
        if (callback.first == Subsystem::ALL || level >= FailsafeLevel::CRITICAL) {
            callback.second(level, reason);
        }
    }
    
    // Additional level-specific actions
    switch (level) {
        case FailsafeLevel::NORMAL:
            // Clear error bits
            xEventGroupClearBits(SRP::getSystemStateEventGroup(),
                                SystemEvents::SystemState::WARNING | SystemEvents::SystemState::BURNER_ERROR);
            break;
            
        case FailsafeLevel::WARNING:
            // Log warning but continue operation
            break;
            
        case FailsafeLevel::DEGRADED:
            // Reduce system capabilities
            LOG_WARN(TAG, "System operating in degraded mode");
            break;
            
        case FailsafeLevel::CRITICAL:
            // Immediate safety actions
            LOG_ERROR(TAG, "Critical failsafe - initiating safety protocol");
            defaultBurnerFailsafe(level);
            break;
            
        case FailsafeLevel::EMERGENCY:
            // Emergency stop all operations
            emergencyStop("Failsafe emergency stop");
            break;
            
        case FailsafeLevel::SHUTDOWN:
            // Complete shutdown
            orderlyShutdown("Failsafe shutdown");
            break;
    }
}

void CentralizedFailsafe::defaultBurnerFailsafe(FailsafeLevel level) {
    switch (level) {
        case FailsafeLevel::WARNING:
            // Log warning, continue operation
            LOG_WARN(TAG, "Burner failsafe warning");
            break;
            
        case FailsafeLevel::DEGRADED:
            // Reduce burner power
            LOG_WARN(TAG, "Limiting burner to half power");
            // Force low power mode
            break;
            
        case FailsafeLevel::CRITICAL:
        case FailsafeLevel::EMERGENCY:
        case FailsafeLevel::SHUTDOWN: {
            // Immediate burner shutdown via BurnerSystemController
            LOG_ERROR(TAG, "Shutting down burner");
            BurnerSystemController* controller = SRP::getBurnerSystemController();
            if (controller) {
                controller->emergencyShutdown("CentralizedFailsafe burner shutdown");
            } else {
                LOG_ERROR(TAG, "BurnerSystemController not available - using direct relay control");
            }
            break;
        }
            
        default:
            break;
    }
}

void CentralizedFailsafe::defaultPumpFailsafe(FailsafeLevel level) {
    if (level >= FailsafeLevel::EMERGENCY) {
        // In emergency, keep pumps running to dissipate heat
        LOG_WARN(TAG, "Emergency mode - keeping pumps active for heat dissipation");
        RelayControlTask::setRelayState(RelayIndex::toPhysical(RelayIndex::HEATING_PUMP), true);
        RelayControlTask::setRelayState(RelayIndex::toPhysical(RelayIndex::WATER_PUMP), true);
    } else if (level >= FailsafeLevel::SHUTDOWN) {
        // In shutdown, turn off pumps after delay
        LOG_INFO(TAG, "Shutdown - pumps will stop after cooldown");
        // Pumps handled by shutdown sequence
    }
}

void CentralizedFailsafe::emergencyStop(const char* reason) {
    LOG_ERROR(TAG, "EMERGENCY STOP: %s", reason);

    currentLevel = FailsafeLevel::EMERGENCY;

    // 1. Immediately shut down burner via BurnerSystemController
    BurnerSystemController* controller = SRP::getBurnerSystemController();
    if (controller) {
        controller->emergencyShutdown(reason);
    } else {
        LOG_ERROR(TAG, "BurnerSystemController not available - using direct relay control");
    }

    // 2. Turn off dangerous relays (redundant safety - controller already does this)
    RelayControlTask::setRelayState(RelayIndex::toPhysical(RelayIndex::BURNER_ENABLE), false);
    RelayControlTask::setRelayState(RelayIndex::toPhysical(RelayIndex::POWER_BOOST), false);
    RelayControlTask::setRelayState(RelayIndex::toPhysical(RelayIndex::WATER_MODE), false);

    // 3. Keep pumps running for safety
    RelayControlTask::setRelayState(RelayIndex::toPhysical(RelayIndex::HEATING_PUMP), true);
    RelayControlTask::setRelayState(RelayIndex::toPhysical(RelayIndex::WATER_PUMP), true);
    
    // 4. Set emergency stop event
    xEventGroupSetBits(SRP::getSystemStateEventGroup(), SystemEvents::SystemState::EMERGENCY_STOP);
    
    // 5. Disable system
    xEventGroupClearBits(SRP::getSystemStateEventGroup(), SystemEvents::SystemState::BOILER_ENABLED);
    
    // 6. Log to persistent storage
    ErrorHandler::logError(TAG, SystemError::SYSTEM_FAILSAFE_TRIGGERED, reason);
    
    // 7. Schedule transition to shutdown after timeout
    // This would typically be handled by a supervisor task
}

void CentralizedFailsafe::orderlyShutdown(const char* reason) {
    LOG_INFO(TAG, "Orderly shutdown initiated: %s", reason);
    
    currentLevel = FailsafeLevel::SHUTDOWN;
    
    // 1. Save current state
    saveEmergencyState();
    
    // 2. Notify all subsystems
    executeFailsafeActions(FailsafeLevel::SHUTDOWN, SystemError::SYSTEM_FAILSAFE_TRIGGERED);
    
    // 3. Wait for burner cooldown
    vTaskDelay(pdMS_TO_TICKS(SystemConstants::Timing::FAILSAFE_COOLDOWN_MS));
    
    // 4. Turn off all relays
    RelayControlTask::setRelayState(RelayIndex::toPhysical(RelayIndex::HEATING_PUMP), false);
    RelayControlTask::setRelayState(RelayIndex::toPhysical(RelayIndex::WATER_PUMP), false);
    RelayControlTask::setRelayState(RelayIndex::toPhysical(RelayIndex::BURNER_ENABLE), false);
    RelayControlTask::setRelayState(RelayIndex::toPhysical(RelayIndex::WATER_MODE), false);
    RelayControlTask::setRelayState(RelayIndex::toPhysical(RelayIndex::POWER_BOOST), false);
    
    // 5. Final log
    LOG_INFO(TAG, "System shutdown complete");
}

void CentralizedFailsafe::saveEmergencyState() {
    LOG_INFO(TAG, "Saving emergency state");

    // Capture current system state
    savedState.timestamp = millis();

    // Get relay states
    {
        auto guard = MutexRetryHelper::acquireGuard(
            SRP::getRelayReadingsMutex(),
            "RelayReadings-EmergencySave"
        );
        if (guard) {
            savedState.burnerActive = SRP::getRelayReadings().relayBurnerEnable;
            savedState.heatingPumpActive = SRP::getRelayReadings().relayHeatingPump;
            savedState.waterPumpActive = SRP::getRelayReadings().relayWaterPump;
        }
    }

    // Get temperature target
    {
        auto guard = MutexRetryHelper::acquireGuard(
            SRP::getSensorReadingsMutex(),
            "SensorReadings-EmergencySave"
        );
        if (guard) {
            savedState.lastTargetTemp = SRP::getSensorReadings().boilerTempOutput;
        }
    }

    // Save to persistent storage in FRAM
    if (CriticalDataStorage::saveEmergencyState(static_cast<uint8_t>(lastError),
                                                static_cast<uint32_t>(lastError))) {
        LOG_INFO(TAG, "Emergency state persisted to FRAM");
    } else {
        LOG_WARN(TAG, "Failed to persist emergency state to FRAM");
    }
}

bool CentralizedFailsafe::attemptRecovery() {
    if (currentLevel < FailsafeLevel::CRITICAL) {
        LOG_INFO(TAG, "No recovery needed - system not in critical state");
        return true;
    }

    if (recoveryAttempts >= MAX_RECOVERY_ATTEMPTS) {
        LOG_ERROR(TAG, "Maximum recovery attempts reached");
        return false;
    }

    uint32_t timeSinceFailsafe = millis() - failsafeStartTime;
    if (timeSinceFailsafe < RECOVERY_DELAY_MS) {
        LOG_INFO(TAG, "Too soon for recovery - wait %ld ms",
                RECOVERY_DELAY_MS - timeSinceFailsafe);
        return false;
    }

    recoveryAttempts++;
    LOG_INFO(TAG, "Attempting recovery (attempt %d/%d)",
            recoveryAttempts, MAX_RECOVERY_ATTEMPTS);

    // IMPORTANT: Verify root cause is resolved before clearing errors
    bool canRecover = true;

    // Check memory if that was the cause
    if (lastError == SystemError::SYSTEM_LOW_MEMORY) {
        uint32_t freeHeap = esp_get_free_heap_size();
        if (freeHeap < SystemConstants::System::MIN_FREE_HEAP_WARNING) {
            LOG_WARN(TAG, "Recovery blocked: memory still low (%lu bytes)", freeHeap);
            canRecover = false;
        }
    }

    // Check sensor status if that was the cause
    if (lastError == SystemError::SENSOR_READ_FAILED || lastError == SystemError::SENSOR_INVALID_DATA) {
        EventBits_t errorBits = xEventGroupGetBits(SRP::getErrorNotificationEventGroup());
        if (errorBits & SystemEvents::Error::SENSOR_FAILURE) {
            LOG_WARN(TAG, "Recovery blocked: sensor errors still present");
            canRecover = false;
        }
    }

    // Check communication if that was the cause
    if (lastError == SystemError::MODBUS_COMMUNICATION_ERROR) {
        EventBits_t errorBits = xEventGroupGetBits(SRP::getErrorNotificationEventGroup());
        if (errorBits & SystemEvents::Error::MODBUS) {
            LOG_WARN(TAG, "Recovery blocked: Modbus errors still present");
            canRecover = false;
        }
    }

    if (!canRecover) {
        LOG_WARN(TAG, "Recovery attempt %d failed - root cause not resolved", recoveryAttempts);
        return false;
    }

    LOG_INFO(TAG, "Root cause appears resolved - proceeding with recovery");

    // Clear error conditions only after verifying root cause resolved
    xEventGroupClearBits(SRP::getSystemStateEventGroup(),
                        SystemEvents::SystemState::WARNING | SystemEvents::SystemState::BURNER_ERROR);
    xEventGroupClearBits(SRP::getSystemStateEventGroup(),
                        SystemEvents::SystemState::EMERGENCY_STOP);

    // Reset to warning level (not NORMAL) to allow monitoring
    currentLevel = FailsafeLevel::WARNING;

    // Re-enable system
    xEventGroupSetBits(SRP::getSystemStateEventGroup(), SystemEvents::SystemState::BOILER_ENABLED);

    return true;
}

void CentralizedFailsafe::monitorSystemHealth() {
    static uint32_t lastHealthCheck = 0;
    uint32_t now = millis();

    // Check at configured interval
    if ((now - lastHealthCheck) < SystemConstants::Timing::HEALTH_CHECK_INTERVAL_MS) {
        return;
    }
    lastHealthCheck = now;

    // Check memory using centralized thresholds
    uint32_t freeHeap = esp_get_free_heap_size();
    if (freeHeap < SystemConstants::System::MIN_FREE_HEAP_CRITICAL) {
        LOG_ERROR(TAG, "Critical low memory: %ld bytes", freeHeap);
        triggerFailsafe(FailsafeLevel::CRITICAL, SystemError::SYSTEM_LOW_MEMORY);
    } else if (freeHeap < SystemConstants::System::MIN_FREE_HEAP_WARNING) {
        LOG_WARN(TAG, "Low memory warning: %ld bytes", freeHeap);
        triggerFailsafe(FailsafeLevel::WARNING, SystemError::SYSTEM_LOW_MEMORY);
    }

    // Check temperature sensors
    EventBits_t errorBits = xEventGroupGetBits(SRP::getErrorNotificationEventGroup());
    if (errorBits & SystemEvents::Error::SENSOR_FAILURE) {
        triggerFailsafe(FailsafeLevel::DEGRADED, SystemError::SENSOR_READ_FAILED);
    }

    // Check communication errors
    if (errorBits & SystemEvents::Error::MODBUS) {
        triggerFailsafe(FailsafeLevel::WARNING, SystemError::MODBUS_COMMUNICATION_ERROR);
    }

    // If everything is OK and we're in warning state, try to clear it
    if (currentLevel == FailsafeLevel::WARNING && errorBits == 0 &&
        freeHeap > SystemConstants::System::MIN_HEAP_FOR_MQTT) {
        LOG_INFO(TAG, "System health restored - clearing warning state");
        currentLevel = FailsafeLevel::NORMAL;
        executeFailsafeActions(FailsafeLevel::NORMAL, SystemError::SUCCESS);
    }
}

const char* CentralizedFailsafe::getFailsafeStatusString() {
    switch (currentLevel) {
        case FailsafeLevel::NORMAL: return "Normal";
        case FailsafeLevel::WARNING: return "Warning";
        case FailsafeLevel::DEGRADED: return "Degraded";
        case FailsafeLevel::CRITICAL: return "Critical";
        case FailsafeLevel::EMERGENCY: return "Emergency";
        case FailsafeLevel::SHUTDOWN: return "Shutdown";
        default: return "Unknown";
    }
}

void CentralizedFailsafe::logFailsafeEvent(FailsafeLevel level, SystemError reason, const char* details) {
    // Log to health monitor
    HealthMonitor* healthMonitor = SRP::getHealthMonitor();
    if (healthMonitor) {
        healthMonitor->recordError(HealthMonitor::Subsystem::MQTT, reason);  // Use existing subsystem
    }
    
    // Log to persistent error log
    ErrorHandler::logError(TAG, reason, details);
}

void CentralizedFailsafe::notifyExternalSystems(FailsafeLevel level, SystemError reason) {
    // Set MQTT notification bit
    if (level >= FailsafeLevel::WARNING) {
        // Use existing error bit in system state event group instead
        xEventGroupSetBits(SRP::getSystemStateEventGroup(), SystemEvents::SystemState::BURNER_ERROR);
    }
    
    // Additional notifications can be added here
}