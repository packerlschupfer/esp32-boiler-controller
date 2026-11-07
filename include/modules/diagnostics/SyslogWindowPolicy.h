// include/modules/diagnostics/SyslogWindowPolicy.h
#ifndef SYSLOG_WINDOW_POLICY_H
#define SYSLOG_WINDOW_POLICY_H

#include <cstddef>
#include <cstdint>

/**
 * @brief May remote logging be switched on right now, and for how long?
 *
 * Header-only and natively testable, like BusReleasePolicy, which this deliberately mirrors -
 * same shape (clamp, refusal enum, one pure decision function), because it answers the same
 * kind of question: an operator wants a bounded diagnostic window on a live heating
 * controller, and the controller decides whether that is safe and ends it by itself.
 *
 * WHY A WINDOW AND NOT A SWITCH (2026-11-07)
 *
 * Syslog was switched off on 2026-10-16 because it appeared to cost ~11 kB of DRAM on a device
 * whose steady-state free DRAM is in the single-digit kB. That figure was real but misattributed:
 * ~9.3 kB of it was the Logger's subscriber task and its 16-deep queue, which syslog merely
 * triggered by calling startSubscriberTask(). Since LOG_DEFERRED_FORMAT moved to
 * [base_debug_selective] (a75499d, 2026-10-18) that task already exists and subscribers run on
 * it synchronously, so the 9.3 kB is no longer allocated at all. Syslog's own cost is ~1.2 kB.
 *
 * So the heap objection is gone. The reason this is still a WINDOW rather than a setting is the
 * STACK, and it is the part that is easy to miss:
 *
 *   Syslog::sendInternal declares its message buffer as a local. A stack frame is reserved at
 *   function ENTRY, not where the declaration sits, so while the subscriber is registered every
 *   log line that passes the device-side level filter pays the whole frame on the Logger task -
 *   including lines that return immediately because the window is shut. Measured on the Logger
 *   task (5120 B): 2100-2112 B used without syslog, 3556-3568 B with it on the deepest path.
 *   The static bound landed 16-28 B above the live high-water mark, so that is trustworthy.
 *
 * An always-armed diagnostic channel therefore charges every log line, forever, for data nobody
 * is reading. Two independent fixes apply and both are in: the library now heap-allocates that
 * buffer in begin() (so the frame drops from 1088 B to 64 B), and this policy's window governs
 * when the subscriber is registered at all, so a shut window costs literally nothing.
 *
 * WHAT THIS DOES NOT DECIDE. Whether anything is LISTENING. The collector is an HA Core custom
 * integration bound to 0.0.0.0:514/udp; it was verified end-to-end on 2026-11-07 by sending a
 * known line and finding it stored. That verification is a fact about the house, not about this
 * firmware, and it can rot - which is why NOT_CONFIGURED below refuses a window aimed at
 * 0.0.0.0 rather than opening one that quietly sends into nothing.
 */
namespace SyslogWindowPolicy {

    /// Default and bounds for the window.
    ///
    /// The cap is longer than BusReleasePolicy's 900 s because the two wait for different
    /// things. A bus release waits for a human running a tool; this waits for the BOILER to do
    /// something worth logging, and the thing most often worth logging is a burn decision.
    /// Measured burns run 16-53 minutes, so a 10-minute window can easily contain no ignition
    /// at all and prove nothing. 30 minutes usually spans a start.
    ///
    /// It is still a hard cap rather than a comfort: every accepted line becomes a row in Home
    /// Assistant's shared recorder database, and at ERROR severity it also drives a persistent
    /// notification. This is for diagnostic windows, not for leaving logging on.
    constexpr uint32_t DEFAULT_SECONDS = 600;   // 10 minutes
    constexpr uint32_t MIN_SECONDS     = 60;
    constexpr uint32_t MAX_SECONDS     = 1800;  // 30 minutes, hard cap

    /// What opening a window actually allocates.
    ///
    /// MEASURED on hardware 2026-11-07, not estimated - the estimate here was 2048 and it was
    /// too low. Live heap_dram either side of the first window of a boot:
    ///
    ///   before open 16432 free / 14324 largest block
    ///   while open  13912 / 12788   -> 2520 B consumed
    ///   after close 15376 / 13812   -> only 1464 B returned
    ///
    /// The 1056 B difference is a ONE-TIME first-use cost, not a leak: a second window moved
    /// the figure by zero bytes (15376 / 13812 identical before and after). It is almost
    /// certainly newlib's tzset allocating the timezone string on the first localtime_r call
    /// inside Syslog::getTimestamp, which is never freed.
    ///
    /// So a FIRST window needs 2520 B and every later one needs ~1464 B. Sized for the first,
    /// since that is the worst case and the one that runs on a freshly booted controller.
    constexpr size_t WINDOW_DRAM_NEED = 2560;

