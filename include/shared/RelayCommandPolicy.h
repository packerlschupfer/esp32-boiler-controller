// include/shared/RelayCommandPolicy.h
#ifndef RELAY_COMMAND_POLICY_H
#define RELAY_COMMAND_POLICY_H

#include <cstdint>
#include "config/RelayIndices.h"

/**
 * @brief Decision rules for single relay commands (header-only, native-testable).
 *
 * BurnerSystemController::executeRelayBatch() sends all three burner relays on
 * every mode switch, including relays whose state does not change. The rate
 * limiter (MIN_RELAY_SWITCH_INTERVAL_MS, MAX_RELAY_TOGGLE_RATE_PER_MIN) counted
 * those no-op commands as toggles, so a genuine power-boost change 2 ms after a
 * HEATING -> WATER switch was rejected and escalated to an emergency stop
 * (2026-09-14 15:41:25).
 *
 * RelayControlTask::setRelayState() / processSingleRelay() use these functions for
 * the whole per-relay decision. Relay numbers are 1-based (physical), the
 * RelayState::desired bitmask and the limiter arrays are 0-based.
 */
namespace RelayCommandPolicy {

    // Physical relay numbers are 1..RelayIndex::MAX_RELAYS
    inline bool isValidRelay(uint8_t relayIndex) {
        return relayIndex >= 1 && relayIndex <= RelayIndex::MAX_RELAYS;
    }

    // Desired state of a 1-based relay in the RelayState::desired bitmask
    // (same as g_relayState.getRelay(relayIndex - 1)); false for an invalid relay
    inline bool desiredBit(uint8_t desiredMask, uint8_t relayIndex) {
        if (!isValidRelay(relayIndex)) {
            return false;
        }
        return (desiredMask & (1 << (relayIndex - 1))) != 0;
    }

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

    inline bool isSafetyCriticalRelay(uint8_t relayIndex) {
        return relayIndex == RelayIndex::toPhysical(RelayIndex::BURNER_ENABLE) ||
               relayIndex == RelayIndex::toPhysical(RelayIndex::POWER_BOOST) ||
               relayIndex == RelayIndex::toPhysical(RelayIndex::WATER_MODE);
    }

    // setRelayState() skips a command for the desired state, but never a redundant
    // OFF to a burner relay (F26: re-assert even if the caches diverged).
    inline bool skipDuplicate(uint8_t relayIndex, bool desiredState, bool requestedState) {
        return desiredState == requestedState &&
               !(requestedState == false && isSafetyCriticalRelay(relayIndex));
    }

    struct RateLimiter {
        uint32_t minIntervalTicks;     // pdMS_TO_TICKS(MIN_RELAY_SWITCH_INTERVAL_MS)
        uint32_t maxTogglesPerWindow;  // MAX_RELAY_TOGGLE_RATE_PER_MIN
        uint32_t* lastToggleTicks;     // [8] tick of the last counted toggle, 0 = none
        uint32_t* toggleCount;         // [8] cleared by updateRateLimitCounters() every minute
    };

    // Counts the toggle only when it is allowed; an invalid relay is never allowed
    inline bool consumeToggle(const RateLimiter& limiter, uint8_t relayIndex, uint32_t nowTicks) {
        if (!isValidRelay(relayIndex)) {
            return false;
        }
        const uint8_t idx = relayIndex - 1;

        // Minimum interval
        if (limiter.lastToggleTicks[idx] != 0) {
            const uint32_t elapsed = nowTicks - limiter.lastToggleTicks[idx];
            if (elapsed < limiter.minIntervalTicks) {
                return false;
            }
        }

        // Toggles per window
        if (limiter.toggleCount[idx] >= limiter.maxTogglesPerWindow) {
            return false;
        }

        limiter.lastToggleTicks[idx] = nowTicks;
        limiter.toggleCount[idx]++;
        return true;
    }

    enum class Admission : uint8_t {
        ACCEPTED,
        RATE_LIMITED,
        PUMP_PROTECTED
    };

    /**
     * @brief processSingleRelay(): may a command for a valid relay (1-8) be queued?
     *
     * No-op and emergency commands skip both protections. The no-op check comes
     * first so an unchanged relay never counts as a toggle. A real change is counted
     * by the rate limiter before pump motor protection is asked (also when that
     * then refuses it).
     *
     * @param pumpProtectionAllows (relayIndex, requestedState), not null
     */
    inline Admission admit(uint8_t relayIndex, bool desiredState, bool requestedState,
                           bool emergencyBypass, const RateLimiter& limiter, uint32_t nowTicks,
                           bool (*pumpProtectionAllows)(uint8_t, bool)) {
        if (!appliesProtection(desiredState, requestedState, emergencyBypass)) {
            return Admission::ACCEPTED;
        }
        if (!consumeToggle(limiter, relayIndex, nowTicks)) {
            return Admission::RATE_LIMITED;
        }
        if (!pumpProtectionAllows(relayIndex, requestedState)) {
            return Admission::PUMP_PROTECTED;
        }
        return Admission::ACCEPTED;
    }

    constexpr int8_t NO_PUMP = -1;

    /**
     * @brief After a queued command: restart the pump motor protection window
     *
     * Only on a real state change, otherwise re-sent unchanged pump commands keep
     * restarting the protection window and block the next genuine change.
     *
     * @param pumpLastChangeTicks [2] heating pump (relay 5), water pump (relay 6)
     * @return updated slot (0 heating, 1 water) or NO_PUMP
     */
    inline int8_t restartPumpTimer(uint8_t relayIndex, bool realChange, uint32_t nowTicks,
                                   uint32_t* pumpLastChangeTicks) {
        if (!realChange) {
            return NO_PUMP;
        }
        if (relayIndex == RelayIndex::toPhysical(RelayIndex::HEATING_PUMP)) {
            pumpLastChangeTicks[0] = nowTicks;
            return 0;
        }
        if (relayIndex == RelayIndex::toPhysical(RelayIndex::WATER_PUMP)) {
            pumpLastChangeTicks[1] = nowTicks;
            return 1;
        }
        return NO_PUMP;
    }

    constexpr uint32_t PUMP_REQUEST_RESEND_MS = 2000;

    /**
     * @brief Re-send a pump relay request while the relay does not follow.
     *
     * Relay request bits are cleared whether or not RelayControlTask accepted the
     * command, and PumpControlModule only requested on its own state changes. A change
     * refused by pump motor protection (15 s) was lost: heating restarting within 15 s
     * of a pump OFF left the heating pump off while the burner fired, and pumps switched
     * ON directly by CentralizedFailsafe::emergencyStop() never got their OFF
     * (review 2026-09-14).
     */
    inline bool pumpRequestResendDue(bool pumpWantedOn, bool relayDesiredOn,
                                     uint32_t nowMs, uint32_t lastRequestMs) {
        return pumpWantedOn != relayDesiredOn &&
               static_cast<uint32_t>(nowMs - lastRequestMs) >= PUMP_REQUEST_RESEND_MS;
    }

} // namespace RelayCommandPolicy

#endif // RELAY_COMMAND_POLICY_H
