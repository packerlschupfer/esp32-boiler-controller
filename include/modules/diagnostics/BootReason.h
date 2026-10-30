// include/modules/diagnostics/BootReason.h
#ifndef BOOT_REASON_H
#define BOOT_REASON_H

#include <cstddef>
#include <cstdint>
#include <cstdio>

/**
 * @brief Why did the controller last restart?
 *
 * Added 2026-10-30 because the firmware could not answer that question. The device
 * restarted during a review session - same firmware, no OTA - and there was no way
 * to tell a watchdog from a brownout from a commanded reboot, because
 * esp_reset_reason() was never called anywhere in the tree. A heating controller
 * that reboots silently and unaccountably is a controller you cannot trust a
 * diagnosis about: every "it has been fine since" is unverifiable.
 *
 * Kept free of ESP headers so it can be tested natively. The numeric codes mirror
 * esp_reset_reason_t (esp_system.h) and are pinned by static_assert at the one
 * place that captures them (main.cpp), so a framework renumbering is a build
 * failure rather than a wrong label on a fault report.
 */
namespace BootReason {

// Mirrors esp_reset_reason_t for esp32, ESP-IDF as vendored by
// framework-arduinoespressif32-libs. Verified against the header, not from memory.
enum Code : uint8_t {
    UNKNOWN     = 0,   // reason could not be determined
    POWERON     = 1,   // power-on event
    EXT         = 2,   // external pin (not applicable on ESP32)
    SW          = 3,   // esp_restart() - ours: OTA, cmd/diagnostics reboot, NVS erase
    PANIC       = 4,   // exception / panic
    INT_WDT     = 5,   // interrupt watchdog
    TASK_WDT    = 6,   // task watchdog
    WDT         = 7,   // other watchdog
    DEEPSLEEP   = 8,   // woke from deep sleep (unused here)
    BROWNOUT    = 9,   // supply dipped
    SDIO        = 10,
    USB         = 11,
    JTAG        = 12,
    EFUSE       = 13,
    PWR_GLITCH  = 14,
    CPU_LOCKUP  = 15,  // double exception
};

/// Short stable label. Published, logged and greppable - do not reword casually.
inline const char* name(uint8_t code) {
    switch (code) {
        case UNKNOWN:    return "unknown";
        case POWERON:    return "poweron";
        case EXT:        return "ext_pin";
        case SW:         return "sw_restart";
        case PANIC:      return "panic";
        case INT_WDT:    return "int_wdt";
        case TASK_WDT:   return "task_wdt";
        case WDT:        return "wdt";
        case DEEPSLEEP:  return "deepsleep";
        case BROWNOUT:   return "brownout";
        case SDIO:       return "sdio";
        case USB:        return "usb";
        case JTAG:       return "jtag";
        case EFUSE:      return "efuse";
        case PWR_GLITCH: return "pwr_glitch";
        case CPU_LOCKUP: return "cpu_lockup";
        default:         return "invalid";
    }
}

/**
 * @brief Did this restart indicate a fault, as opposed to someone asking for it?
 *
 * SW is NOT abnormal: every OTA, every cmd/diagnostics reboot and the NVS erase
 * path go through esp_restart(), and flagging those would make the signal useless
 * in exactly the week you do most reboots. POWERON is not abnormal either - a
 * power cut is a fact about the house, not a defect in the firmware.
 *
 * UNKNOWN IS treated as abnormal on purpose. An unexplained restart is the case
 * this whole facility exists for; defaulting it to "fine" would reproduce the
 * silence it was added to break.
 */
inline bool isAbnormal(uint8_t code) {
    switch (code) {
        case PANIC:
        case INT_WDT:
        case TASK_WDT:
        case WDT:
        case BROWNOUT:
        case EFUSE:
        case PWR_GLITCH:
        case CPU_LOCKUP:
        case UNKNOWN:
            return true;
        default:
            return false;
    }
}

/**
 * @brief Build the retained boiler/status/device/boot payload.
 *
 * Fixed-width by construction: the longest label is 10 chars and the counter is
 * bounded by uint32_t, so the worst case is well under 96 B and cannot be the
 * cause of a dropped publish (the health topic's 256 B cliff is a live concern -
 * see MQTTPublisher.cpp - so new payloads state their bound).
 *
 * @return chars written excluding the NUL, or 0 if the buffer was too small (in
 *         which case buf is left empty rather than truncated, because snprintf
 *         leaves a valid-looking partial string on overflow).
 */
inline size_t format(char* buf, size_t len, uint8_t code, uint32_t bootCount) {
    if (!buf || len == 0) return 0;
    const int n = snprintf(buf, len, "{\"reason\":\"%s\",\"code\":%u,\"boots\":%lu,\"abnormal\":%s}",
                           name(code), static_cast<unsigned>(code),
                           static_cast<unsigned long>(bootCount),
                           isAbnormal(code) ? "true" : "false");
    if (n < 0 || static_cast<size_t>(n) >= len) {
        buf[0] = '\0';
        return 0;
    }
    return static_cast<size_t>(n);
}

}  // namespace BootReason

/**
 * Captured ONCE in setup() before any task starts, then read-only - so no mutex.
 * Defined in main.cpp. Declared outside the namespace to match the project's other
 * boot-time globals (g_roomTargetIsTemporary).
 */
extern uint8_t  g_bootReasonCode;
extern uint32_t g_bootCount;

#endif  // BOOT_REASON_H