    /// Left free AFTER the window's own allocation. Mirrors
    /// SystemConstants::System::MIN_DRAM_BLOCK_CRITICAL (1536 B) - not #included, to keep this
    /// header native-testable with no ESP dependencies. If that constant moves, move this.
    constexpr size_t DRAM_BLOCK_RESERVE = 1536;

    /// The largest free DRAM block a window needs to find before it may open.
    constexpr size_t REQUIRED_DRAM_BLOCK = WINDOW_DRAM_NEED + DRAM_BLOCK_RESERVE;

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
        NONE,             // may open
        // NOT "DISABLED": Arduino's esp32-hal-gpio.h #defines DISABLED to a numeric
        // constant, so an enumerator of that name does not compile. This project has been
        // bitten by that exact macro before (the heating-control diagnostics enum).
        SWITCHED_OFF,     // syslog/enabled is false - the persistent master off-switch
        OTA_ACTIVE,       // an update is running - see the note on mayOpen
        LOW_DRAM,         // largest free block too small to afford the window
        NOT_CONFIGURED    // no syslog server address set; the window would send nowhere
    };

    /**
     * @brief May a window be opened right now?
     *
     * @param enabled          the persisted syslog/enabled master switch.
     * @param otaActive        an OTA is in progress.
     * @param largestDramBlock heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT).
     * @param serverConfigured the configured server address is not 0.0.0.0.
     *
     * OTA IS REFUSED, and this is the condition that matters most. The September 2026 OTA
     * watchdog reboot was DRAM starvation, not networking: the update path drops the largest
     * free block to ~1.5 kB until onOTAStart frees memory. A window's ~1.4 kB taken from that
     * same pool at that same moment is how you reproduce that failure on purpose. The exclusion
     * is mutual - the OTA path refuses to start while a window is open - because checking only
     * one direction leaves the race that the check exists to prevent.
     *
     * NOTE ON RE-OPENING: a request while a window is already open is NOT a refusal. The
     * library's begin() is repeatable and waits for any in-flight send, so re-opening simply
     * extends the deadline. That is the useful behaviour for an operator watching for a burn
     * that has not happened yet, and it keeps the caller from having to close first.
     */
    inline Refusal mayOpen(bool enabled, bool otaActive, size_t largestDramBlock,
                           bool serverConfigured) {
        // syslog/enabled keeps its job under the window design. Without this the parameter
        // would still be accepted, persisted and published while governing nothing - a dead
        // parameter, which is a defect shape this project has found four times already. It
        // also gives an operator a persistent off-switch that survives reboots, without
        // needing a firmware change to suppress the feature.
        if (!enabled) {
            return Refusal::SWITCHED_OFF;
        }
        if (otaActive) {
            return Refusal::OTA_ACTIVE;
        }
        if (!serverConfigured) {
            return Refusal::NOT_CONFIGURED;
        }
        if (largestDramBlock < REQUIRED_DRAM_BLOCK) {
            return Refusal::LOW_DRAM;
        }
        return Refusal::NONE;
    }

    /// Short stable labels. Published on the status topic and greppable - do not reword casually.
    inline const char* toString(Refusal r) {
        switch (r) {
            case Refusal::NONE:           return "ok";
            case Refusal::SWITCHED_OFF:   return "disabled";
            case Refusal::OTA_ACTIVE:     return "ota_active";
            case Refusal::LOW_DRAM:       return "low_dram";
            case Refusal::NOT_CONFIGURED: return "not_configured";
        }
        return "unknown";
    }

    /**
     * @brief Has an open window expired?
     *
     * DELIBERATELY THE SAME IDIOM AS BusReleasePolicy::expired() - signed difference against a
     * DEADLINE, not elapsed-since-start. Two different rollover-safe time idioms in one
     * codebase is how one of them eventually gets "simplified" by someone comparing it to the
     * other. A plain `now >= deadline` breaks across the 49.7-day millis() wrap, and this
     * controller has run longer than that.
     */
    inline bool expired(uint32_t nowMs, uint32_t deadlineMs) {
        return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
    }

    /// Is `candidate` later than `reference`? Rollover-safe, same reasoning as expired().
    /// Used to make a re-request EXTEND and never SHORTEN an open window.
    inline bool isLater(uint32_t candidate, uint32_t reference) {
        return static_cast<int32_t>(candidate - reference) > 0;
    }

    /// Rounded UP, so a window with 200 ms left reports 1 rather than 0 - reporting 0 while
    /// the window is still open would contradict the open flag published beside it.
    inline uint32_t secondsLeft(uint32_t nowMs, uint32_t deadlineMs) {
        if (expired(nowMs, deadlineMs)) {
            return 0;
        }
        return (deadlineMs - nowMs + 999u) / 1000u;
    }

}  // namespace SyslogWindowPolicy

#endif  // SYSLOG_WINDOW_POLICY_H
