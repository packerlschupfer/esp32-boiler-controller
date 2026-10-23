// include/modules/control/RoomSensorDiagnostics.h
#ifndef ROOM_SENSOR_DIAGNOSTICS_H
#define ROOM_SENSOR_DIAGNOSTICS_H

#include <cstdint>
#include <cstddef>
#include <cstdio>

/**
 * @brief Payload for boiler/status/sensors/room (header-only, native-testable).
 *
 * WHY THIS EXISTS (2026-10-24)
 *
 * On 2026-10-23 08:34 the room sensor failed three consecutive reads, which marked the
 * reading invalid, which escalated TemperatureSensorFallback to SHUTDOWN, which stopped the
 * burner mid-cycle and cost one ignition. **Why** it failed is permanently unknown: the HAL
 * logs the reason with LOG_WARN, syslog is deliberately off on this device (it cost ~11 kB
 * of DRAM) and no serial capture was running, so the string went to a UART nobody was
 * listening to.
 *
 * The ESP32-ANDRTF3 library gained per-kind counters for exactly this reason (requested
 * 2026-10-24, shipped in b4a661d). Publishing them puts the evidence in Home Assistant's
 * recorder, where a dropout can be explained after the fact instead of being guessed at.
 * The maintainer's hypothesis for 08:34 is BUS_BUSY - pure RS485 contention, nothing even
 * sent - and `busy` is the field that will confirm or kill it.
 *
 * Shape: successes are always published (they are the denominator for a failure rate); error
 * kinds appear only when non-zero, so the steady-state payload is about 30 bytes and a
 * subscriber can tell "no failures yet" from "no such counter".
 */
namespace RoomSensorDiagnostics {

    /// Mirrors andrtf3::ANDRTF3::ReadError's order. Index 0 is NONE = successful reads.
    /// Kept as plain strings rather than including the library header, so this stays
    /// native-testable; a static_assert in ANDRTF3Task.cpp pins the count to the enum.
    constexpr const char* KIND_KEYS[] = {
        "ok",    // NONE - successful reads
        "busy",  // BUS_BUSY - bus mutex not acquired, nothing sent: pure contention
        "to",    // TIMEOUT - request sent, no answer
        "crc",   // CRC - corrupted answer
        "exc",   // DEVICE_EXCEPTION
        "bad",   // BAD_RESPONSE
        "flt",   // SENSOR_FAULT - answered 0x0000 / 0xFFFF
        "rng",   // OUT_OF_RANGE
        "oth"    // OTHER
    };
    constexpr size_t KIND_KEY_COUNT = sizeof(KIND_KEYS) / sizeof(KIND_KEYS[0]);

    struct State {
        uint32_t counts[KIND_KEY_COUNT] = {0};
        uint32_t lastGoodAgeMs = UINT32_MAX;
        uint8_t lastKind = 0;
        bool available = false;
    };

    /**
     * @brief Serialise to JSON.
     * @return bytes written, or 0 if the buffer was too small (caller must not publish).
     *
     * Worst case with every kind non-zero and ten-digit counts is about 130 B, against
     * MQTTTask's 319 B usable payload - which REFUSES an oversized payload rather than
     * truncating, so a silent cut would become a dropped publish.
     *
     * `age` is SECONDS since the last good read, omitted entirely when there has never been
     * one. Seconds rather than milliseconds because this is for a human reading history, and
     * because it keeps the field two or three digits for any age that matters.
     */
    inline size_t format(char* buf, size_t size, const State& s) {
        if (buf == nullptr || size == 0) {
            return 0;
        }
        size_t used = 0;
        int n = snprintf(buf, size, "{\"ok\":%lu", static_cast<unsigned long>(s.counts[0]));
        if (n < 0 || static_cast<size_t>(n) >= size) {
            return 0;
        }
        used = static_cast<size_t>(n);

        // Error kinds only when non-zero: "no failures" is then the absence of the key, and
        // the steady-state payload stays tiny.
        for (size_t i = 1; i < KIND_KEY_COUNT; ++i) {
            if (s.counts[i] == 0) {
                continue;
            }
            n = snprintf(buf + used, size - used, ",\"%s\":%lu", KIND_KEYS[i],
                         static_cast<unsigned long>(s.counts[i]));
            if (n < 0 || static_cast<size_t>(n) >= size - used) {
                return 0;
            }
            used += static_cast<size_t>(n);
        }

        if (s.lastGoodAgeMs != UINT32_MAX) {
            n = snprintf(buf + used, size - used, ",\"age\":%lu",
                         static_cast<unsigned long>(s.lastGoodAgeMs / 1000));
            if (n < 0 || static_cast<size_t>(n) >= size - used) {
                return 0;
            }
            used += static_cast<size_t>(n);
        }

        n = snprintf(buf + used, size - used, ",\"last\":\"%s\"}",
                     s.lastKind < KIND_KEY_COUNT ? KIND_KEYS[s.lastKind] : "oth");
        if (n < 0 || static_cast<size_t>(n) >= size - used) {
            return 0;
        }
        return used + static_cast<size_t>(n);
    }

    /**
     * @brief True when something a subscriber cares about changed.
     *
     * Deliberately ignores `lastGoodAgeMs`: it changes on every evaluation, so including it
     * would publish every cycle and defeat the point of change detection. The age still goes
     * out - on the caller's heartbeat, or alongside a counter that did change.
     */
    inline bool differs(const State& a, const State& b) {
        if (a.available != b.available || a.lastKind != b.lastKind) {
            return true;
        }
        for (size_t i = 0; i < KIND_KEY_COUNT; ++i) {
            if (a.counts[i] != b.counts[i]) {
                return true;
            }
        }
        return false;
    }

} // namespace RoomSensorDiagnostics

#endif // ROOM_SENSOR_DIAGNOSTICS_H
