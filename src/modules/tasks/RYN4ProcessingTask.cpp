// src/modules/tasks/RYN4ProcessingTask.cpp
// Handles scheduled Modbus operations for RYN4 relay module via ModbusCoordinator
#include "modules/tasks/RYN4ProcessingTask.h"
#include "RYN4.h"
#include "LoggingMacros.h"
#include "core/SystemResourceProvider.h"
#include "core/ModbusCoordinator.h"
#include "config/SystemConstants.h"
#include "shared/RelayState.h"
#include "shared/SharedRelayReadings.h"
#include "shared/RelayBindings.h"
#include "modules/tasks/RelayVerificationManager.h"  // F4: relay-health escalation
#include "events/SystemEventsGenerated.h"
#include <TaskManager.h>
#include <IDeviceInstance.h>
#include <climits>  // For ULONG_MAX
#include <array>

static const char* TAG = "RYN4Processing";

// Task handle for coordinator registration
static TaskHandle_t ryn4ProcessingTaskHandle = nullptr;

// DELAY watchdog: Track SET tick count
static uint32_t g_setTickCounter = 0;

// Helper: Send DELAY commands for state changes
static void sendDelayCommands(RYN4* ryn4, uint8_t desired) {
    std::array<RYN4::RelayCommandSpec, 8> commands;

    for (int i = 0; i < 8; i++) {
        bool shouldBeOn = (desired >> i) & 0x01;
        if (shouldBeOn) {
            // Relay ON: Use DELAY watchdog (auto-OFF in 10s if not renewed)
            uint8_t delaySeconds = SystemConstants::Relay::DELAY_WATCHDOG_SECONDS;
            commands[i] = {ryn4::RelayAction::DELAY, delaySeconds};
            // Track DELAY - don't verify until timer would expire
            g_relayState.setDelayCommand(i, delaySeconds);
        } else {
            // Relay OFF: Use DELAY 0 to cancel any active timer
            commands[i] = {ryn4::RelayAction::DELAY, 0};

            // Track DELAY 0 - verify immediately on next READ tick
            // Hardware responds <100ms, READ tick at 1000ms provides sufficient time
            g_relayState.setDelayCommand(i, 0);
        }
    }

    ryn4::RelayErrorCode result = ryn4->setMultipleRelayCommands(commands);

    if (result == ryn4::RelayErrorCode::SUCCESS) {
        // Update 'sent' state - this is what we verify against in READ tick
        g_relayState.sent.store(desired, std::memory_order_release);
        LOG_INFO(TAG, "Relay DELAY commands sent: 0x%02X", desired);
    } else {
        LOG_ERROR(TAG, "Failed to send DELAY commands: %d", static_cast<int>(result));
        g_relayState.pendingWrite.store(true, std::memory_order_release);
    }
}

