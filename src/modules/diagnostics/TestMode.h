// src/modules/diagnostics/TestMode.h
#ifndef TEST_MODE_H
#define TEST_MODE_H

#include <cstdint>

#include "modules/diagnostics/TestModePolicy.h"

/**
 * @brief Runtime state for a bounded operator window of direct relay control.
 *
 * See TestModePolicy.h for what this is for, what it deliberately does NOT check, and how the
 * three automatic writers stand down. This header is only the state and the lifecycle.
 *
 * SHAPED AFTER ModbusBusRelease AND SyslogWindow on purpose - same mutator/reader split, same
 * extend-never-shorten rule, same consumeStateChange() for the publisher, same rollover-safe
 * deadline. That is now three bounded operator windows in this firmware; an operator who knows
 * `bus_release` should be able to guess how `test_mode` behaves, and be right.
 *
 * isActive() IS A PURE ATOMIC READ, called from the hottest paths there are - the relay write
 * choke point, the pump reconcile loop, and setHeatDemand() under demandMutex. It must never
 * mutate anything. expireIfDue() is the ONLY function that ends a window on its own, and it is
 * called from ModbusCoordinator::processTick() (every 500 ms, the thing that schedules relay
 * writes) with the MQTT publisher as a backstop. An earlier draft of ModbusBusRelease expired
 * inside its isReleased(); that put a relay-mask mutation inside the demand-write path, and the
 * same mistake is available here.
 */
namespace TestMode {

    /**
     * @brief Open (or extend) a window of direct relay control for `seconds`.
     * @return Refusal::NONE on success; otherwise nothing changed.
     *
     * Snapshots the desired relay mask so leave() can put back what it found - see
     * TestModePolicy::exitMask(). A call while a window is already open EXTENDS it and never
     * shortens it, and does NOT re-snapshot: the baseline stays the state the boiler was in
     * before the operator started, not the state they have since created.
     */
    TestModePolicy::Refusal enter(uint32_t seconds);

    /// End the window now and restore the relays. Safe to call when not active.
    void leave();

    /// Pure read. True while the operator holds the relays.
    bool isActive();

    /// End the window if its deadline has passed. Call from the relay scheduler.
    void expireIfDue();

    /// Seconds remaining, 0 when inactive.
    uint32_t secondsLeft();

    /**
     * @brief Drive one relay by its RelayIndex (0-7). NO INTERLOCKS - see TestModePolicy.h.
     * @return false if no window is open, the index is out of range, or the relay layer
     *         rejected the write.
     *
     * Routed through RelayControlTask::setRelayStateManual(), which bypasses the duplicate
     * skip, the toggle rate limiter and pump motor protection. Bypassing those is the point:
     * a diagnostic tool whose commands are SILENTLY DROPPED teaches the operator that the
     * hardware is broken when it is not. The rate limiter dropping an unchanged command is
     * exactly how the 2026-09-14 emergency stop happened, and a 15 s pump-protection refusal
     * looks identical to a dead pump from the outside.
     */
    bool setRelay(uint8_t relayIndex, bool on);

    /**
     * @brief End the window immediately because an OTA is starting.
     *
     * Called from OTATask::onOTAStart, beside the MQTT teardown and SyslogWindow::closeForOTA()
     * that are already there. Not about DRAM, unlike the syslog window: an OTA ends in a
     * REBOOT, and nothing here is persisted, so a window left open would be silently gone
     * afterwards with manually-set relays surviving only until the DELAY watchdog dropped them
     * ten seconds later. Ending it properly restores the entry mask first.
     */
    void closeForOTA();

    /// How the window ended, for the status topic.
    enum class Ended : uint8_t { NOT_YET, BY_COMMAND, BY_TIMEOUT, BY_OTA };

    /// True once per transition, so a periodic publisher reports changes without republishing
    /// the same state forever. Clears the flag.
    bool consumeStateChange();

    /// Why the last window ended.
    Ended lastEnd();

    /// Label for lastEnd(), for the status payload. Stable and greppable.
    const char* endedToString(Ended e);

}  // namespace TestMode

#endif  // TEST_MODE_H
