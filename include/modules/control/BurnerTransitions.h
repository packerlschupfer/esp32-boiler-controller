// include/modules/control/BurnerTransitions.h
#ifndef BURNER_TRANSITIONS_H
#define BURNER_TRANSITIONS_H

#include <cstdint>

#include "modules/control/BurnerSMState.h"
#include "modules/control/BurnerTransitionPolicy.h"

/**
 * @brief Burner state machine transition logic (Stage B, 2026-09-14).
 *
 * step() decides the next state for the demand-driven states (IDLE, PRE_PURGE,
 * IGNITION, RUNNING_LOW/HIGH, MODE_SWITCHING) and the early restart from
 * POST_PURGE. Header-only and FreeRTOS-free so
 * whole scenarios can be replayed in native tests.
 *
 * Inputs are read through Environment on demand, in the same order and under the
 * same conditions as the former state handlers: some probes have side effects
 * (BurnerSystemController::performSafetyCheck() performs an emergency shutdown on
 * over-temperature) or take mutexes, so they must not be evaluated eagerly.
 *
 * Timeout transitions (PRE_PURGE -> IGNITION, IGNITION -> LOCKOUT) stay in the
 * StateMachine configuration; entry/exit actions, the post-purge duration and the
 * LOCKOUT and ERROR handlers stay in BurnerStateMachine.
 */
namespace BurnerTransitions {

    // Burner running without any active heating/water mode request: stop after this
    // grace period (during a handover the old mode clears its bits before the new
    // mode sets them).
    constexpr uint32_t MODE_DEMAND_LOSS_GRACE_MS = 10000;

    class Environment {
    public:
        virtual ~Environment() {}

        virtual bool safetyOk() = 0;           // may trigger an emergency shutdown
        virtual bool flameDetected() = 0;

        virtual bool boilerEnabled() = 0;
        virtual bool heatingEnabled() = 0;
        virtual bool waterEnabled() = 0;
        virtual bool heatingOn() = 0;          // SystemState::HEATING_ON
        virtual bool waterOn() = 0;            // SystemState::WATER_ON
        virtual bool waterPriority() = 0;      // SystemState::WATER_PRIORITY
        virtual bool heatingRequested() = 0;   // BurnerRequest::HEATING
        virtual bool waterRequested() = 0;     // BurnerRequest::WATER
        virtual bool relaysInWaterMode() = 0;  // mode actually switched on the relays

        virtual bool highPowerAllowed(bool requestedHighPower) = 0;
        virtual bool heatingLikelyWanted() = 0;

        virtual bool canTurnOn() = 0;          // anti-flapping minimum off-time
        virtual bool canTurnOff() = 0;         // anti-flapping minimum on-time
        virtual bool canChangePower(bool toHigh) = 0;

        // Switch the burner relays to the new mode; false on failure.
        virtual bool switchMode(bool toWater) = 0;
    };

    struct Timing {
        uint32_t ignitionMinTimeMs;            // flame is not checked before this
        uint32_t ignitionTimeoutMs;
        uint8_t maxIgnitionRetries;
        uint32_t modeDemandLossGraceMs;
    };

    // State carried between steps.
    struct Memory {
        uint8_t ignitionRetries;
        bool runningModeIsWater;               // mode the burner was started/switched in
        uint32_t noModeDemandSinceMs;          // 0 = mode request present
    };

    struct Context {
        BurnerSMState state;
        uint32_t timeInStateMs;
        uint32_t nowMs;
        bool heatDemand;
        bool requestedHighPower;
    };

    enum class Reason : uint8_t {
        NONE,
        STALE_DEMAND,              // IDLE: demand without an active mode request
        START,
        START_DELAYED,             // minimum off-time
        SAFETY_FAILED,
        MODE_WITHDRAWN,            // PRE_PURGE: mode request gone
        DEMAND_WITHDRAWN,          // PRE_PURGE: heat demand gone
        IGNITION_OK,
        IGNITION_RETRY,
        IGNITION_LOCKOUT,
        SWITCH_SEAMLESS,
        SWITCH_NEEDS_STOP,         // mode changed but seamless switch not possible
        EXPLICIT_DISABLE,
        NO_MODE_DEMAND,
        DEMAND_ENDED,
        FLAME_LOST,
        POWER_UP,
        POWER_UP_DELAYED,
        POWER_DOWN,
        POWER_DOWN_DELAYED,
        SWITCH_WAIT_FOR_HEATING,
        SWITCH_NO_DEMAND,
        SWITCH_RESUME,
        SWITCH_REVERT_WAIT,
        SWITCH_REVERT_STOP,
        SWITCH_FAILED,
        SWITCH_DONE,
        RESTART_FROM_POST_PURGE    // heat demand returned before the post-purge ended
    };