// Helper: Renew DELAY for contiguous ON relays (minimal Modbus traffic)
// Returns true only if EVERY block was renewed successfully. The caller (F14)
// uses this to retry on the next SET tick rather than waiting a full parity
// cycle, so a single lost renewal does not race the 10s hardware auto-OFF.
static bool sendCompactDelayRenewal(RYN4* ryn4, uint8_t desired) {
    // Find contiguous blocks of ON relays and renew them efficiently
    // E.g., if R1,R2,R5 are ON: send R1-R2 (4 bytes), then R5 (2 bytes)
    // Much more efficient than sending all 8 relays (16 bytes)

    uint8_t start = 0xFF;
    uint8_t count = 0;
    bool allOk = true;

    for (int i = 0; i <= 8; i++) {  // Loop to 8 to flush last block
        bool isOn = (i < 8) && ((desired >> i) & 0x01);

        if (isOn && start == 0xFF) {
            // Start of contiguous block
            start = i;
            count = 1;
        } else if (isOn && start != 0xFF) {
            // Continue block
            count++;
        } else if (!isOn && start != 0xFF) {
            // End of block - send renewal for start..start+count-1.
            //
            // setRelayCommandRange (ESP32-RYN4 v0.1.0, added 2026-10-18 at my request)
            // replaces a hand-rolled register write that went around the library:
            // writeMultipleRegisters(uint16_t, const std::vector<uint16_t>&) is declared
            // in ESP32-ModbusDevice and inherited by RYN4, so it forced a heap
            // allocation - 2-16 B, but on the path that owns the DELAY watchdog, at a 5 s
            // cadence, in a project that avoids heap on runtime paths. It also meant
            // encoding the wire format here (0x0600 | seconds), duplicating knowledge
            // that belongs in the library. Both are gone: the library builds the register
            // payload on the stack and owns the encoding.
            //
            // Indices are 0-based and so is `start`; the library documents
            // "startRelay = 0 targets Relay 1". Timing is unchanged: the new method is
            // single-attempt, because RetryPolicies::modbusDefault() is maxRetries = 0,
            // exactly like writeMultipleRegisters has always been. That matters - F14
            // below depends on failing fast and retrying on the NEXT SET tick rather
            // than blocking, so a lost renewal cannot race the 10 s hardware auto-OFF.
            const ryn4::RelayErrorCode result = ryn4->setRelayCommandRange(
                start, count,
                RYN4::RelayCommandSpec{ryn4::RelayAction::DELAY,
                                       SystemConstants::Relay::DELAY_WATCHDOG_SECONDS});
            if (result == ryn4::RelayErrorCode::SUCCESS) {
                // Update tracking
                for (int j = start; j < start + count; j++) {
                    g_relayState.setDelayCommand(j, SystemConstants::Relay::DELAY_WATCHDOG_SECONDS);
                }
                LOG_DEBUG(TAG, "DELAY renewed: R%d-%d (%d bytes)", start + 1, start + count, count * 2);
            } else {
                LOG_ERROR(TAG, "Failed DELAY renewal: R%d-%d (err %d)",
                          start + 1, start + count, static_cast<int>(result));
                allOk = false;
            }

            start = 0xFF;
            count = 0;
        }
    }

    return allOk;
}

