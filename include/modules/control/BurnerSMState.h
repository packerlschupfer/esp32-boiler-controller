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

#endif // BURNER_SM_STATE_H
