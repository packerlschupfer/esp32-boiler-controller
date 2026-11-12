// src/modules/diagnostics/TestMode.cpp
#include "modules/diagnostics/TestMode.h"

#include <Arduino.h>  // millis()
#include <atomic>

#include "LoggingMacros.h"
#include "config/RelayIndices.h"
#include "core/ModbusBusRelease.h"
#include "modules/control/BurnerStateMachine.h"
#include "modules/tasks/OTATask.h"
#include "modules/tasks/RelayControlTask.h"
#include "shared/RelayState.h"

static const char* TAG = "TestMode";

namespace {

std::atomic<bool> s_active{false};
std::atomic<uint32_t> s_deadlineMs{0};
std::atomic<bool> s_changed{false};
std::atomic<uint8_t> s_lastEnd{static_cast<uint8_t>(TestMode::Ended::NOT_YET)};

/**
 * The desired-relay mask as it was when the window opened, so leave() can put back what it
 * found. See TestModePolicy::exitMask() for which bits are restored and which are not.
 *
 * Taken once per window, NOT per enter() call: an extension must not re-baseline onto the
 * relays the operator has since been setting, or the restore would hand back their last
 * experiment as if it were the boiler's normal state.
 */
std::atomic<uint8_t> s_entryDesired{0};

/**
 * @brief End the window: restore the relays FIRST, then drop the flag.
 *
 * ORDER MATTERS, and the reasoning is lifted straight from ModbusBusRelease::endRelease()
 * because the hazard is the same one: the moment the flag clears, the automatic path is free
 * to act. Restoring first means it re-decides against a mask that is already safe.
 *
 * The difference from the bus release is that here the mask is WRITTEN rather than merely
 * masked down, so a `store` is correct - the operator may have set bits that nothing else
 * would ever clear (the valve has no automatic writer at all).
 *
 * pendingWrite is set so the next RYN4_SET tick pushes the restored mask out rather than
 * waiting for some control path to touch a relay. Mirrors RelayState::setRelay().
 */
void endWindow(TestMode::Ended how) {
    const uint8_t live = g_relayState.desired.load(std::memory_order_acquire);
    const uint8_t restore =
        TestModePolicy::exitMask(s_entryDesired.load(std::memory_order_acquire), live);
    if (restore != live) {
        g_relayState.desired.store(restore, std::memory_order_release);
        g_relayState.pendingWrite.store(true, std::memory_order_release);
    }
    s_lastEnd.store(static_cast<uint8_t>(how), std::memory_order_release);
    s_active.store(false, std::memory_order_release);
    s_changed.store(true, std::memory_order_release);
}

}  // namespace

