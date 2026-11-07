// src/modules/diagnostics/SyslogWindow.cpp
#include "modules/diagnostics/SyslogWindow.h"

#include <Arduino.h>  // millis()
#include <Syslog.h>
#include <atomic>
#include <esp_heap_caps.h>

#include "LoggingMacros.h"
#include "config/SystemSettingsStruct.h"
#include "core/SystemResourceProvider.h"
#include "modules/tasks/OTATask.h"

static const char* TAG = "SyslogWin";

namespace {

/**
 * The one and only Syslog instance, constructed at static-init and never destroyed.
 *
 * Costs ~260 B permanently (the object plus its StaticSemaphore_t - the library's mutex is
 * xSemaphoreCreateMutexStatic on a member, so construction touches no heap). Everything that
 * actually costs memory - the 512 B message buffer, the EthernetUDP object with its own 512 B
 * buffer, and the lwIP socket, ~1.4 kB together - is allocated by begin() and freed by end(),
 * so it exists only while a window is open.
 *
 * See the header for why this must NOT be a `new`.
 */
Syslog s_syslog("esp32-boiler", "boiler");

std::atomic<bool> s_open{false};
std::atomic<uint32_t> s_deadlineMs{0};
std::atomic<bool> s_changed{false};
std::atomic<bool> s_subscribed{false};
std::atomic<uint8_t> s_lastEnd{static_cast<uint8_t>(SyslogWindow::Ended::NOT_YET)};

/**
 * Runs on the Logger task, nested inside processDeferred. Kept trivial on purpose: with the
 * library's heap buffer this frame is 64 B, and everything below it is the library's.
 */
void syslogWindowCallback(esp_log_level_t level, const char* tag, const char* message) {
    s_syslog.send(level, tag, message);
}

/**
 * @brief Tear the window down. The ORDER here is the whole safety argument.
 *
 * Deregister FIRST, so no NEW line can enter the send path, then end(), which waits for any
 * in-flight send under the library's mutex before freeing the socket and buffer. A call that
 * slipped past the deregistration lands on the static instance and is rejected by
 * `!initialized_` under that same mutex. Never free before deregistering.
 */
void endWindow(SyslogWindow::Ended how) {
    if (s_subscribed.exchange(false, std::memory_order_acq_rel)) {
#if defined(USE_CUSTOM_LOGGER) && !defined(LOG_NO_CUSTOM_LOGGER)
        SRP::getLogger().removeLogSubscriber(syslogWindowCallback);
#endif
    }
    s_syslog.end();
    SRP::setSyslog(nullptr);
    s_lastEnd.store(static_cast<uint8_t>(how), std::memory_order_release);
    s_open.store(false, std::memory_order_release);
    s_changed.store(true, std::memory_order_release);
}

}  // namespace

