// include/modules/diagnostics/TestModePolicy.h
#ifndef TEST_MODE_POLICY_H
#define TEST_MODE_POLICY_H

#include <cstdint>
#include <cstring>

#include "config/RelayIndices.h"
#include "modules/control/BurnerSMState.h"

/**
 * @brief May an operator take direct control of the relays right now, and for how long?
 *
 * Header-only and natively testable, like BusReleasePolicy and SyslogWindowPolicy, and
 * deliberately the same shape: clamp, named refusal enum, one pure decision function,
 * on-device expiry. Three bounded operator windows in this firmware should not be three
 * different idioms.
 *
 * WHAT THIS IS FOR
 *
 * Driving individual relays by hand on a live controller - to listen to a pump, confirm a
 * valve actually diverts, check wiring after work, or chase a fault that only appears under
 * load. There is no other way to do any of that today: the add-on panel's "Manual Relay
 * Control" card published boiler/cmd/relay/<name> to a topic nothing has ever subscribed to,
 * and the only real override that ever existed (relays/<1-8>/control, live for five commits
 * on 2025-06-13) was commented out six months before that card was written.
 *
 * NO INTERLOCKS ON THE COMMANDS. OWNER'S DECISION, 2026-11-12.
 *
 * A relay command inside an open window goes to the hardware. It is not checked against
 * overheat, sensor validity, emergency state or pump protection. The 2025 override behaved the
 * same way. Stated plainly because it is the whole point of the feature and because a future
 * reader will otherwise assume the interlocks are there:
 *
 *   CLOSING BURNER_ENABLE WITH NO PUMP RUNNING FIRES THE BOILER INTO STATIC WATER.
 *   Nothing in this firmware will stop that. The Vaillant's own overheat cutout is the only
 *   remaining protection, and it is the boiler's last line, not ours.
 *
 * WHAT STILL PROTECTS YOU, none of it from this firmware's control logic:
 *   - The RYN4 DELAY watchdog de-energises any closed relay within 10 s unless it is renewed,
 *     so a manual relay cannot outlive the controller that set it. A free dead-man's switch.
 *   - The window self-expires on-device; a dropped network cannot leave it open.
 *   - The boiler's own thermostat, overheat cutout and flame supervision are untouched. This
 *     bypasses OUR interlocks, not the boiler's.
 *
 * ENTRY REFUSALS ARE NOT INTERLOCKS, and they stay. They do not restrict what an operator may
 * do inside the window - they stop the window OPENING on top of a burn that is already
 * running or mid-sequence, which would hand over control in the middle of an ignition. That is
 * about not breaking a running system, not about second-guessing the operator.
 *
 * HOW THE AUTOMATIC PATH STANDS DOWN, and why it is three changes rather than one
 *
 * A manual relay command that is not defended gets REVERTED, which would make the whole
 * feature useless in a way that looks like broken hardware. Three separate writers had to be
 * made to yield, and they are different in kind:
 *
 *  1. DEMAND CANNOT BE ARMED (BurnerStateMachine::setHeatDemand). Same gate `bus_release`
 *     already uses, one more `&&`. Without it the state machine walks into PRE_PURGE on its
 *     own while the operator is holding the relays.
 *  2. PumpControlModule SKIPS ITS WHOLE RECONCILE BLOCK. This is the one that matters and
 *     the one the design note predicted would be easiest to get wrong. That module is not
 *     edge-driven: RelayCommandPolicy::pumpRequestResendDue() RE-SENDS its request whenever
 *     the relay bit disagrees with its own state, after PUMP_REQUEST_RESEND_MS (2000 ms). So
 *     a manual pump command - in EITHER direction - is undone about two seconds later, and no
 *     amount of blocking at the relay layer fixes it, because the module would just keep
 *     asking. It has to stand down at the source.
 *  3. THE RELAY LAYER REFUSES NON-EMERGENCY *ON* COMMANDS (RelayControlTask::setRelayState).
 *     Belt and braces for any writer not enumerated above, present or future.
 *
 * Two carve-outs in (3), both deliberate:
 *
 *   - ONLY "ON" IS BLOCKED. Every OFF goes through. CentralizedFailsafe stops the burner with
 *     ORDINARY setRelayState() calls (burner enable, power boost, water mode), not the
 *     emergency setter, so a blanket block would disable the overheat shutdown - and the one
 *     thing this must not do is bypass the SAFETY system while bypassing the CONTROL system.
 *     An OFF cannot create a hazard, so letting all of them through costs nothing.
 *   - ALARM IS EXEMPT. BurnerStateMachine raises it on lockout entry. Swallowing that would
 *     silence a real alarm for the length of the window, which is a poor trade for tidiness.
 *
 * SAFETY STILL ACTS, THEN. CentralizedFailsafe can shut the burner down under the operator,
 * and will. That is not an interlock on the command - nothing checks what the operator asks
 * for - it is an independent actor that was always there, and leaving it armed is the whole
 * reason "no interlocks" is an acceptable answer to decision 1.
 */
