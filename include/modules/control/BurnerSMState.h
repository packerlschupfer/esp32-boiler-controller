// include/modules/control/BurnerSMState.h
#ifndef BURNER_SM_STATE_H
#define BURNER_SM_STATE_H

/**
 * @brief Enhanced burner states for state machine
 *
 * Kept free of FreeRTOS/Arduino headers so the transition logic
 * (BurnerTransitions.h) can be tested natively.
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
 * @brief Is the gas valve open in this state - i.e. could a flame be burning?
 *
 * ONE definition, because this question was being answered independently at six
 * sites and two of them had different answers. The 2026-10-30 diff review found
 * BoilerTempControlTask resyncing the controller's flame bit as "burner out" during
 * MODE_SWITCHING, which running() enters ONLY when flameDetected() is true
 * (BurnerTransitions.h:252) - so a seamless heating->water handover reset the dead
 * band to its ON edge mid-burn, disarmed demand, and ba30452's
 * "!heatDemand -> POST_PURGE" branch then cut a perfectly good flame.
 *
 * MODE_SWITCHING is the state everyone forgets: it is a RUNNING state wearing a
 * different name. If a new lit state is ever added, add it here and nowhere else.
 *
 * PRE_PURGE and POST_PURGE are deliberately NOT lit - the fan runs, the valve does
 * not. IGNITION is lit because the valve opens during it.
 */
inline bool burnerLit(BurnerSMState s) {
    switch (s) {
        case BurnerSMState::IGNITION:
        case BurnerSMState::RUNNING_LOW:
        case BurnerSMState::RUNNING_HIGH:
        case BurnerSMState::MODE_SWITCHING:
            return true;
        case BurnerSMState::IDLE:
        case BurnerSMState::PRE_PURGE:
        case BurnerSMState::POST_PURGE:
        case BurnerSMState::LOCKOUT:
        case BurnerSMState::ERROR:
            return false;
    }
    return false;  // unreachable; no default, so a new enumerator is a compile warning
}

#endif // BURNER_SM_STATE_H