// Handle SET tick - DELAY watchdog with staggered renewal
static void handleSetTick(RYN4* ryn4) {
    g_setTickCounter++;

    // F34: test-and-clear pendingWrite BEFORE snapshotting desired. If a writer
    // (RelayControlTask, other core) updates desired and sets pendingWrite AFTER
    // our exchange, the flag stays set and the change is handled next tick,
    // instead of being cleared while we transmit a stale bitmask.
    bool hasPendingWrite = g_relayState.pendingWrite.exchange(false, std::memory_order_acq_rel);
    uint8_t desired = g_relayState.desired.load(std::memory_order_acquire);

    // F14: renew on an elapsed-time schedule (not tick parity) so a single lost
    // renewal is retried on the very next SET tick instead of waiting a full 5s
    // parity cycle and racing the 10s hardware auto-OFF.
    //
    // THE MARGIN WAS 2.5 s, NOT 5 s, FOR AS LONG AS THIS CODE EXISTED. renewalIntervalMs is
    // half the watchdog, but s_lastRenewalMs used to be stamped with millis() AFTER the Modbus
    // transaction returned, so it carried the transaction time. SET ticks arrive on a FIXED
    // 2.5 s grid (RYN4_SET_TICKS = {1,6} of ten 500 ms ticks), so the tick 5.0 s later saw
    // 5000 - delta, was not due, and the renewal actually landed at 7.5 s - leaving 2.5 s to
    // hardware expiry instead of 5 s. A single failed renewal then retried at 10.0 s, i.e. AT
    // the auto-OFF, and the "alarm after 2 misses, still before expiry" below fired at 10.0 s
    // too. One CRC-corrupted reply mid-burn therefore dropped BURNER_ENABLE and re-energised
    // it a moment later: an unlogged flame-out and a relight with no pre-purge, invisible to
    // isFlameDetected() because that only reports the relay we commanded. The bus does produce
    // errors - 12 in 8198 reads measured on the room sensor. Found 2026-10-28.
    //
    // Stamping the TICK time instead of the completion time puts the renewal back on the 5.0 s
    // tick with a real 5 s margin, and leaves the retry at 7.5 s, 2.5 s before expiry.
    static uint32_t s_lastRenewalMs = 0;
    static bool     s_renewalRetryPending = false;
    static uint8_t  s_renewalMissCount = 0;
    const uint32_t renewalIntervalMs =
        (static_cast<uint32_t>(SystemConstants::Relay::DELAY_WATCHDOG_SECONDS) * 1000u) / 2u;

    // The time this tick began, used as the renewal stamp. Deliberately NOT millis() after
    // the transaction: see above.
    const uint32_t tickMs = millis();

    // Prioritize state changes over renewal
    if (hasPendingWrite) {
        LOG_INFO(TAG, "SET tick - state change: 0x%02X", desired);
        sendDelayCommands(ryn4, desired);
        // A state change also (re)issues DELAY commands, so it counts as a renewal.
        s_lastRenewalMs = tickMs;
        s_renewalRetryPending = false;
        s_renewalMissCount = 0;

    } else if (desired != 0) {
        // Compact renewal: Send only contiguous ON relay blocks
        // Example: R1,R2,R5 ON → 2 transactions (R1-R2: 4 bytes, R5: 2 bytes)
        uint32_t sinceRenewal = static_cast<uint32_t>(millis() - s_lastRenewalMs);
        bool due = s_renewalRetryPending || (sinceRenewal >= renewalIntervalMs);
        if (due) {
            LOG_DEBUG(TAG, "SET tick - compact DELAY renewal%s", s_renewalRetryPending ? " (retry)" : "");
            if (sendCompactDelayRenewal(ryn4, desired)) {
                s_lastRenewalMs = tickMs;
                s_renewalRetryPending = false;
                s_renewalMissCount = 0;
            } else {
                // Retry on the NEXT SET tick (2.5s) rather than the next parity cycle.
                //
                // ALARM ON THE FIRST MISS. With the renewal on the 5.0 s tick, a miss leaves
                // exactly ONE retry (7.5 s) before the 10 s hardware auto-OFF - so the first
                // miss is already the last warning, and waiting for a second meant alarming
                // at the moment the relays dropped. The condition is also the only way to see
                // this at all: relay read-back verification cannot detect it, because the
                // relays are still obeying their last command right up to expiry.
                s_renewalRetryPending = true;
                if (++s_renewalMissCount >= 1) {
                    LOG_ERROR(TAG, "DELAY renewal failed %d time(s) - every ON relay auto-OFFs "
                                   "in ~%lu ms unless the retry succeeds",
                              s_renewalMissCount,
                              (unsigned long)(SystemConstants::Relay::DELAY_WATCHDOG_SECONDS * 1000u
                                              - renewalIntervalMs));
                    // This is the case nothing else covers, and the reason COMM_ERROR is now
                    // read by SafetyInterlocks: two missed renewals means every ON relay is
                    // about 2.5 s from a hardware auto-OFF with no post-purge and no pump
                    // overrun. Stopping the burn deliberately beats being dropped by the
                    // watchdog. Relay read-back verification cannot see this at all - the
                    // relays are still following their last command, for now.
                    EventGroupHandle_t relayStatusEventGroup = SRP::getRelayStatusEventGroup();
                    if (relayStatusEventGroup) {
                        xEventGroupSetBits(relayStatusEventGroup, SystemEvents::RelayStatus::COMM_ERROR);
                        xEventGroupClearBits(relayStatusEventGroup, SystemEvents::RelayStatus::SYNCHRONIZED);
                    }
                }
            }
        } else {
            LOG_DEBUG(TAG, "SET tick - no renewal (%lu ms since last)", (unsigned long)sinceRenewal);
        }

    } else {
        // All relays OFF - no renewal needed
        LOG_DEBUG(TAG, "SET tick - all relays OFF");
        s_renewalRetryPending = false;
        s_renewalMissCount = 0;
    }

    // Clean up expired DELAY timers
    uint8_t delayMask = g_relayState.delayMask.load(std::memory_order_acquire);
    if (delayMask != 0) {
        for (int i = 0; i < 8; i++) {
            if (delayMask & (1 << i)) {
                if (!g_relayState.isDelayActive(i)) {
                    g_relayState.clearDelay(i);
                    LOG_DEBUG(TAG, "DELAY expired for relay %d", i + 1);
                }
            }
        }
    }
}