namespace TestMode {

TestModePolicy::Refusal enter(uint32_t seconds) {
    if (s_active.load(std::memory_order_acquire)) {
        // Already open: extend, never shorten - the same bug ModbusBusRelease::release() had
        // and the same fix. Keep the later deadline, and do NOT re-snapshot the entry mask.
        const uint32_t requested = TestModePolicy::clampSeconds(seconds);
        const uint32_t current = s_deadlineMs.load(std::memory_order_acquire);
        const uint32_t proposed = millis() + requested * 1000u;
        const bool longer = TestModePolicy::isLater(proposed, current);
        if (longer) {
            s_deadlineMs.store(proposed, std::memory_order_release);
            s_changed.store(true, std::memory_order_release);
        }
        LOG_WARN(TAG, "Test mode already active - window %s (%lu s left)",
                 longer ? "extended" : "kept, request was shorter",
                 static_cast<unsigned long>(TestModePolicy::secondsLeft(
                     millis(), s_deadlineMs.load(std::memory_order_acquire))));
        return TestModePolicy::Refusal::NONE;
    }

    // CLAIM THE WINDOW FIRST, then validate, and roll back if refused.
    //
    // Exactly the race ModbusBusRelease::release() had to close, for exactly the same reason:
    // setHeatDemand() refuses arming by reading isActive() under demandMutex, so an arm
    // landing between the state read below and the claim would be neither seen by the check
    // nor refused by the gate. enter() runs on MQTTTask at priority 2; both callers of
    // setHeatDemand(true) are priority 4, so they preempt it at any point in that span or run
    // concurrently on the other core. Claiming first inverts the race into the safe direction.
    //
    // The DEADLINE is written before the flag so a concurrent expireIfDue() cannot see the
    // previous (long past) deadline and end a window that has not started.
    const uint32_t window = TestModePolicy::clampSeconds(seconds);
    s_deadlineMs.store(millis() + window * 1000u, std::memory_order_release);
    s_lastEnd.store(static_cast<uint8_t>(Ended::NOT_YET), std::memory_order_release);
    s_active.store(true, std::memory_order_release);

    // The hardware-verified mask, not the desired one: the question is what the boiler is
    // doing, not what was asked for.
    const uint8_t actual = g_relayState.actual.load(std::memory_order_acquire);
    const BurnerSMState burnerState = BurnerStateMachine::getCurrentState();
    const bool otaActive = OTATask::isOtaInProgress();
    const bool busReleased = ModbusBusRelease::isReleased();

    const TestModePolicy::Refusal refusal =
        TestModePolicy::mayEnter(actual, burnerState, otaActive, busReleased);
    if (refusal != TestModePolicy::Refusal::NONE) {
        s_active.store(false, std::memory_order_release);
        LOG_WARN(TAG, "Test mode refused (%s): relays=0x%02X sm=%d ota=%d bus=%d",
                 TestModePolicy::toString(refusal), actual, static_cast<int>(burnerState),
                 otaActive ? 1 : 0, busReleased ? 1 : 0);
        return refusal;
    }

    // Snapshot AFTER the refusal checks, so a refused attempt cannot overwrite the baseline of
    // nothing, and after the claim, so the automatic path is already standing down and the
    // mask cannot move under us.
    s_entryDesired.store(g_relayState.desired.load(std::memory_order_acquire),
                         std::memory_order_release);

    s_changed.store(true, std::memory_order_release);
    LOG_WARN(TAG, "TEST MODE ACTIVE for %lu s - relays under MANUAL control, NO interlocks, "
                  "burner cannot be armed automatically (entry mask 0x%02X)",
             static_cast<unsigned long>(window),
             s_entryDesired.load(std::memory_order_acquire));
    return TestModePolicy::Refusal::NONE;
}

void leave() {
    if (!s_active.load(std::memory_order_acquire)) {
        return;
    }
    endWindow(Ended::BY_COMMAND);
    LOG_WARN(TAG, "Test mode ended by command - automatic control resumes");
}

bool isActive() {
    return s_active.load(std::memory_order_acquire);
}

void expireIfDue() {
    if (!s_active.load(std::memory_order_acquire)) {
        return;
    }
    if (!TestModePolicy::expired(millis(), s_deadlineMs.load(std::memory_order_acquire))) {
        return;
    }
    endWindow(Ended::BY_TIMEOUT);
    // No logging here: this runs from ModbusCoordinator's 500 ms tick, and one LOG_* costs
    // ~1312 B of stack on this project. The publisher reports the transition instead - the
    // same arrangement as ModbusBusRelease::expireIfDue() and SyslogWindow::expireIfDue().
}

uint32_t secondsLeft() {
    if (!s_active.load(std::memory_order_acquire)) {
        return 0;
    }
    return TestModePolicy::secondsLeft(millis(),
                                       s_deadlineMs.load(std::memory_order_acquire));
}

bool setRelay(uint8_t relayIndex, bool on) {
    if (!s_active.load(std::memory_order_acquire)) {
        LOG_WARN(TAG, "Relay command ignored - test mode is not active");
        return false;
    }
    if (!TestModePolicy::relayAllowed(relayIndex)) {
        LOG_WARN(TAG, "Relay command ignored - index %u out of range", relayIndex);
        return false;
    }
    // WARN, not INFO: a manual relay write on a live boiler is worth finding in the log
    // afterwards, and the operator will want to correlate it with what the boiler did.
    LOG_WARN(TAG, "MANUAL relay %u (phys %u) -> %s", relayIndex,
             RelayIndex::toPhysical(relayIndex), on ? "ON" : "OFF");
    return RelayControlTask::setRelayStateManual(RelayIndex::toPhysical(relayIndex), on);
}

void closeForOTA() {
    if (!s_active.load(std::memory_order_acquire)) {
        return;
    }
    endWindow(Ended::BY_OTA);
    LOG_WARN(TAG, "Test mode ended - OTA starting, relays restored before the reboot");
}

bool consumeStateChange() {
    return s_changed.exchange(false, std::memory_order_acq_rel);
}

Ended lastEnd() {
    return static_cast<Ended>(s_lastEnd.load(std::memory_order_acquire));
}

const char* endedToString(Ended e) {
    switch (e) {
        case Ended::NOT_YET:    return "none";
        case Ended::BY_COMMAND: return "command";
        case Ended::BY_TIMEOUT: return "timeout";
        case Ended::BY_OTA:     return "ota";
    }
    return "unknown";
}

}  // namespace TestMode