    struct Decision {
        BurnerSMState next;
        Reason reason;
        bool fromWater;            // mode switch / explicit disable: running mode
        bool toWater;              // mode switch: new mode
        bool bothModesOn;          // WATER_ON and HEATING_ON both set
        bool boilerDisabled;
        bool heatingWanted;
        bool newModeHasDemand;
        bool stopDelayed;          // stop wanted but minimum on-time not reached
        uint32_t elapsedMs;        // NO_MODE_DEMAND: time without request
    };

    namespace detail {

        inline Decision make(BurnerSMState next, Reason reason) {
            Decision d = Decision();
            d.next = next;
            d.reason = reason;
            return d;
        }

        inline bool hasActiveModeDemand(Environment& env) {
            return (env.heatingOn() && env.heatingRequested()) ||
                   (env.waterOn() && env.waterRequested());
        }

        inline Decision idle(const Context& c, Environment& env) {
            // heatDemand is latched and survives emergencyStop()/ERROR recovery. Only
            // act on it while a heating/water mode is actually requesting the burner -
            // otherwise a stale demand fires the burner with no mode active (and so no
            // pump), as happened on 2026-09-12/13.
            if (c.heatDemand && !hasActiveModeDemand(env)) {
                return make(BurnerSMState::IDLE, Reason::STALE_DEMAND);
            }
            if (c.heatDemand && env.safetyOk()) {
                return env.canTurnOn() ? make(BurnerSMState::PRE_PURGE, Reason::START)
                                       : make(BurnerSMState::IDLE, Reason::START_DELAYED);
            }
            return make(BurnerSMState::IDLE, Reason::NONE);
        }

        inline Decision prePurge(const Context& c, Environment& env) {
            if (!env.safetyOk()) {
                return make(BurnerSMState::ERROR, Reason::SAFETY_FAILED);
            }
            // Abort the start if the mode/request went away during pre-purge -
            // otherwise onEnterIgnition() activates the burner relays with no mode active.
            if (!hasActiveModeDemand(env)) {
                return make(BurnerSMState::IDLE, Reason::MODE_WITHDRAWN);
            }
            // Same if the heat demand itself was withdrawn (BoilerTempControlTask
            // re-asserting OFF) - otherwise a brief re-arm ignites and then has to run
            // for the anti-flapping minimum on-time.
            if (!c.heatDemand) {
                return make(BurnerSMState::IDLE, Reason::DEMAND_WITHDRAWN);
            }
            // PRE_PURGE -> IGNITION is the StateMachine timeout
            return make(BurnerSMState::PRE_PURGE, Reason::NONE);
        }

        inline Decision ignition(const Context& c, const Timing& t, Environment& env, Memory& m) {
            // Real ignition takes 3-5 s; flame detection (relay proxy) returns immediately
            if (c.timeInStateMs < t.ignitionMinTimeMs) {
                return make(BurnerSMState::IGNITION, Reason::NONE);
            }
            if (env.flameDetected()) {
                m.ignitionRetries = 0;
                return make(env.highPowerAllowed(c.requestedHighPower) ? BurnerSMState::RUNNING_HIGH
                                                                       : BurnerSMState::RUNNING_LOW,
                            Reason::IGNITION_OK);
            }
            if (c.timeInStateMs >= t.ignitionTimeoutMs) {
                m.ignitionRetries++;
                if (m.ignitionRetries >= t.maxIgnitionRetries) {
                    return make(BurnerSMState::LOCKOUT, Reason::IGNITION_LOCKOUT);
                }
                return make(BurnerSMState::PRE_PURGE, Reason::IGNITION_RETRY);
            }
            return make(BurnerSMState::IGNITION, Reason::NONE);
        }