// Handle READ tick - verify relay states match desired
static void handleReadTick(RYN4* ryn4) {
    LOG_DEBUG(TAG, "READ tick - verifying relay states");

    // Read actual relay states as bitmap
    auto bitmapResult = ryn4->readBitmapStatus(true);  // true = update internal cache

    if (bitmapResult.isError()) {
        LOG_ERROR(TAG, "Failed to read relay status bitmap");
        return;
    }

    uint16_t actualBitmap = bitmapResult.value();
    uint8_t actual = static_cast<uint8_t>(actualBitmap & 0xFF);  // Only lower 8 bits for 8 relays
    uint8_t sent = g_relayState.sent.load(std::memory_order_acquire);  // Verify against what we sent

    // Update actual state
    g_relayState.actual.store(actual, std::memory_order_release);

    // Update SharedRelayReadings for MQTT publishing
    if (SRP::takeRelayReadingsMutex(pdMS_TO_TICKS(100))) {
        for (int i = 0; i < 8; i++) {
            bool* statePtr = RelayBindings::getStatePtr(i);
            if (statePtr) {
                *statePtr = (actual >> i) & 0x01;
            }
        }
        SRP::giveRelayReadingsMutex();
        SRP::setRelayEventBits(SystemEvents::RelayControl::DATA_AVAILABLE);
    }

    // COMM_OK means "the read came back", which is all we know at this point.
    //
    // SYNCHRONIZED used to be set here too, unconditionally, BEFORE the mismatch check below -
    // so it meant "a read succeeded" rather than "the relays are doing what we commanded", was
    // never cleared by anything, and therefore made SafetyInterlocks' `communicationOk` term
    // permanently true from the first successful read onwards. The 2026-10-25 review found it.
    // It is now earned per verification, in the two branches below.
    EventGroupHandle_t relayStatusEventGroup = SRP::getRelayStatusEventGroup();
    if (relayStatusEventGroup) {
        xEventGroupSetBits(relayStatusEventGroup, SystemEvents::RelayStatus::COMM_OK);
    }

    // Check for mismatch - compare against 'sent' (what we commanded).
    // F15: the DELAY-skip must be DIRECTION-aware. A relay is only given an
    // active DELAY when commanded ON, so a DELAY relay reading OFF (sent=ON,
    // actual=OFF) is a genuine failure and must be caught, not masked. Mask ONLY
    // DELAY relays commanded OFF (benign coast-down of a prior DELAY). The old
    // `mismatchMask & ~delayMask` masked EVERY energized relay, leaving ON-relay
    // verification permanently inert (a stuck-OFF pump/burner relay went unseen).
    uint8_t mismatchMask = static_cast<uint8_t>(actual ^ sent);
    uint8_t delayMask = g_relayState.delayMask.load(std::memory_order_acquire);
    uint8_t maskableDelay = static_cast<uint8_t>(delayMask & ~sent);  // DELAY + commanded OFF
    uint8_t realMismatch = static_cast<uint8_t>(mismatchMask & ~maskableDelay);

    // F4: feed the per-relay health escalation (previously dead code). A
    // persistent real mismatch on BURNER_ENABLE escalates to a CRITICAL failsafe
    // (emergency shutdown); other relays escalate to WARNING. success == no real
    // mismatch on that relay, which also resets its failure counter.
    static uint8_t s_relayHealthFailures[8] = {0};
    for (int i = 0; i < 8; i++) {
        bool relayMismatch = (realMismatch >> i) & 0x01;
        RelayVerificationManager::checkRelayHealthAndEscalate(
            static_cast<uint8_t>(i + 1), !relayMismatch, s_relayHealthFailures);
    }

    // Error::RELAY had NO reachable clear. Its only xEventGroupClearBits lives in
    // ErrorRecovery.cpp:403, and ErrorRecovery is dead code - reachable only through the
    // REPORT_ERROR/REPORT_WARNING/REPORT_CRITICAL macros in its own header, none of which is
    // used anywhere in src/. So one escalation from checkRelayHealthAndEscalate() latched the
    // bit until a power cycle, and Error::RELAY is one of SafetyInterlocks' CRITICAL_ERRORS
    // (SafetyInterlocks.cpp:362-366): every later ignition failed SAFETY_FAILED and
    // boiler/cmd/emergency_reset was refused. A flaky-bus moment therefore meant no heat and
    // no remote way back. Exactly the defect fixed for Error::SENSOR_FAILURE in c204a6d, on
    // the sibling bit set by the very same line (BurnerSystemController.cpp:220). Found by
    // the 2026-10-25 review.
    //
    // Cleared only on a FULLY clean verification - every relay's read-back matching what was
    // commanded - rather than on one relay's success. That matters twice: a still-broken
    // relay cannot clear an error raised by another, and because escalation deliberately
    // zeroes its own failure counter, a per-relay clear would let the bit flap on and off
    // between a broken relay's failures, briefly re-permitting the burner each time.
    if (realMismatch == 0) {
        EventGroupHandle_t errorGroup = SRP::getErrorNotificationEventGroup();
        if (errorGroup) {
            xEventGroupClearBits(errorGroup, SystemEvents::Error::RELAY);
        }
        // The same reasoning applies to the relay-status bits, and for the same reason: a
        // COMM_ERROR nobody clears would be a second unreleasable latch, which is the defect
        // this block exists to fix. Set and cleared together on a fully clean verification.
        EventGroupHandle_t statusGroup = SRP::getRelayStatusEventGroup();
        if (statusGroup) {
            xEventGroupClearBits(statusGroup, SystemEvents::RelayStatus::COMM_ERROR);
            xEventGroupSetBits(statusGroup, SystemEvents::RelayStatus::SYNCHRONIZED);
        }
    }

    if (realMismatch != 0) {
        // We have real mismatches (energized relays that failed to actuate, or
        // OFF relays that failed to release)
        uint8_t mismatches = g_relayState.consecutiveMismatches.fetch_add(1, std::memory_order_acq_rel) + 1;

        if (mismatches == 1) {
            // First mismatch - likely timing issue
            LOG_DEBUG(TAG, "Relay verification pending (attempt 1/2): Sent: 0x%02X, Actual: 0x%02X",
                      sent, actual);
        } else {
            // Persistent mismatch - real problem!
            LOG_ERROR(TAG, "Relay verification FAILED after %d attempts! Sent: 0x%02X, Actual: 0x%02X",
                      mismatches, sent, actual);

            // Log individual real mismatches
            for (int i = 0; i < 8; i++) {
                if (realMismatch & (1 << i)) {
                    bool sentBit = (sent >> i) & 0x01;
                    bool actualBit = (actual >> i) & 0x01;
                    LOG_ERROR(TAG, "  Relay %d: sent=%s, actual=%s",
                              i + 1, sentBit ? "ON" : "OFF", actualBit ? "ON" : "OFF");
                }
            }

            // Set communication error event bit AND drop SYNCHRONIZED. Setting COMM_ERROR
            // alone was inert: no code read that bit, and the one term that could have
            // noticed - SafetyInterlocks' `communicationOk` - reads SYNCHRONIZED, which was
            // set unconditionally above and never cleared.
            EventGroupHandle_t relayStatusEventGroup = SRP::getRelayStatusEventGroup();
            if (relayStatusEventGroup) {
                xEventGroupSetBits(relayStatusEventGroup, SystemEvents::RelayStatus::COMM_ERROR);
                xEventGroupClearBits(relayStatusEventGroup, SystemEvents::RelayStatus::SYNCHRONIZED);
            }
        }

        // Queue retry
        g_relayState.pendingWrite.store(true, std::memory_order_release);

    } else {
        // No real mismatch - reset counter. Covers exact match AND the benign
        // masked-DELAY case (a commanded-OFF relay still coasting on a prior DELAY).
        uint8_t previousMismatches = g_relayState.consecutiveMismatches.exchange(0, std::memory_order_acq_rel);

        if (previousMismatches > 0) {
            LOG_INFO(TAG, "Relay verification SUCCESS after %d attempts: 0x%02X",
                     previousMismatches + 1, actual);
        } else if (actual != sent) {
            LOG_DEBUG(TAG, "Relay verification deferred (DELAY coast-down): Sent: 0x%02X, Actual: 0x%02X, Delay mask: 0x%02X",
                      sent, actual, delayMask);
        } else {
            LOG_DEBUG(TAG, "Relay states verified: 0x%02X", actual);
        }
    }
}

