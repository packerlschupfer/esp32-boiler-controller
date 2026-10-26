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

    enum class ToggleGate : uint8_t {
        NOT_APPLICABLE,  // no-op or emergency: neither protection applies
        ALLOWED,         // real change, counted by the rate limiter; ask pump protection next
        RATE_LIMITED     // real change, refused by the rate limiter
    };

    /**
     * @brief The protection decision that must run under the rate-limiter lock.
     *
     * Split out of admit() because processSingleRelay() cannot hold its critical
     * section across the whole admission: checkPumpProtection() takes
     * relayStateMutex_ for up to 10 ms and blocking on a mutex inside
     * portENTER_CRITICAL is illegal. Production calls THIS between
     * portENTER_CRITICAL/portEXIT_CRITICAL and then does the pump step outside;
     * admit() below is written in terms of the same function, so the gate cannot
     * differ between the two paths - previously the sequence was retyped in
     * processSingleRelay() and again in the test, i.e. three copies of one
     * decision.
     *
     * Touches limiter state, so callers must serialise it.
     */
    inline ToggleGate gateToggle(uint8_t relayIndex, bool desiredState, bool requestedState,
                                 bool emergencyBypass, const RateLimiter& limiter,
                                 uint32_t nowTicks) {
        if (!appliesProtection(desiredState, requestedState, emergencyBypass)) {
            return ToggleGate::NOT_APPLICABLE;
        }
        return consumeToggle(limiter, relayIndex, nowTicks) ? ToggleGate::ALLOWED
                                                           : ToggleGate::RATE_LIMITED;
    }

    enum class Outcome : uint8_t {
        PROCEED,                 // queue the command
        REJECT_RATE_LIMITED,     // limiter refused a real change
        REJECT_PUMP_PROTECTED    // pump motor protection refused it
    };

    /**
     * @brief Must pump motor protection be consulted for this gate result?
     *
     * Kept separate from decide() so the caller can SKIP the call rather than pass its
     * result: checkPumpProtection() takes relayStateMutex_ for up to 10 ms, and asking it
     * about a no-op or an emergency command is the F13 hazard - an emergency pump-ON that
     * pump protection then refuses. Laziness is part of the contract, not an optimisation.
     */
    inline bool pumpCheckRequired(ToggleGate gate) {
        return gate == ToggleGate::ALLOWED;
    }

    /**
     * @brief The whole per-command verdict, given the gate and the pump answer.
     *
     * This is the last piece of processSingleRelay()'s decision that used to live in the
     * .cpp, where no native test could reach it (env:native_test cannot compile
     * RelayControlTask.cpp - its include cone pulls in RYN4, TaskManager, esp_task_wdt and
     * SystemResourceProvider). Hoisting it here leaves that function with NO decision of
     * its own: it locks, calls gateToggle(), optionally calls checkPumpProtection(), and
     * switches on this. Same pattern as BurnerTransitions::step() and WaterChargePolicy.
     *
     * @param pumpAllows result of checkPumpProtection(), or true when
     *        pumpCheckRequired(gate) is false and it was therefore not called
     */
    inline Outcome decide(ToggleGate gate, bool pumpAllows) {
        if (gate == ToggleGate::RATE_LIMITED) {
            return Outcome::REJECT_RATE_LIMITED;
        }
        if (pumpCheckRequired(gate) && !pumpAllows) {
            return Outcome::REJECT_PUMP_PROTECTED;
        }
        return Outcome::PROCEED;
    }

    /**
     * @brief gateToggle() + the pump-protection step, as one call. NO PRODUCTION CALLER.
     *
     * processSingleRelay() does not use this: it calls gateToggle() inside the
     * rate-limiter critical section and then checkPumpProtection() outside it, because
     * that function takes relayStateMutex_ and must not block inside
     * portENTER_CRITICAL. admit() exists so the two-step decision can be asserted
     * against a stated expectation table in the native tests, which cannot compile
     * RelayControlTask.cpp. Both paths share gateToggle(), so that part cannot drift;
     * the pump step here mirrors production by inspection only.
     *
     * No-op and emergency commands skip both protections. The no-op check comes first
     * so an unchanged relay never counts as a toggle. A real change is counted by the
     * rate limiter before pump motor protection is asked (also when that then refuses).
     *
     * @param pumpProtectionAllows (relayIndex, requestedState), not null
     */
    inline Admission admit(uint8_t relayIndex, bool desiredState, bool requestedState,
                           bool emergencyBypass, const RateLimiter& limiter, uint32_t nowTicks,
                           bool (*pumpProtectionAllows)(uint8_t, bool)) {
        const ToggleGate gate = gateToggle(relayIndex, desiredState, requestedState,
                                           emergencyBypass, limiter, nowTicks);
        const bool pumpAllows = pumpCheckRequired(gate)
                                    ? pumpProtectionAllows(relayIndex, requestedState)
                                    : true;
        switch (decide(gate, pumpAllows)) {
            case Outcome::REJECT_RATE_LIMITED: return Admission::RATE_LIMITED;
            case Outcome::REJECT_PUMP_PROTECTED: return Admission::PUMP_PROTECTED;
            case Outcome::PROCEED: return Admission::ACCEPTED;
        }
        return Admission::ACCEPTED;  // unreachable; keeps the compiler quiet
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