namespace TestModePolicy {

    /// Default and bounds. Shorter than the syslog window because this one energises
    /// hardware with no software protection behind it: the operator is expected to be
    /// standing at the boiler, and a forgotten window is a worse thing to leave open.
    constexpr uint32_t DEFAULT_SECONDS = 300;   // 5 minutes
    constexpr uint32_t MIN_SECONDS     = 30;
    constexpr uint32_t MAX_SECONDS     = 900;   // 15 minutes, hard cap

    /// 0 means "unspecified", which takes the default rather than the minimum.
    inline uint32_t clampSeconds(uint32_t requested) {
        if (requested == 0) {
            return DEFAULT_SECONDS;
        }
        if (requested < MIN_SECONDS) {
            return MIN_SECONDS;
        }
        if (requested > MAX_SECONDS) {
            return MAX_SECONDS;
        }
        return requested;
    }

    enum class Refusal : uint8_t {
        NONE,                // may enter
        BURNER_ACTIVE,       // a firing relay is closed
        BURNER_SEQUENCING,   // mid purge / ignition / mode switch
        OTA_ACTIVE,          // an update is running
        BUS_RELEASED         // the RS485 bus belongs to another master, so relays are unreachable
    };

    /// Relay bits meaning "the burner is firing", taken from RelayIndex so this and
    /// BusReleasePolicy::FIRING_MASK cannot drift apart.
    constexpr uint8_t FIRING_MASK =
        static_cast<uint8_t>((1u << RelayIndex::BURNER_ENABLE) | (1u << RelayIndex::WATER_MODE));

    /**
     * @brief Relay bits the PUMP MODULE owns, which this window must not restore on exit.
     *
     * Same two bits as BusReleasePolicy::PUMP_MASK and for a related reason, but used the
     * other way round: there they are a REFUSAL condition, here they are an EXEMPTION. On
     * exit PumpControlModule reconciles these within PUMP_REQUEST_RESEND_MS whatever we
     * leave behind, so writing them would only produce a flap - and if the operator's last
     * manual state happened to match what the module wants, not even that.
     */
    constexpr uint8_t PUMP_MASK =
        static_cast<uint8_t>((1u << RelayIndex::HEATING_PUMP) | (1u << RelayIndex::WATER_PUMP));

    /**
     * @brief The desired-relay mask to impose when the window ends.
     *
     * LEAVE THE RELAYS AS YOU FOUND THEM. Not "all off", which was the first draft: the valve
     * and the alarm have NO automatic writer that would put them back. Nothing in this
     * firmware ever commands VALVE (SystemEventsGenerated declares VALVE_OPEN/CLOSE and no
     * handler exists), and BurnerStateMachine writes ALARM only on lockout ENTRY and EXIT,
     * which are edges. So zeroing them would silently hold a diverter closed or a lockout
     * alarm silent until the next edge - a test mode leaving the boiler in a different state
     * than it found it, which is the one thing it must not do.
     *
     * Three rules, in order of precedence:
     *   - the firing bits are forced OFF, whatever they were at entry. mayEnter() checks
     *     `actual`, and `desired` can legitimately lead it, so an entry snapshot is not
     *     guaranteed to have them clear.
     *   - the pump bits are taken LIVE, not from the snapshot - see PUMP_MASK.
     *   - everything else is restored from the entry snapshot.
     */
    inline uint8_t exitMask(uint8_t entryDesired, uint8_t liveDesired) {
        const uint8_t restored =
            static_cast<uint8_t>(entryDesired & ~FIRING_MASK & ~PUMP_MASK);
        return static_cast<uint8_t>(restored | (liveDesired & PUMP_MASK));
    }

    /**
     * @brief Should the relay layer refuse this command because test mode holds the relays?
     *
     * The rule behind carve-out (3) above, pulled out so it is testable natively and stated
     * once. `isEmergency` commands and every OFF pass; ALARM passes in both directions.
     */
    inline bool standDownBlocks(uint8_t relayIndex0, bool turningOn, bool isEmergency) {
        if (isEmergency || !turningOn) {
            return false;
        }
        if (relayIndex0 == RelayIndex::ALARM) {
            return false;
        }
        return true;
    }

    /**
     * @brief Is the burner state machine mid-sequence?
     *
     * Same split as BusReleasePolicy::isSequencing(): IDLE, LOCKOUT and ERROR are quiescent
     * and do not block; everything else has relay writes due on the next tick.
     */
    inline bool isSequencing(BurnerSMState state) {
        switch (state) {
            case BurnerSMState::IDLE:
            case BurnerSMState::LOCKOUT:
            case BurnerSMState::ERROR:
                return false;
            case BurnerSMState::PRE_PURGE:
            case BurnerSMState::IGNITION:
            case BurnerSMState::RUNNING_LOW:
            case BurnerSMState::RUNNING_HIGH:
            case BurnerSMState::MODE_SWITCHING:
            case BurnerSMState::POST_PURGE:
                return true;
        }
        return true;  // an unknown state is not a safe one
    }