/**
 * @brief Clear a latched RYN4 offline state, or finish a boot init that failed.
 *
 * ONE OWNER FOR BOTH CASES. A failed boot probe and a mid-life module death now land in the
 * same place - statusFlags.moduleOffline - and both make every relay write refused, which the
 * pre-ignition gate in BurnerSystemController turns into "no burner". So both need the same
 * recovery, and this task is the only one that can do it: it runs for the life of the device
 * and already owns the relay path. The boot-time retry briefly lived in the ModbusInit task
 * instead, which was wrong - that task self-deletes, and it watched isInitialized(), which
 * stays TRUE through a runtime death.
 *
 * THE ESCAPE HATCH MATTERS. Once moduleOffline latches, the library short-circuits its public
 * control and read methods with MODBUS_ERROR, so no amount of ordinary polling will clear it -
 * nothing goes out on the wire. probeModule() is the way out: it ALWAYS hits the wire via the
 * base class, bypassing both the latch gate and the passive shortcut in isModuleResponsive(),
 * and a reply clears the latch through the normal response path. Without it, the library's
 * runtime latching would turn a recoverable bus glitch into a permanently dead relay layer.
 *
 * Rate-limited because an unresponsive probe costs a ~1 s bus timeout on a half-duplex bus
 * shared with both sensor modules and this same relay board.
 */
