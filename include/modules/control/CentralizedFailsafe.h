#ifndef CENTRALIZED_FAILSAFE_H
#define CENTRALIZED_FAILSAFE_H

#include <cstdint>
#include "modules/control/EmergencyStopRelease.h"
#include <atomic>
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
    static bool isInFailsafe() { return currentLevel.load() > FailsafeLevel::WARNING; }

    // Get current failsafe level
    static FailsafeLevel getCurrentLevel() { return currentLevel.load(); }

    /**
     * @brief May the burner use stage 2 (full power)?
     *
     * DEGRADED's documented action - "limiting burner to half power" - was a LOG LINE above an
     * empty comment reading "// Force low power mode" for the life of the project
     * (2026-10-25 review). This is what makes it true: BoilerTempControlTask ANDs it into the
     * stage-2 decision alongside the bang-bang's request, the state machine's hot-boiler
     * ceiling and BurnerBoostPolicy's shoulder-season lock.
     *
     * The level is STICKY: triggerFailsafe() never downgrades. THAT SENTENCE USED TO END "so a
     * fault must be cleared through a recovery path", and there was no such path for a bare
     * DEGRADED - every downgrade required a LATCHED emergency stop, and attemptRecovery() has
     * no caller. So this function, added the day before, turned a sticky level with no release
     * into a permanent half-power lock-out, reachable from a single refused relay write. Found
     * by the 2026-10-26 review; see clearDegradedForCause() for the release that now exists.
     */
    static bool fullPowerPermitted() { return currentLevel.load() < FailsafeLevel::DEGRADED; }

    /**
     * @brief Lower a DEGRADED level back to WARNING once the fault that raised it has resolved.
     *
     * CAUSE-MATCHED on purpose, and this is the whole design of it. A blanket "clear on any
     * success" would let one successful relay write clear a DEGRADED raised by sensor-mutex
     * timeouts or by heap pressure - the same mistake as clearing Error::RELAY on a single
     * relay's success instead of a fully clean verification (5521222).
     *
     * @param cause must equal the SystemError that raised the level, or nothing happens.
     * @return true if the level was lowered.
     */
    static bool clearDegradedForCause(SystemError cause);

    /// True once per level change, for the publisher. Same shape as
    /// ModbusBusRelease::consumeStateChange(), and for the same reason: the level changes in
    /// contexts that must not do MQTT or logging work.
    static bool consumeLevelChange();
    
    // Attempt system recovery
    static bool attemptRecovery();
    
    // Emergency stop - immediate shutdown
    static void emergencyStop(const char* reason,
                              EmergencyStopRelease::Cause cause = EmergencyStopRelease::Cause::OTHER);

    // Release a latched emergency stop (MQTT boiler/cmd/emergency_reset) once the
    // causes have cleared; restores BOILER_ENABLED from the saved setting
    static EmergencyStopRelease::Result clearEmergencyStop();

    // Sensor recovery: releases the latch only if stale sensor data caused it
    // (BOILER_ENABLED stays cleared); returns true if released
    static bool releaseAfterSensorRecovery();
    
    // Perform orderly shutdown
    static void orderlyShutdown(const char* reason);
    
    // Save critical state before shutdown
    static void saveEmergencyState();
    
    // Get failsafe status string
    static const char* getFailsafeStatusString();
    
private:
    // Round 20 Issue #10: Mutex to protect static state from concurrent access
    static SemaphoreHandle_t stateMutex_;

    // F28: atomic so the unguarded writes from emergencyStop/orderlyShutdown/
    // attemptRecovery/clearEmergencyStop (which run outside stateMutex_) cannot
    // tear against triggerFailsafe's guarded read-modify-write of the level.
    static std::atomic<FailsafeLevel> currentLevel;
    /// Set by setLevel() on every real change, consumed by the publisher.
    static std::atomic<bool> levelChanged_;

    /// The ONLY writer of currentLevel. Added so the change flag cannot be forgotten at one of
    /// the six sites that used to store it directly.
    static void setLevel(FailsafeLevel level);
    static std::atomic<EmergencyStopRelease::Cause> latchCause_;
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