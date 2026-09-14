// include/shared/RelayCommandPolicy.h
#ifndef RELAY_COMMAND_POLICY_H
#define RELAY_COMMAND_POLICY_H

/**
 * @brief Decision rules for single relay commands (header-only, native-testable).
 *
 * BurnerSystemController::executeRelayBatch() sends all three burner relays on
 * every mode switch, including relays whose state does not change. The rate
 * limiter (MIN_RELAY_SWITCH_INTERVAL_MS, MAX_RELAY_TOGGLE_RATE_PER_MIN) counted
 * those no-op commands as toggles, so a genuine power-boost change 2 ms after a
 * HEATING -> WATER switch was rejected and escalated to an emergency stop
 * (2026-09-14 15:41:25).
 */
namespace RelayCommandPolicy {

    // Command does not change the desired relay state: accept it without
    // touching rate limiting or pump motor protection.
    inline bool isNoOp(bool desiredState, bool requestedState) {
        return desiredState == requestedState;
    }

    // Rate limiting / pump protection apply only to real state changes that are
    // not emergency/failsafe commands.
    inline bool appliesProtection(bool desiredState, bool requestedState, bool emergencyBypass) {
        return !emergencyBypass && !isNoOp(desiredState, requestedState);
    }

} // namespace RelayCommandPolicy

#endif // RELAY_COMMAND_POLICY_H
