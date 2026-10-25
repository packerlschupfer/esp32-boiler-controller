// include/modules/control/BusReleasePolicy.h
#ifndef BUS_RELEASE_POLICY_H
#define BUS_RELEASE_POLICY_H

#include <cstdint>

#include "config/RelayIndices.h"  // single source of truth for the relay bit positions

/**
 * @brief Rules for deliberately handing the RS485 bus to another master.
 *
 * WHY THIS EXISTS (2026-10-25)
 *
 * The room sensor runs open firmware whose maintenance tools - identity, the
 * parallel-resistor calibration, A/B firmware updates over Modbus - all require being the
 * SOLE master on the bus. This controller polls that bus continuously, so those tools cannot
 * run while it is alive. Powering the ESP32 down works and is strictly safer (BURNER_ENABLE
 * is normally open, so an unpowered controller cannot fire the boiler at all), but it needs
 * physical access and loses the window's observability.
 *
 * WHAT MAKES THIS SAFE, AND WHAT DOES NOT
 *
 * The relays are on the SAME bus as the sensors - RYN4 writes are scheduled by
 * ModbusCoordinator alongside the sensor reads. So the obvious design, "stop polling and let
 * TemperatureSensorFallback stop the burner", is wrong: once the bus is released, nothing can
 * actuate anything, and the safety layer that would stop the burner has no way to reach it.
 *
 * Hence the shape here:
 *
 *  1. ENTRY REFUSES rather than orchestrates. The burner must already be off and no heat
 *     demand present. There is no new shutdown path to get that wrong - turn the boiler off
 *     first, then release.
 *  2. The burner cannot be ARMED while released (BurnerStateMachine::setHeatDemand consults
 *     this), so the state machine cannot walk into an ignition sequence whose relay writes
 *     would land the moment the bus comes back.
 *  3. The firing bits are forced out of the DESIRED mask before bus access resumes, so the
 *     first resumed write cannot energise the burner on a decision made from stale readings.
 *  4. The window EXPIRES ON-DEVICE. This is the part that is not optional: with the bus
 *     released there is no path for a remote "acquire" to be guaranteed, so a dropped network
 *     or a wedged broker must not be able to leave the controller out of service. The
 *     deadline is enforced by the same timer that would otherwise grant bus access.
 *
 * Note that a reboot mid-window is a SAFE outcome by construction: nothing here is persisted,
 * so the controller comes back fully operational with a busy bus. The other master's tool
 * fails visibly, which is the right way round.
 *
 * Sensors go stale during the window and the fallback escalates as it would for a bus fault -
 * that path is unchanged and already exercised.
 *
 * THE RELAY BOARD MAKES THIS FAIL-SAFE IN HARDWARE, which is the strongest property here and
 * was not designed for this feature at all. Every relay switched ON is held by an RYN4 DELAY
 * watchdog of SystemConstants::Relay::DELAY_WATCHDOG_SECONDS (10 s), renewed on each RYN4_SET
 * tick. Withhold the ticks and the board de-energises EVERYTHING by itself within 10 seconds -
 * burner, pumps and all - with no cooperation from this firmware. So a released bus converges
 * on the same physical state as pulling the controller's power, which is the comparison this
 * feature has to beat. It also means the window ends from an all-off hardware state, so there
 * is nothing stale left energised when polling resumes.
 *
 * That is also why entry refuses while the burner is firing: releasing mid-burn would not stop
 * it gracefully, it would drop the flame and the pumps together ten seconds later, with no
 * post-purge and no pump overrun.
 */
namespace BusReleasePolicy {

    /// Default and bounds for the window. The cap is deliberately short: this exists for a
    /// tool run measured in seconds, not for leaving the controller parked.
    constexpr uint32_t DEFAULT_SECONDS = 600;  // 10 minutes
    constexpr uint32_t MIN_SECONDS     = 30;
    constexpr uint32_t MAX_SECONDS     = 900;  // 15 minutes, hard cap

    /// Relay bits meaning "the burner is firing", in RelayState/RelayIndex order: the burner
    /// enable (heating mode) or the water mode relay. This is the same condition as the
    /// published `r & 17` on boiler/status/sensors, which uses a different bit order - taking
    /// these from RelayIndex keeps the two from drifting apart.
    constexpr uint8_t FIRING_MASK =
        static_cast<uint8_t>((1u << RelayIndex::BURNER_ENABLE) | (1u << RelayIndex::WATER_MODE));

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
        NONE,            // may release
        BURNER_ACTIVE,   // a firing relay is closed
        DEMAND_PRESENT   // heat demand is armed, so the burner is about to fire
    };

    /**
     * @brief May the bus be released right now?
     *
     * @param relaysActual the HARDWARE-VERIFIED relay mask (RelayState::actual), not the
     *        desired one: the question is what the boiler is doing, not what we asked for.
     * @param heatDemand BurnerStateMachine's armed demand. Checked as well as the relays
     *        because demand precedes the relays by a pre-purge, so firing bits can be clear
     *        while an ignition is seconds away.
     */
    inline Refusal mayRelease(uint8_t relaysActual, bool heatDemand) {
        if ((relaysActual & FIRING_MASK) != 0) {
            return Refusal::BURNER_ACTIVE;
        }
        if (heatDemand) {
            return Refusal::DEMAND_PRESENT;
        }
        return Refusal::NONE;
    }

    inline const char* toString(Refusal r) {
        switch (r) {
            case Refusal::NONE:           return "ok";
            case Refusal::BURNER_ACTIVE:  return "burner_active";
            case Refusal::DEMAND_PRESENT: return "demand_present";
        }
        return "unknown";
    }

    /**
     * @brief Has the window ended?
     *
     * The subtraction is done in uint32_t and the result read as signed, which stays correct
     * across the 49.7-day millis() rollover where `now >= deadline` would not. A rollover
     * mid-window would otherwise either end the window instantly or extend it by 49 days -
     * and the long way round is the one that matters here.
     */
    inline bool expired(uint32_t nowMs, uint32_t deadlineMs) {
        return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
    }

    /// Rounded UP, so a window with 200 ms left reports 1 rather than 0: reporting 0 while
    /// still released would contradict the released flag published beside it.
    inline uint32_t secondsLeft(uint32_t nowMs, uint32_t deadlineMs) {
        if (expired(nowMs, deadlineMs)) {
            return 0;
        }
        return (deadlineMs - nowMs + 999u) / 1000u;
    }

    /// The desired-relay mask to impose before bus access resumes: everything as it was,
    /// minus the firing bits. The hardware is already all-off by then (the DELAY watchdog),
    /// so this is purely about what the FIRST resumed write would push out - it must not
    /// re-energise the burner on a decision taken from readings that went stale mid-window.
    /// Pump bits are left for the control logic to re-decide on fresh readings.
    inline uint8_t safeDesiredMask(uint8_t desired) {
        return static_cast<uint8_t>(desired & ~FIRING_MASK);
    }

} // namespace BusReleasePolicy

#endif // BUS_RELEASE_POLICY_H
