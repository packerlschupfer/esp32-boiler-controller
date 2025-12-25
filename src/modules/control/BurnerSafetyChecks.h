// src/modules/control/BurnerSafetyChecks.h
#ifndef BURNER_SAFETY_CHECKS_H
#define BURNER_SAFETY_CHECKS_H

#include "utils/StateMachine.h"

// Forward declaration to avoid circular dependency
enum class BurnerSMState;

/**
 * @brief Burner safety validation and mode switch detection
 *
 * Extracted from BurnerStateMachine.cpp (Round 21 Refactoring).
 * Provides safety checks, flame detection, and mode switch logic.
 *
 * Thread Safety:
 * - All functions are thread-safe using SRP mutex guards
 * - Uses MutexRetryHelper for sensor data access
 * - Can be called from any task
 */
class BurnerSafetyChecks {
public:
    /**
     * @brief Check if flame is detected
     * @return true if flame detected (or assumed present via relay state proxy)
     *
     * HARDWARE LIMITATION: No flame sensor installed.
     * Currently returns burner relay state as proxy.
     * When flame sensor hardware is added, update implementation to read GPIO.
     */
    static bool isFlameDetected();

    /**
     * @brief Run safety validation
     * @return true if all safety conditions pass
     *
     * Calls BurnerSystemController::performSafetyCheck() to validate:
     * - Temperature limits
     * - Pressure limits
     * - Sensor staleness
     * - Emergency stop status
     */
    static bool checkSafetyConditions();

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
    static bool canSeamlesslySwitch(BurnerSMState currentState);

    /**
     * @brief Check for mode switch and return appropriate transition state
     * @param currentState Current burner state
     * @param currentStateName Name of current state for logging (e.g., "RUNNING_LOW")
     * @param runningModeIsWater Reference to current mode (updated if switch detected)
     * @return MODE_SWITCHING if seamless, POST_PURGE if shutdown needed, IDLE if no switch
     *
     * Detects mode transitions (water ↔ heating) by checking system event bits.
     * Uses WATER_PRIORITY as tiebreaker if both modes set simultaneously.
     */
    static BurnerSMState checkModeSwitchTransition(
        BurnerSMState currentState,
        const char* currentStateName,
        bool& runningModeIsWater
    );

    /**
     * @brief Check if burner should shut down due to safety or demand loss
     * @param currentState Current state for sentinel return
     * @param heatDemand Current heat demand state
     * @return POST_PURGE if shutdown needed, currentState otherwise
     *
     * Checks anti-flapping before allowing shutdown.
     * Logs delay time if anti-flapping prevents immediate stop.
     */
    static BurnerSMState checkSafetyShutdown(BurnerSMState currentState, bool heatDemand);

    /**
     * @brief Check for flame loss conditions
     * @param currentState Current state for sentinel return
     * @param heatDemand Current heat demand state
     * @return POST_PURGE if flame lost, currentState otherwise
     *
     * Differentiates intentional shutdown (no demand) from unexpected flame loss.
     * Both cases transition to POST_PURGE, bypassing anti-flapping for safety.
     */
    static BurnerSMState checkFlameLoss(BurnerSMState currentState, bool heatDemand);
};

#endif // BURNER_SAFETY_CHECKS_H
