// src/modules/diagnostics/SyslogWindow.h
#ifndef SYSLOG_WINDOW_H
#define SYSLOG_WINDOW_H

#include <cstdint>

#include "modules/diagnostics/SyslogWindowPolicy.h"

/**
 * @brief Runtime state for a bounded remote-logging window.
 *
 * See SyslogWindowPolicy.h for WHY this is a window rather than a setting. This header is only
 * the state and the lifecycle: open, close, expire, report.
 *
 * SHAPED AFTER ModbusBusRelease on purpose - same mutator/reader split, same extend-never-
 * shorten rule, same consumeStateChange() for the publisher. An operator who has used
 * `bus_release` should find `syslog_window` behaves the way they expect, and a maintainer
 * should be able to read one after the other without switching idioms.
 *
 * THE INSTANCE IS FILE-SCOPE AND IS NEVER DESTROYED. That is what makes deregistering the log
 * subscriber safe: the hazard in the obvious design is that Logger::invokeSubscribers copies
 * the callback list and calls it WITHOUT a lock, so removeLogSubscriber() cannot fence a call
 * that is already in flight. With a heap instance that would be a use-after-free. With a
 * static one, an in-flight call lands on a valid object and is rejected by `!initialized_`,
 * which end() clears under the very mutex sendInternal checks it under. We get a zero-cost
 * closed window AND no race, rather than having to choose.
 */
namespace SyslogWindow {

    /**
     * @brief Open (or extend) a window for `seconds`, clamped by SyslogWindowPolicy.
     * @return Refusal::NONE on success; otherwise nothing changed.
     *
     * A call while a window is already open EXTENDS it and never shortens it - the useful
     * behaviour for an operator waiting on a burn that has not happened yet.
     */
    SyslogWindowPolicy::Refusal open(uint32_t seconds);

    /// Close the window now. Safe to call when not open.
    void close();

    /// Pure read. True while remote logging is live.
    bool isOpen();

    /// Close the window if its deadline has passed. Call periodically.
    void expireIfDue();

    /// Seconds remaining, 0 when closed.
    uint32_t secondsLeft();

    /**
     * @brief Close immediately because an OTA is starting.
     *
     * Called from OTATask::onOTAStart, beside the MQTT teardown that is already there for the
     * same reason. An OTA drops the largest free DRAM block to ~1.5 kB, and the window holds
     * ~1.4 kB of the same pool - which is how the September 2026 OTA watchdog reboot happened
     * in the first place. SyslogWindowPolicy refuses to OPEN a window during an OTA; this is
     * the other half, and it is a teardown rather than a refusal because by the time a start
     * callback runs the update has already been accepted.
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

}  // namespace SyslogWindow

#endif  // SYSLOG_WINDOW_H
