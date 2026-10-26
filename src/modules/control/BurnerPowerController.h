// src/modules/control/BurnerPowerController.h
#ifndef BURNER_POWER_CONTROLLER_H
#define BURNER_POWER_CONTROLLER_H

/**
 * @brief Burner power level decision logic
 *
 * Extracted from BurnerStateMachine.cpp (Round 21 Refactoring).
 * Handles power increase/decrease decisions with temperature safety limits.
 *
 * Thread Safety:
 * - All functions are thread-safe using MutexRetryHelper
 * - Safe to call from BurnerStateMachine state handlers
 */
class BurnerPowerController {
public:
    /**
     * @brief Check if burner should increase to high power
     * @param requestedHighPower PID-driven power level request from control module
     * @return true if safe to increase to high power
     *
     * SAFETY CHECK: Blocks high power if boiler temp >= 80.0°C to prevent overshoot.
     * Uses PID-driven power request when temperature is safe.
     *
     * The solenoid gas valve can switch frequently, so this trusts PID calculations
     * while enforcing hard temperature limits for safety.
     */
    static bool shouldIncreasePower(bool requestedHighPower);

    /**
     * @brief Check if burner should decrease to low power
     * @param requestedHighPower PID-driven power level request from control module
     * @return true if should decrease to low power
     *
     * Uses PID-driven power level decision. The solenoid gas valve can switch
     * frequently, so this trusts PID's calculation.
     */
    static bool shouldDecreasePower(bool requestedHighPower);
};

#endif // BURNER_POWER_CONTROLLER_H