static void attemptRYN4Recovery(RYN4* ryn4) {
    if (!ryn4) {
        return;
    }
    const bool needsRecovery = !ryn4->isInitialized() || ryn4->isModuleOffline();
    if (!needsRecovery) {
        return;
    }

    static uint32_t lastAttemptMs = 0;
    constexpr uint32_t RECOVERY_INTERVAL_MS = 30000;
    const uint32_t nowMs = millis();
    if (lastAttemptMs != 0 && (nowMs - lastAttemptMs) < RECOVERY_INTERVAL_MS) {
        return;
    }

    lastAttemptMs = nowMs;

    // probeModule() ALWAYS hits the wire. It reads the return-delay register through the base
    // class, so it bypasses both the moduleOffline latch gate on the RYN4-level methods and
    // the passive shortcut in isModuleResponsive(); a reply clears the latch through the
    // library's normal response path, a failure increments the consecutive-failure counter.
    //
    // This replaced a local 30 s constant and a "wait out the passive window" branch. The
    // problem that branch existed for: isModuleResponsive() returns true WITHOUT touching the
    // wire when a reply arrived inside its own (private) RESPONSIVE_TIMEOUT, and while the
    // latch is set no traffic goes out - so a recent timestamp is stale evidence from before
    // the latch, and probing then reported "not answering" about a module never asked. My
    // workaround duplicated the library's deadline, which is the shape I have spent this
    // session deleting elsewhere. probeModule() makes the question unambiguous instead:
    // "is it responsive (cached)" and "ask it right now" are different, and latch recovery
    // wants the second. Added to ESP32-RYN4 on request, 2026-10-28.
    if (ryn4->probeModule() && !ryn4->isModuleOffline()) {
        LOG_WARN(TAG, "RYN4 answered a probe - offline latch cleared, relays are controllable");
        return;
    }

    // Still offline, or never initialised at all: a full init is the heavier option and is
    // what a boot-time failure actually needs.
    if (!ryn4->isInitialized()) {
        LOG_WARN(TAG, "RYN4 uninitialised - retrying initialize()");
        if (ryn4->initialize()) {
            LOG_WARN(TAG, "RYN4 initialised on retry - relays are controllable again");
            ryn4->setWatchdogSeconds(SystemConstants::Relay::DELAY_WATCHDOG_SECONDS);
        }
        return;
    }

    // getConsecutiveFailures() vs the relay read-back verification are two different signals,
    // which is why both are reported: a climbing failure count means the module is not TALKING,
    // whereas SYNCHRONIZED dropping with a count of 0 means it is talking and the contacts
    // disagree with what we commanded. Different faults, different responses.
    LOG_ERROR(TAG, "RYN4 is offline and not answering (%u consecutive failures, last reply "
                   "%lu ms ago) - every relay write is being refused",
              ryn4->getConsecutiveFailures(),
              (unsigned long)((xTaskGetTickCount() - ryn4->getLastResponseTime())
                              * portTICK_PERIOD_MS));
}