        inline Decision running(const Context& c, const Timing& t, Environment& env, Memory& m) {
            const bool high = (c.state == BurnerSMState::RUNNING_HIGH);
            Decision d = make(c.state, Reason::NONE);

            // 1. Mode switch (water <-> heating). When both WATER_ON and HEATING_ON
            //    are set, WATER_PRIORITY decides.
            const bool waterOn = env.waterOn();
            const bool heatingOn = env.heatingOn();
            const bool waterPriority = env.waterPriority();
            d.bothModesOn = waterOn && heatingOn;
            const bool modeIsWater = waterOn && (!heatingOn || waterPriority);
            if (modeIsWater != m.runningModeIsWater) {
                d.fromWater = m.runningModeIsWater;
                d.toWater = modeIsWater;
                // Seamless only while safe and burning; heat demand is not checked here
                // because the old mode clears it before the new mode sets it - the
                // MODE_SWITCHING step validates the new mode's request.
                const bool seamless = env.safetyOk() && env.flameDetected();
                d.next = seamless ? BurnerSMState::MODE_SWITCHING : BurnerSMState::POST_PURGE;
                d.reason = seamless ? Reason::SWITCH_SEAMLESS : Reason::SWITCH_NEEDS_STOP;
                return d;
            }

            // 2. Explicit disable of the running mode (or of the whole boiler) stops the
            //    burner now instead of waiting out the minimum on-time. The running mode
            //    comes from the relays actually switched.
            const bool relaysWater = env.relaysInWaterMode();
            const bool boilerEnabled = env.boilerEnabled();
            const bool heatingEnabled = env.heatingEnabled();
            const bool waterEnabled = env.waterEnabled();
            if (BurnerTransitionPolicy::stopForExplicitDisable(relaysWater, boilerEnabled,
                                                               heatingEnabled, waterEnabled)) {
                d.next = BurnerSMState::POST_PURGE;
                d.reason = Reason::EXPLICIT_DISABLE;
                d.fromWater = relaysWater;
                d.boilerDisabled = !boilerEnabled;
                return d;
            }

            // 3. Running without any active mode request: stop after a short grace
            //    period. Bypasses anti-flapping (like flame loss) - with no mode active
            //    nothing guarantees circulation.
            if (hasActiveModeDemand(env)) {
                m.noModeDemandSinceMs = 0;
            } else if (m.noModeDemandSinceMs == 0) {
                m.noModeDemandSinceMs = c.nowMs | 1;  // 0 is the "request present" sentinel
            } else if (c.nowMs - m.noModeDemandSinceMs >= t.modeDemandLossGraceMs) {
                d.elapsedMs = c.nowMs - m.noModeDemandSinceMs;
                m.noModeDemandSinceMs = 0;
                d.next = BurnerSMState::POST_PURGE;
                d.reason = Reason::NO_MODE_DEMAND;
                return d;
            }

            // 4. Demand ended or safety check failed: stop once the minimum on-time allows
            if (!c.heatDemand || !env.safetyOk()) {
                if (env.canTurnOff()) {
                    d.next = BurnerSMState::POST_PURGE;
                    d.reason = Reason::DEMAND_ENDED;
                    return d;
                }
                d.stopDelayed = true;
            }

            // 5. Flame loss (intentional or not) bypasses anti-flapping
            if (!env.flameDetected()) {
                d.next = BurnerSMState::POST_PURGE;
                d.reason = Reason::FLAME_LOST;
                return d;
            }

            // 6. Power level follows the PID request; entering high power is blocked
            //    while the boiler is hot (BurnerPowerController).
            if (!high) {
                if (env.highPowerAllowed(c.requestedHighPower)) {
                    if (env.canChangePower(true)) {
                        d.next = BurnerSMState::RUNNING_HIGH;
                        d.reason = Reason::POWER_UP;
                    } else {
                        d.reason = Reason::POWER_UP_DELAYED;
                    }
                }
            } else if (!c.requestedHighPower) {
                if (env.canChangePower(false)) {
                    d.next = BurnerSMState::RUNNING_LOW;
                    d.reason = Reason::POWER_DOWN;
                } else {
                    d.reason = Reason::POWER_DOWN_DELAYED;
                }
            }
            return d;
        }

