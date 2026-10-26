// src/modules/control/BurnerStateMachine.h
#ifndef BURNER_STATE_MACHINE_H
#define BURNER_STATE_MACHINE_H

#include "utils/StateMachine.h"
#include "modules/control/BurnerSystemController.h"
#include "modules/control/BurnerSMState.h"
#include "modules/control/BurnerTransitions.h"
#include "modules/control/BurnerDemandGate.h"
#include "config/SystemConstants.h"
#include "shared/SharedSensorReadings.h"
#include "shared/Temperature.h"
#include "events/SystemEventsGenerated.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

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
    static constexpr uint32_t LOCKOUT_TIME_MS = SystemConstants::Burner::LOCKOUT_TIME_MS;
    static constexpr uint8_t MAX_IGNITION_RETRIES = SystemConstants::Burner::MAX_IGNITION_RETRIES;

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
     * @return false if not applied: demand ON without permission (setDemandPermission)
     *         or demand mutex timeout
     */
    static bool setHeatDemand(bool demand, Temperature_t target = 0, bool highPower = false);

    /**
     * @brief Publish BurnerControlTask's arming permission (BurnerDemandGate)
     *
     * setHeatDemand(true) is refused under demandMutex while not permitted. A revoke
     * is stored before the caller's setHeatDemand(false), so an arm racing it is
     * either refused or overwritten by that OFF.
     */
    static void setDemandPermission(const BurnerDemandGate::Permission& permission);

    /**
     * @brief Current arming permission (lock-free)
     */
    static BurnerDemandGate::Permission getDemandPermission();

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
     * @brief Get current heat demand state including the requested power (thread-safe)
     * @param outHighPower Output: high power requested
     * @return true if values were successfully retrieved
     */
    static bool getHeatDemandState(bool& outDemand, Temperature_t& outTarget, bool& outHighPower);

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
    static void handlePowerLevelFault(bool high);

    // Helper functions
    // Round 21: Most helper functions moved to BurnerSafetyChecks, BurnerPowerController, BurnerRuntimeTracker

    // Stage B: the demand-driven states delegate to BurnerTransitions::step()
    static BurnerSMState runTransitionStep();
    static void logTransitionDecision(const BurnerTransitions::Context& ctx,
                                      const BurnerTransitions::Decision& decision,
                                      uint32_t systemBits, uint32_t requestBits);

    static void logStateTransition(BurnerSMState from, BurnerSMState to);
};

#endif // BURNER_STATE_MACHINE_H