void RYN4ProcessingTask(void* parameter) {
    auto* ryn4 = static_cast<RYN4*>(parameter);

    if (!ryn4) {
        LOG_ERROR(TAG, "Started with null RYN4 instance");
        vTaskDelete(NULL);
        return;
    }

    ryn4ProcessingTaskHandle = xTaskGetCurrentTaskHandle();

    // uxTaskGetStackHighWaterMark returns BYTES on ESP32 (portSTACK_TYPE is uint8_t,
    // so sizeof(StackType_t) == 1). The "* 4" this used to carry reported 4x the real
    // free stack - a 400 B margin printed as "Stk:1600". Corrected 2026-09-18.
    LOG_INFO(TAG, "Started C%d Stk:%d", xPortGetCoreID(),
             uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));

    // Register with watchdog
    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::enabled(
        false,  // not critical
        SystemConstants::System::WDT_SENSOR_PROCESSING_MS
    );

    if (!SRP::getTaskManager().registerCurrentTaskWithWatchdog("RYN4Processing", wdtConfig)) {
        LOG_WARN(TAG, "Failed to register with watchdog");
    } else {
        LOG_INFO(TAG, "WDT OK %lums", SystemConstants::System::WDT_SENSOR_PROCESSING_MS);
    }

    // Wait for device initialization to complete using event bits
    LOG_INFO(TAG, "Waiting for device initialization...");

    EventGroupHandle_t deviceReadyGroup = ryn4->getExternalEventGroup();
    EventBits_t readyBit = ryn4->getReadyBit();
    EventBits_t errorBit = ryn4->getErrorBit();

    if (deviceReadyGroup != nullptr && (readyBit || errorBit)) {
        EventBits_t waitBits = readyBit | errorBit;
        EventBits_t bits = xEventGroupWaitBits(
            deviceReadyGroup,
            waitBits,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(30000)
        );

        if (bits & readyBit) {
            LOG_INFO(TAG, "Device initialization complete - ready");
        } else if (bits & errorBit) {
            LOG_WARN(TAG, "Device initialization failed - will wait for background retry");
        } else {
            LOG_WARN(TAG, "Timeout waiting for device initialization");
        }
    } else {
        LOG_WARN(TAG, "No event group configured - using fallback delay");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    // Register with ModbusCoordinator for both SET and READ operations
    auto& coordinator = ModbusCoordinator::getInstance();

    if (!coordinator.registerSensor(ModbusCoordinator::SensorType::RYN4_SET, ryn4ProcessingTaskHandle)) {
        LOG_ERROR(TAG, "Failed to register for RYN4_SET");
    } else {
        LOG_INFO(TAG, "Registered for RYN4_SET notifications");
    }

    if (!coordinator.registerSensor(ModbusCoordinator::SensorType::RYN4_READ, ryn4ProcessingTaskHandle)) {
        LOG_ERROR(TAG, "Failed to register for RYN4_READ");
    } else {
        LOG_INFO(TAG, "Registered for RYN4_READ notifications");
    }

    // Let RYN4 hardware settle after init (DELAY 0 × 8 needs processing time)
    vTaskDelay(pdMS_TO_TICKS(100));

    // Read initial relay states
    LOG_INFO(TAG, "Reading initial relay states...");
    auto bitmapResult = ryn4->readBitmapStatus(true);
    if (bitmapResult.isOk()) {
        uint8_t initial = static_cast<uint8_t>(bitmapResult.value() & 0xFF);
        g_relayState.actual.store(initial, std::memory_order_release);
        g_relayState.desired.store(initial, std::memory_order_release);  // Initialize desired to match actual
        LOG_INFO(TAG, "Initial relay states: 0x%02X", initial);
    } else {
        LOG_WARN(TAG, "Failed to read initial relay states");
    }

    LOG_INFO(TAG, "Entering main processing loop - waiting for coordinator notifications");

    // Main processing loop
    constexpr TickType_t WAIT_TIMEOUT = pdMS_TO_TICKS(SystemConstants::Timing::TASK_NOTIFICATION_TIMEOUT_MS);  // 3s timeout for watchdog

    while (true) {
        // Before anything else: if the relay layer is unusable, try to get it back. Deliberately
        // outside the notification branch - a latched module means no traffic, and recovery must
        // not depend on ticks we might not be getting.
        attemptRYN4Recovery(ryn4);

        // Wait for coordinator notification. F35: the coordinator now notifies
        // with eSetBits (a distinct bit per SensorType) so a SET and a READ tick
        // that both land while a slow transaction was in flight are coalesced
        // rather than one overwriting the other. Clear all bits on read and
        // service every pending tick.
        uint32_t notificationBits = 0;
        if (xTaskNotifyWait(0, ULONG_MAX, &notificationBits, WAIT_TIMEOUT) == pdTRUE) {
            const uint32_t setBit  = 1UL << static_cast<uint32_t>(ModbusCoordinator::SensorType::RYN4_SET);
            const uint32_t readBit = 1UL << static_cast<uint32_t>(ModbusCoordinator::SensorType::RYN4_READ);

            // review-fix: service READ before SET when both coalesced into one
            // wake (the task missed ticks under bus stress). handleReadTick then
            // verifies the PREVIOUSLY-commanded, already-settled hardware state
            // (actual vs the old `sent`); handleSetTick re-issues afterwards and
            // is verified on the next READ tick. Doing SET first would have
            // read back a just-commanded relay before it physically actuated,
            // producing a spurious mismatch that can escalate BURNER_ENABLE to a
            // failsafe from pure timing. Order is irrelevant when only one bit is
            // set (the normal non-coalesced case).
            if (notificationBits & readBit) {
                handleReadTick(ryn4);
            }
            if (notificationBits & setBit) {
                handleSetTick(ryn4);
            }
            if ((notificationBits & (setBit | readBit)) == 0) {
                LOG_WARN(TAG, "Unexpected notification bits: 0x%lX", notificationBits);
            }
        }

        // Feed watchdog
        (void)SRP::getTaskManager().feedWatchdog();
    }

    // Cleanup (should never reach here)
    coordinator.unregisterSensor(ModbusCoordinator::SensorType::RYN4_SET);
    coordinator.unregisterSensor(ModbusCoordinator::SensorType::RYN4_READ);
    vTaskDelete(NULL);
}