namespace SyslogWindow {

SyslogWindowPolicy::Refusal open(uint32_t seconds) {
    const uint32_t window = SyslogWindowPolicy::clampSeconds(seconds);

    if (s_open.load(std::memory_order_acquire)) {
        // Extend, never shorten. BusReleasePolicy carried exactly this bug for a while - the
        // comment promised extend-only while the code stored the new deadline unconditionally,
        // so a second short request cut a long window out from under whoever was using it.
        const uint32_t current = s_deadlineMs.load(std::memory_order_acquire);
        const uint32_t proposed = millis() + window * 1000u;
        const bool longer = SyslogWindowPolicy::isLater(proposed, current);
        if (longer) {
            s_deadlineMs.store(proposed, std::memory_order_release);
            s_changed.store(true, std::memory_order_release);
        }
        LOG_WARN(TAG, "Syslog window already open - %s (%lu s left)",
                 longer ? "extended" : "kept, request was shorter",
                 static_cast<unsigned long>(secondsLeft()));
        return SyslogWindowPolicy::Refusal::NONE;
    }

    // VALIDATE BEFORE ALLOCATING. The opposite order to ModbusBusRelease, and deliberately so:
    // there, claiming first closes a race where the burner could arm during validation, and the
    // claim is a cheap flag. Here the "claim" would be a 1.4 kB allocation on a device with
    // single-digit kB of DRAM headroom, and nothing can arm anything by logging - the worst a
    // concurrent line can do is not be sent. So the cheap check goes first.
    SystemSettings& settings = SRP::getSystemSettings();
    const bool configured = (settings.syslogServerIP[0] | settings.syslogServerIP[1] |
                             settings.syslogServerIP[2] | settings.syslogServerIP[3]) != 0;
    const size_t block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

    const SyslogWindowPolicy::Refusal refusal =
        SyslogWindowPolicy::mayOpen(settings.syslogEnabled, OTATask::isOtaInProgress(),
                                    block, configured);
    if (refusal != SyslogWindowPolicy::Refusal::NONE) {
        LOG_WARN(TAG, "Syslog window refused (%s): enabled=%d block=%u ota=%d configured=%d",
                 SyslogWindowPolicy::toString(refusal), settings.syslogEnabled ? 1 : 0,
                 static_cast<unsigned>(block), OTATask::isOtaInProgress() ? 1 : 0,
                 configured ? 1 : 0);
        return refusal;
    }

    const IPAddress serverIP(settings.syslogServerIP[0], settings.syslogServerIP[1],
                             settings.syslogServerIP[2], settings.syslogServerIP[3]);

    if (!s_syslog.begin(serverIP, settings.syslogPort,
                        static_cast<Syslog::Facility>(settings.syslogFacility))) {
        // ALWAYS end() a failed begin(): it frees whatever a part-way begin left behind (the
        // UDP object may exist without the buffer, or vice versa). Not calling it leaks on
        // every failed attempt, and a window is something an operator retries.
        s_syslog.end();
        LOG_ERROR(TAG, "Syslog window: begin() failed for %u.%u.%u.%u:%u",
                  settings.syslogServerIP[0], settings.syslogServerIP[1],
                  settings.syslogServerIP[2], settings.syslogServerIP[3], settings.syslogPort);
        return SyslogWindowPolicy::Refusal::LOW_DRAM;
    }

    s_syslog.setMinLevel(static_cast<esp_log_level_t>(settings.syslogMinLevel));
    SRP::setSyslog(&s_syslog);

    // Deadline before the flag, so a concurrent expireIfDue() cannot read a stale deadline
    // from a previous window and close this one the instant it opens.
    s_deadlineMs.store(millis() + window * 1000u, std::memory_order_release);
    s_lastEnd.store(static_cast<uint8_t>(Ended::NOT_YET), std::memory_order_release);
    s_open.store(true, std::memory_order_release);

    // Subscribe LAST. Until this line nothing enters the send path at all, which is the point
    // of the whole design: a closed window costs zero per log line, not a rejected call.
#if defined(USE_CUSTOM_LOGGER) && !defined(LOG_NO_CUSTOM_LOGGER)
    if (SRP::getLogger().addLogSubscriber(syslogWindowCallback)) {
        s_subscribed.store(true, std::memory_order_release);
    } else {
        LOG_ERROR(TAG, "Syslog window: could not register log subscriber");
        endWindow(Ended::BY_COMMAND);
        return SyslogWindowPolicy::Refusal::LOW_DRAM;
    }
#endif

    s_changed.store(true, std::memory_order_release);
    LOG_WARN(TAG, "Syslog window OPEN for %lu s -> %u.%u.%u.%u:%u minLevel=%u",
             static_cast<unsigned long>(window),
             settings.syslogServerIP[0], settings.syslogServerIP[1],
             settings.syslogServerIP[2], settings.syslogServerIP[3],
             settings.syslogPort, settings.syslogMinLevel);
    return SyslogWindowPolicy::Refusal::NONE;
}

void close() {
    if (!s_open.load(std::memory_order_acquire)) {
        return;
    }
    endWindow(Ended::BY_COMMAND);
    LOG_WARN(TAG, "Syslog window closed by command");
}

bool isOpen() {
    return s_open.load(std::memory_order_acquire);
}

void expireIfDue() {
    if (!s_open.load(std::memory_order_acquire)) {
        return;
    }
    if (!SyslogWindowPolicy::expired(millis(), s_deadlineMs.load(std::memory_order_acquire))) {
        return;
    }
    endWindow(Ended::BY_TIMEOUT);
    // No logging here. This runs from a periodic caller, and one LOG_* costs ~1312 B of stack
    // on this project - the same reason ModbusBusRelease::expireIfDue() is silent. The
    // publisher reports the transition via consumeStateChange().
}

uint32_t secondsLeft() {
    if (!s_open.load(std::memory_order_acquire)) {
        return 0;
    }
    return SyslogWindowPolicy::secondsLeft(millis(),
                                           s_deadlineMs.load(std::memory_order_acquire));
}

void closeForOTA() {
    if (!s_open.load(std::memory_order_acquire)) {
        return;
    }
    endWindow(Ended::BY_OTA);
    LOG_WARN(TAG, "Syslog window closed - OTA starting, its DRAM is needed for the transfer");
}

bool consumeStateChange() {
    return s_changed.exchange(false, std::memory_order_acq_rel);
}

Ended lastEnd() {
    return static_cast<Ended>(s_lastEnd.load(std::memory_order_acquire));
}

const char* endedToString(Ended e) {
    switch (e) {
        case Ended::NOT_YET:    return "not_yet";
        case Ended::BY_COMMAND: return "command";
        case Ended::BY_TIMEOUT: return "timeout";
        case Ended::BY_OTA:     return "ota";
    }
    return "unknown";
}

}  // namespace SyslogWindow
