// src/modules/tasks/RelayVerificationManager.h
#ifndef RELAY_VERIFICATION_MANAGER_H
#define RELAY_VERIFICATION_MANAGER_H

#include <cstdint>
#include <freertos/FreeRTOS.h>

/**
 * @brief Relay verification and health monitoring
 *
 * Extracted from RelayControlTask.cpp (Round 21 Refactoring).
 * Handles pump motor protection, relay health tracking, and failsafe escalation.
 *
 * Thread Safety:
 * - Uses relay state mutex for state array access
 * - Safe to call from RelayControlTask context
 */
class RelayVerificationManager {
public:
    /**
     * @brief Check pump motor protection before state change
     * @param relayIndex Physical relay index (1-8)
     * @param desiredState Desired relay state
     * @param relayStates Array of current relay states (8 elements)
     * @param relayStatesKnown True if relay states are known
     * @param pumpLastChangeTime Array of last change timestamps (2 elements: heating, water)
     * @return true if state change allowed, false if blocked by protection
     *
     * Enforces minimum delay between pump state changes to protect motor windings.
     * Only applies to heating pump (relay 5) and water pump (relay 6).
     */
    static bool checkPumpProtection(
        uint8_t relayIndex,
        bool desiredState,
        const bool* relayStates,
        bool relayStatesKnown,
        TickType_t* pumpLastChangeTime
    );

    /**
     * @brief Get remaining protection time for pump relay
     * @param relayIndex Physical relay index (1-8)
     * @param pumpLastChangeTime Array of last change timestamps (2 elements)
     * @return Milliseconds until pump can change state, 0 if no protection active
     */
    static uint32_t getPumpProtectionTimeRemaining(
        uint8_t relayIndex,
        const TickType_t* pumpLastChangeTime
    );

    /**
     * @brief Track relay health and escalate to failsafe if needed
     * @param relayIndex Physical relay index (1-8)
     * @param success True if relay operation succeeded
     * @param consecutiveFailures Array of consecutive failure counters (8 elements)
     *
     * Tracks consecutive relay failures. After MAX_CONSECUTIVE_FAILURES:
     * - Burner relay (index 0): Triggers CRITICAL failsafe (emergency shutdown)
     * - Other relays: Triggers WARNING failsafe (monitoring only)
     */
    static void checkRelayHealthAndEscalate(
        uint8_t relayIndex,
        bool success,
        uint8_t* consecutiveFailures
    );

private:
    // Maximum consecutive failures before escalation
    static constexpr uint8_t MAX_CONSECUTIVE_FAILURES = 3;
};

#endif // RELAY_VERIFICATION_MANAGER_H
