// src/core/ModbusBusRelease.cpp
#include "core/ModbusBusRelease.h"

#include <Arduino.h>  // millis()
#include <atomic>

#include "LoggingMacros.h"
#include "core/SystemResourceProvider.h"
#include "events/SystemEventsGenerated.h"
#include "modules/control/BurnerStateMachine.h"
#include "shared/RelayState.h"
#include "shared/Temperature.h"

static const char* TAG = "BusRelease";

namespace {
    std::atomic<bool> s_released{false};
    std::atomic<uint32_t> s_deadlineMs{0};
    std::atomic<bool> s_changed{false};
    std::atomic<uint8_t> s_lastEnd{static_cast<uint8_t>(ModbusBusRelease::Ended::NOT_YET)};

    /**
     * @brief Impose the safe desired-relay mask, then drop the release flag.
     *
     * ORDER MATTERS. The firing bits leave `desired` BEFORE the flag clears, so there is no
     * tick at which the scheduler is allowed to write a mask that still has them. The tick
     * that notices the expiry grants nothing anyway, so the first write after a window is at
     * least one tick later, against an already-safe mask.
     *
     * pendingWrite is set so the next RYN4_SET tick actually pushes the mask out rather than
     * waiting for the control logic to touch a relay. Mirrors RelayState::setRelay(), which
     * does the same fetch_and plus pendingWrite.
     */
    void endRelease(ModbusBusRelease::Ended how) {
        const uint8_t before = g_relayState.desired.load(std::memory_order_acquire);
        const uint8_t safe = BusReleasePolicy::safeDesiredMask(before);
        if (safe != before) {
            g_relayState.desired.fetch_and(static_cast<uint8_t>(~BusReleasePolicy::FIRING_MASK),
                                           std::memory_order_acq_rel);
            g_relayState.pendingWrite.store(true, std::memory_order_release);
        }
        s_lastEnd.store(static_cast<uint8_t>(how), std::memory_order_release);
        s_released.store(false, std::memory_order_release);
        s_changed.store(true, std::memory_order_release);
    }
}

namespace ModbusBusRelease {

BusReleasePolicy::Refusal release(uint32_t seconds) {
    if (s_released.load(std::memory_order_acquire)) {
        // Already released: extend, never shorten. The comment here used to claim exactly
        // that while the code did the opposite - it stored `now + requested` unconditionally,
        // so a second call with a small value clamped up to MIN_SECONDS and cut a 15-minute
        // window down to 30 s under whoever was using it. Keep the later deadline.
        const uint32_t requested = BusReleasePolicy::clampSeconds(seconds);
        const uint32_t current = s_deadlineMs.load(std::memory_order_acquire);
        const uint32_t proposed = millis() + requested * 1000u;
        const bool longer = BusReleasePolicy::isLater(proposed, current);
        if (longer) {
            s_deadlineMs.store(proposed, std::memory_order_release);
            s_changed.store(true, std::memory_order_release);
        }
        LOG_WARN(TAG, "Bus already released - window %s (%lu s left)",
                 longer ? "extended" : "kept, request was shorter",
                 static_cast<unsigned long>(BusReleasePolicy::secondsLeft(
                     millis(), s_deadlineMs.load(std::memory_order_acquire))));
        return BusReleasePolicy::Refusal::NONE;
    }

    // The hardware-verified mask, not the desired one: what the boiler is doing.
    const uint8_t actual = g_relayState.actual.load(std::memory_order_acquire);

    bool demand = false;
    Temperature_t target = 0;
    if (!BurnerStateMachine::getHeatDemandState(demand, target)) {
        // Could not read the demand under its mutex. Refuse: an unknown demand is not a
        // cleared one, and this is an operator convenience, never urgent.
        LOG_WARN(TAG, "Bus release refused - heat demand state unreadable");
        return BusReleasePolicy::Refusal::DEMAND_PRESENT;
    }

    const BurnerSMState burnerState = BurnerStateMachine::getCurrentState();
    const bool emergencyLatched =
        (SRP::getSystemStateEventBits() & SystemEvents::SystemState::EMERGENCY_STOP) != 0;

    const BusReleasePolicy::Refusal refusal =
        BusReleasePolicy::mayRelease(actual, demand, burnerState, emergencyLatched);
    if (refusal != BusReleasePolicy::Refusal::NONE) {
        LOG_WARN(TAG, "Bus release refused (%s): relays=0x%02X demand=%d sm=%d estop=%d",
                 BusReleasePolicy::toString(refusal), actual, demand ? 1 : 0,
                 static_cast<int>(burnerState), emergencyLatched ? 1 : 0);
        return refusal;
    }

    const uint32_t window = BusReleasePolicy::clampSeconds(seconds);
    s_deadlineMs.store(millis() + window * 1000u, std::memory_order_release);
    s_lastEnd.store(static_cast<uint8_t>(Ended::NOT_YET), std::memory_order_release);
    s_released.store(true, std::memory_order_release);
    s_changed.store(true, std::memory_order_release);
    LOG_WARN(TAG, "RS485 bus RELEASED to another master for %lu s - no polling, no relay "
                  "writes, burner cannot be armed",
             static_cast<unsigned long>(window));
    return BusReleasePolicy::Refusal::NONE;
}

void acquire() {
    if (!s_released.load(std::memory_order_acquire)) {
        return;
    }
    endRelease(Ended::BY_COMMAND);
    LOG_WARN(TAG, "RS485 bus reacquired by command - polling resumes");
}

bool isReleased() {
    return s_released.load(std::memory_order_acquire);
}

void expireIfDue() {
    if (!s_released.load(std::memory_order_acquire)) {
        return;
    }
    if (!BusReleasePolicy::expired(millis(), s_deadlineMs.load(std::memory_order_acquire))) {
        return;
    }
    endRelease(Ended::BY_TIMEOUT);
    // No logging here: this runs in the coordinator's timer callback, and a single LOG_*
    // costs ~1312 B of stack on this project. The publisher logs the transition instead.
}

uint32_t secondsLeft() {
    if (!s_released.load(std::memory_order_acquire)) {
        return 0;
    }
    return BusReleasePolicy::secondsLeft(millis(), s_deadlineMs.load(std::memory_order_acquire));
}

bool consumeStateChange() {
    return s_changed.exchange(false, std::memory_order_acq_rel);
}

Ended lastEnd() {
    return static_cast<Ended>(s_lastEnd.load(std::memory_order_acquire));
}

} // namespace ModbusBusRelease
