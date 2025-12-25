#ifndef CENTRALIZED_FAILSAFE_H
#define CENTRALIZED_FAILSAFE_H

#include <cstdint>
#include <functional>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "utils/ErrorHandler.h"
#include "shared/Temperature.h"

/**
 * @brief Centralized failsafe system for coordinated emergency response
 * 
 * Provides a unified failsafe mechanism that ensures all subsystems
 * respond appropriately to critical failures.
 */
class CentralizedFailsafe {
public:
    // Failsafe levels
    enum class FailsafeLevel {
        NORMAL = 0,        // System operating normally
        WARNING = 1,       // Non-critical issues detected
        DEGRADED = 2,      // Operating with reduced functionality
        CRITICAL = 3,      // Critical issues - immediate action required
        EMERGENCY = 4,     // Emergency shutdown in progress
        SHUTDOWN = 5       // System shut down
    };
    
    // Subsystem identifiers
    enum class Subsystem {
        BURNER,
        HEATING_PUMP,
        WATER_PUMP,
        SENSORS,
        COMMUNICATION,
        POWER,
        MEMORY,
        ALL
    };
    
    // Failsafe action callback
    using FailsafeCallback = std::function<void(FailsafeLevel level, SystemError reason)>;
    
    // System state for recovery
    struct SystemState {
        bool burnerActive = false;
        bool heatingPumpActive = false;
        bool waterPumpActive = false;
        Temperature_t lastTargetTemp = 0;
        uint32_t timestamp = 0;
    };
    
    // Initialize the failsafe system
    static void initialize();

    // Cleanup for partial init recovery (clears callbacks and state)
    static void cleanup();
    
    // Register subsystem callback
    static void registerSubsystem(Subsystem subsystem, FailsafeCallback callback);
    
    // Trigger failsafe at specific level
    static void triggerFailsafe(FailsafeLevel level, SystemError reason, const char* details = nullptr);
    
    // Check if system is in failsafe mode
    static bool isInFailsafe() { return currentLevel > FailsafeLevel::WARNING; }
    
    // Get current failsafe level
    static FailsafeLevel getCurrentLevel() { return currentLevel; }
    
    // Attempt system recovery
    static bool attemptRecovery();
    
    // Emergency stop - immediate shutdown
    static void emergencyStop(const char* reason);
    
    // Perform orderly shutdown
    static void orderlyShutdown(const char* reason);
    
    // Save critical state before shutdown
    static void saveEmergencyState();
    
    // Monitor system health and trigger failsafe if needed
    static void monitorSystemHealth();
    
    // Get failsafe status string
    static const char* getFailsafeStatusString();
    
private:
    // Round 20 Issue #10: Mutex to protect static state from concurrent access
    static SemaphoreHandle_t stateMutex_;

    static FailsafeLevel currentLevel;
    static SystemError lastError;
    static uint32_t failsafeStartTime;
    static uint32_t recoveryAttempts;
    static SystemState savedState;
    static bool initialized;
    
    static constexpr uint8_t MAX_RECOVERY_ATTEMPTS = 3;
    static constexpr uint32_t RECOVERY_DELAY_MS = 30000; // 30 seconds
    static constexpr uint32_t EMERGENCY_TIMEOUT_MS = 5000; // 5 seconds for emergency shutdown
    
    // Subsystem callbacks
    static std::vector<std::pair<Subsystem, FailsafeCallback>> subsystemCallbacks;
    
    // Execute failsafe actions for all subsystems
    static void executeFailsafeActions(FailsafeLevel level, SystemError reason);
    
    // Default failsafe actions
    static void defaultBurnerFailsafe(FailsafeLevel level);
    static void defaultPumpFailsafe(FailsafeLevel level);
    static void defaultCommunicationFailsafe(FailsafeLevel level);
    
    // Log failsafe event
    static void logFailsafeEvent(FailsafeLevel level, SystemError reason, const char* details);
    
    // Notify external systems (MQTT, etc.)
    static void notifyExternalSystems(FailsafeLevel level, SystemError reason);
};

#endif // CENTRALIZED_FAILSAFE_H