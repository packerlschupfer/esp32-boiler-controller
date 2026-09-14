// src/modules/control/BurnerSafetyChecks.h
#ifndef BURNER_SAFETY_CHECKS_H
#define BURNER_SAFETY_CHECKS_H

#include "utils/StateMachine.h"

/**
 * @brief Burner safety validation
 *
 * Extracted from BurnerStateMachine.cpp (Round 21 Refactoring). The mode switch
 * and shutdown decisions moved to BurnerTransitions::step() (Stage B); these
 * checks are its firmware inputs.
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
     * @brief Check if a heating or water mode is active with a matching burner request
     * @return true if (HEATING_ON && HEATING request) || (WATER_ON && WATER request)
     *
     * The state machine's heatDemand is a latched copy set by BurnerControlTask /
     * BoilerTempControlTask. emergencyStop() does not clear it, so after an ERROR
     * recovery a stale demand could fire the burner although the owning control
     * task had already withdrawn its request and mode bit (incidents 2026-09-12/13:
     * heating relay on, HEATING_ON off, boiler to 90°C). Burner start and continued
     * operation therefore also require a live mode + request.
     *
     * Deliberately does NOT look at pump relays: pumps follow the mode bits via
     * PumpControlModule (and ReturnPreheater cycles the heating pump), the burner
     * never commands or requires them.
     */
    static bool hasActiveModeDemand();
};

#endif // BURNER_SAFETY_CHECKS_H