    /**
     * @brief May a window be opened right now?
     *
     * @param relaysActual the HARDWARE-VERIFIED relay mask, not the desired one - the question
     *        is what the boiler is doing, not what was asked for.
     * @param burnerState  the state machine's current state.
     * @param otaActive    an OTA is in progress.
     * @param busReleased  the RS485 bus has been handed to another master.
     *
     * Deliberately NOT refused: a latched emergency stop, a failed sensor, a DEGRADED failsafe.
     * Those are exactly the conditions an operator opens this mode to investigate, and
     * refusing then would make the tool useless in the case it exists for.
     */
    inline Refusal mayEnter(uint8_t relaysActual, BurnerSMState burnerState,
                            bool otaActive, bool busReleased) {
        if (otaActive) {
            return Refusal::OTA_ACTIVE;
        }
        if (busReleased) {
            return Refusal::BUS_RELEASED;
        }
        if ((relaysActual & FIRING_MASK) != 0) {
            return Refusal::BURNER_ACTIVE;
        }
        if (isSequencing(burnerState)) {
            return Refusal::BURNER_SEQUENCING;
        }
        return Refusal::NONE;
    }

    /// Short stable labels, published on the status topic. Do not reword casually.
    inline const char* toString(Refusal r) {
        switch (r) {
            case Refusal::NONE:              return "ok";
            case Refusal::BURNER_ACTIVE:     return "burner_active";
            case Refusal::BURNER_SEQUENCING: return "burner_sequencing";
            case Refusal::OTA_ACTIVE:        return "ota_active";
            case Refusal::BUS_RELEASED:      return "bus_released";
        }
        return "unknown";
    }

    /**
     * @brief Map a relay NAME to its RelayIndex.
     * @return the index, or 0xFF if the name is not recognised.
     *
     * NAMES, NOT NUMBERS, and that is deliberate. The 2025 override addressed relays by
     * PHYSICAL NUMBER (relays/3/control), so a hardware re-map would silently redirect a
     * command to different hardware while every topic kept working. These resolve through
     * RelayIndex, which is the single source of truth for the assignment.
     *
     * `power_boost` is the canonical name. `half_power` is accepted because the add-on panel
     * used it for a year - but it was always a MISNOMER: the bit is set from relayPowerBoost,
     * so ON means FULL power, not half. Accepting the old name keeps an existing panel
     * working; it is not an endorsement of it.
     */
    inline uint8_t relayFromName(const char* name) {
        if (name == nullptr) return 0xFF;
        struct Entry { const char* name; uint8_t idx; };
        static const Entry table[] = {
            { "burner",       RelayIndex::BURNER_ENABLE },
            { "burner_enable",RelayIndex::BURNER_ENABLE },
            { "power_boost",  RelayIndex::POWER_BOOST   },
            { "half_power",   RelayIndex::POWER_BOOST   },  // legacy panel name, a misnomer
            { "water_mode",   RelayIndex::WATER_MODE    },
            { "valve",        RelayIndex::VALVE         },
            { "heating_pump", RelayIndex::HEATING_PUMP  },
            { "water_pump",   RelayIndex::WATER_PUMP    },
            { "spare",        RelayIndex::SPARE_7       },
            { "alarm",        RelayIndex::ALARM         },
        };
        for (const auto& e : table) {
            if (std::strcmp(e.name, name) == 0) return e.idx;
        }
        return 0xFF;
    }

    /// Is this a relay index we will accept a command for? All eight, per the owner's
    /// "one tier" decision - no safe/hazardous split.
    inline bool relayAllowed(uint8_t idx) {
        return idx < RelayIndex::MAX_RELAYS;
    }

    /// Rollover-safe, same idiom as BusReleasePolicy and SyslogWindowPolicy. Do NOT
    /// "simplify" to `now >= deadline`: millis() wraps every 49.7 days and this controller
    /// has run longer than that.
    inline bool expired(uint32_t nowMs, uint32_t deadlineMs) {
        return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
    }

    /// Is `candidate` later than `reference`? Used so a re-issue EXTENDS and never SHORTENS.
    inline bool isLater(uint32_t candidate, uint32_t reference) {
        return static_cast<int32_t>(candidate - reference) > 0;
    }

    /// Rounded UP, so a window with 200 ms left reports 1 rather than 0.
    inline uint32_t secondsLeft(uint32_t nowMs, uint32_t deadlineMs) {
        if (expired(nowMs, deadlineMs)) {
            return 0;
        }
        return (deadlineMs - nowMs + 999u) / 1000u;
    }

}  // namespace TestModePolicy

#endif  // TEST_MODE_POLICY_H
