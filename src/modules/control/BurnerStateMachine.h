// src/modules/control/BurnerStateMachine.h
#ifndef BURNER_STATE_MACHINE_H
#define BURNER_STATE_MACHINE_H

#include "utils/StateMachine.h"
#include "modules/control/BurnerSystemController.h"
#include "config/SystemConstants.h"
#include "shared/SharedSensorReadings.h"
#include "shared/Temperature.h"
#include "events/SystemEventsGenerated.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

/**
 * @brief Enhanced burner states for state machine
 */
enum class BurnerSMState {
    IDLE,               // Burner off, waiting for demand
    PRE_PURGE,          // Pre-purge sequence before ignition
    IGNITION,           // Ignition sequence
    RUNNING_LOW,        // Running at low power
    RUNNING_HIGH,       // Running at high power
    MODE_SWITCHING,     // Seamless mode transition (water ↔ heating)
    POST_PURGE,         // Post-purge after shutdown
    LOCKOUT,            // Safety lockout state
    ERROR               // Error state
};

/**
 * @brief Burner state machine implementation
 */
class BurnerStateMachine {
private:
    static StateMachine<BurnerSMState> stateMachine;
    static const char* TAG;
    
    // Use timing constants from SystemConstants::Burner
    static constexpr uint32_t PRE_PURGE_TIME_MS = SystemConstants::Burner::PRE_PURGE_TIME_MS;
    static constexpr uint32_t IGNITION_TIME_MS = SystemConstants::Burner::IGNITION_TIME_MS;
    static constexpr uint32_t POST_PURGE_TIME_MS = SystemConstants::Burner::POST_PURGE_TIME_MS;
    static constexpr uint32_t LOCKOUT_TIME_MS = SystemConstants::Burner::LOCKOUT_TIME_MS;
    static constexpr uint8_t MAX_IGNITION_RETRIES = SystemConstants::Burner::MAX_IGNITION_RETRIES;
    
    // Retry counters
    static uint8_t ignitionRetries;
    
    // Demand tracking (protected by demandMutex)
    static bool heatDemand;
    static Temperature_t targetTemperature;
    static bool requestedHighPower;  // PID-driven power level request
    static SemaphoreHandle_t demandMutex;

    // Mutex timeout for demand operations - uses centralized constant from SystemConstants
    static constexpr TickType_t DEMAND_MUTEX_TIMEOUT = pdMS_TO_TICKS(SystemConstants::Timing::MUTEX_DEFAULT_TIMEOUT_MS);
    
public:
    /**
     * @brief Initialize the burner state machine
     */
    static void initialize();
    
    /**
     * @brief Update the state machine (call from task)
     */
    static void update();
    
    /**
     * @brief Set heat demand with PID-driven power level
     * @param demand True if heat is requested
     * @param target Target temperature (fixed-point, tenths of °C)
     * @param highPower True for high power (full), false for low power (half)
     */
    static void setHeatDemand(bool demand, Temperature_t target = 0, bool highPower = false);
    
    /**
     * @brief Emergency stop
     */
    static void emergencyStop();
    
    /**
     * @brief Get current state
     */
    static BurnerSMState getCurrentState();

    /**
     * @brief Get current heat demand state (thread-safe)
     * @param outDemand Output: current heat demand
     * @param outTarget Output: current target temperature
     * @return true if values were successfully retrieved
     */
    static bool getHeatDemandState(bool& outDemand, Temperature_t& outTarget);

    /**
     * @brief Reset from lockout
     */
    static void resetLockout();
    
private:
    // State handlers
    static BurnerSMState handleIdleState();
    static BurnerSMState handlePrePurgeState();
    static BurnerSMState handleIgnitionState();
    static BurnerSMState handleRunningLowState();
    static BurnerSMState handleRunningHighState();
    static BurnerSMState handleModeSwitchingState();
    static BurnerSMState handlePostPurgeState();
    static BurnerSMState handleLockoutState();
    static BurnerSMState handleErrorState();

    // Entry/Exit actions
    static void onEnterPrePurge();
    static void onEnterIgnition();
    static void onEnterRunningLow();
    static void onEnterRunningHigh();
    static void onEnterModeSwitching();
    static void onEnterPostPurge();
    static void onEnterLockout();
    static void onEnterError();
    static void onExitLockout();
    static void onExitRunning();

    // Helper functions

    /**
     * @brief Check if flame is detected
     * @return true if flame detected (or assumed present)
     *
     * HARDWARE LIMITATION: No flame sensor installed.
     * Currently returns burner relay state as proxy.
     * When flame sensor hardware is added, implement actual GPIO read.
     */
    static bool isFlameDetected();
    static bool checkSafetyConditions();
    static bool shouldIncreasePower();
    static bool shouldDecreasePower();

    /**
     * @brief Check if seamless mode switch is safe
     * @return true if mode can be switched without shutdown
     *
     * Validates conditions for seamless water ↔ heating transition:
     * - Currently in RUNNING_LOW or RUNNING_HIGH
     * - Safety conditions pass
     * - Flame detected
     *
     * Note: Does NOT check heatDemand because old mode clears demand
     * before new mode sets it. MODE_SWITCHING handler validates new demand.
     */
    static bool canSeamlesslySwitch();

    /**
     * @brief Check for mode switch and return appropriate transition state
     * @param currentStateName Name of current state for logging (e.g., "RUNNING_LOW")
     * @return MODE_SWITCHING, POST_PURGE if switch detected, or IDLE as sentinel for no transition
     *
     * M1: Extracted common logic from handleRunningLowState/handleRunningHighState
     */
    static BurnerSMState checkModeSwitchTransition(const char* currentStateName);

    /**
     * @brief Check safety shutdown conditions
     * @param currentState Current state for sentinel return
     * @return POST_PURGE if shutdown needed, currentState otherwise
     *
     * Extracted common logic from handleRunningLowState/handleRunningHighState
     */
    static BurnerSMState checkSafetyShutdown(BurnerSMState currentState);

    /**
     * @brief Check flame loss conditions
     * @param currentState Current state for sentinel return
     * @return POST_PURGE if flame lost, currentState otherwise
     *
     * Extracted common logic from handleRunningLowState/handleRunningHighState
     */
    static BurnerSMState checkFlameLoss(BurnerSMState currentState);

    static void logStateTransition(BurnerSMState from, BurnerSMState to);
};

#endif // BURNER_STATE_MACHINE_H