        inline Decision modeSwitching(const Context& c, Environment& env, Memory& m) {
            if (!env.safetyOk()) {
                return make(BurnerSMState::ERROR, Reason::SAFETY_FAILED);
            }

            // Burner request bits carry the actual demand; the SystemState ON bits may
            // not be set yet during a seamless transition.
            const bool waterRequested = env.waterRequested();
            const bool heatingRequested = env.heatingRequested();
            const bool waterPriority = env.waterPriority();

            Decision d = make(BurnerSMState::MODE_SWITCHING, Reason::NONE);
            d.fromWater = m.runningModeIsWater;
            d.toWater = waterRequested && (!heatingRequested || waterPriority);
            d.newModeHasDemand = d.toWater ? waterRequested : heatingRequested;

            if (!d.newModeHasDemand) {
                // Water -> heating handover: HeatingControlTask (5 s cycle) may not have
                // raised its request yet. Wait only while heating is likely wanted, and
                // never longer than MODE_SWITCH_MAX_WAIT_MS.
                d.heatingWanted = !d.toWater && env.heatingLikelyWanted();
                if (BurnerTransitionPolicy::onNoDemandForNewMode(d.toWater, d.heatingWanted, c.timeInStateMs) ==
                    BurnerTransitionPolicy::ModeSwitchAction::WAIT) {
                    d.reason = Reason::SWITCH_WAIT_FOR_HEATING;
                } else {
                    d.next = BurnerSMState::POST_PURGE;
                    d.reason = Reason::SWITCH_NO_DEMAND;
                }
                return d;
            }

            if (d.toWater == m.runningModeIsWater) {
                // Demand points back to the running mode (race): resume only once its
                // ON bit is set again, otherwise RUNNING <-> MODE_SWITCHING would bounce.
                const bool runningOnBit = m.runningModeIsWater ? env.waterOn() : env.heatingOn();
                switch (BurnerTransitionPolicy::onModeReverted(runningOnBit, c.timeInStateMs)) {
                    case BurnerTransitionPolicy::RevertAction::RESUME_RUNNING:
                        // Safe low power - the PID power request may not be updated yet
                        d.next = BurnerSMState::RUNNING_LOW;
                        d.reason = Reason::SWITCH_RESUME;
                        break;
                    case BurnerTransitionPolicy::RevertAction::WAIT:
                        d.reason = Reason::SWITCH_REVERT_WAIT;
                        break;
                    case BurnerTransitionPolicy::RevertAction::STOP:
                    default:
                        d.next = BurnerSMState::POST_PURGE;
                        d.reason = Reason::SWITCH_REVERT_STOP;
                        break;
                }
                return d;
            }

            if (!env.switchMode(d.toWater)) {
                d.next = BurnerSMState::POST_PURGE;
                d.reason = Reason::SWITCH_FAILED;
                return d;
            }
            m.runningModeIsWater = d.toWater;
            d.next = env.highPowerAllowed(c.requestedHighPower) ? BurnerSMState::RUNNING_HIGH
                                                                : BurnerSMState::RUNNING_LOW;
            d.reason = Reason::SWITCH_DONE;
            return d;
        }

        inline Decision postPurge(const Context& c, Environment& env) {
            // Post-purge only keeps the burner off; the pumps carry the heat away
            // independently. If heat demand returns meanwhile, restart under the same
            // conditions as from IDLE instead of holding off for the rest of the
            // post-purge; the anti-flapping minimum off-time still applies.
            if (!c.heatDemand || !hasActiveModeDemand(env)) {
                return make(BurnerSMState::POST_PURGE, Reason::NONE);
            }
            // Never restart a mode that was just disabled: its request can still be
            // set for one control cycle after the explicit-disable stop
            const bool waterOn = env.waterOn();
            const bool toWater = waterOn && (!env.heatingOn() || env.waterPriority());
            if (BurnerTransitionPolicy::stopForExplicitDisable(toWater, env.boilerEnabled(),
                                                               env.heatingEnabled(), env.waterEnabled())) {
                return make(BurnerSMState::POST_PURGE, Reason::NONE);
            }
            if (!env.safetyOk() || !env.canTurnOn()) {
                return make(BurnerSMState::POST_PURGE, Reason::NONE);
            }
            return make(BurnerSMState::PRE_PURGE, Reason::RESTART_FROM_POST_PURGE);
        }

    } // namespace detail

    /**
     * @brief Decide the next state for one state machine tick.
     *
     * States not handled here (LOCKOUT, ERROR) return themselves. POST_PURGE only
     * decides the early restart; its completion is checked by the caller.
     */
    inline Decision step(const Context& c, const Timing& t, Environment& env, Memory& m) {
        switch (c.state) {
            case BurnerSMState::IDLE:
                return detail::idle(c, env);
            case BurnerSMState::PRE_PURGE:
                return detail::prePurge(c, env);
            case BurnerSMState::IGNITION:
                return detail::ignition(c, t, env, m);
            case BurnerSMState::RUNNING_LOW:
            case BurnerSMState::RUNNING_HIGH:
                return detail::running(c, t, env, m);
            case BurnerSMState::MODE_SWITCHING:
                return detail::modeSwitching(c, env, m);
            case BurnerSMState::POST_PURGE:
                return detail::postPurge(c, env);
            default:
                return detail::make(c.state, Reason::NONE);
        }
    }

} // namespace BurnerTransitions

#endif // BURNER_TRANSITIONS_H
