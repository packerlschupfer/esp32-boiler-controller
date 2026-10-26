// include/modules/control/RoomSensorRegisterReport.h
#ifndef ROOM_SENSOR_REGISTER_REPORT_H
#define ROOM_SENSOR_REGISTER_REPORT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/**
 * @brief The ANDRTF3 open firmware's own counters (input registers 50..77), formatted for MQTT.
 *
 * WHY THIS EXISTS (2026-10-26)
 *
 * The controller's own room-sensor counters on `boiler/status/sensors/room` count what the
 * CONTROLLER saw, and since the open firmware they no longer count what the SENSOR saw. The
 * open firmware samples every 1 s and bridges up to two consecutive bad readings: register 50
 * keeps the last good value with status bit 5 set, and only goes to 0 from the third
 * consecutive failure on. So our `SENSOR_FAULT` count is a count of SUSTAINED failures
 * (>= 3 s), not of raw ones - anything shorter than our 5 s poll is invisible to us by design.
 *
 * Comparing that count against the old stock-firmware baseline therefore OVERSTATES the
 * improvement, because the baseline counted single glitches (the stock firmware wrote a raw 0
 * for one ~2.2 s cycle per bad reading, which is where the old 44 % catch-rate correction came
 * from - it no longer applies). The raw rate only exists in the sensor: register 55.
 *
 * `ANDRTF3::readDiagnostics()` has been in the library the whole time and nothing in this
 * project ever called it. Twenty-eight registers of sensor-side counters, bus error counters,
 * reset cause and running slot, available for one FC04 and never read - the same shape as the
 * sensor offsets and the parameter-save debounce, wired at one end only.
 *
 * The struct is deliberately OURS and not `andrtf3::Diagnostics`: it carries only the fields
 * that get published, the native test env can include this header with no library path, and
 * the mapping between the two is pinned by static_asserts in ANDRTF3TemperatureSensor.cpp,
 * where both headers are in scope (same approach as RoomSensorDiagnostics::KIND_KEYS).
 */
namespace RoomSensorRegisterReport {

    struct Snapshot {
        bool available = false;        ///< a room sensor device exists at all
        bool ok = false;               ///< the FC04 50..77 read succeeded
        bool openFirmware = false;     ///< detectFirmware() found the open firmware
        uint16_t firmwareVersion = 0;  ///< raw input register 2; bit 15 set = open firmware

        uint32_t uptimeSeconds = 0;    ///< 58/59 - the denominator, since 54 wraps
        uint16_t goodReadings = 0;     ///< 54 - WRAPS at 65536 (~18 h at 1 s)
        uint16_t failedReadings = 0;   ///< 55 - the raw failure count
        uint16_t lastFailReason = 0;   ///< 56 - 1 ADC timeout, 2 overflow, 3 range
        uint16_t adcTimeouts = 0;      ///< 60
        uint16_t rangeErrors = 0;      ///< 61
        uint16_t adcOverflows = 0;     ///< 62

        uint16_t secondsSinceGood = 0; ///< 53 - 0xFFFF = never / saturated
        uint16_t status = 0;           ///< 51 - STATUS_* bits
        uint16_t resistance = 0;       ///< 52 - PT1000 in 0.1 Ohm, 0 = not measurable
        int16_t temperatureCenti = 0;  ///< 57 - last good, unfiltered, 0.01 C

        uint16_t busFrames = 0;        ///< 65 - frames seen on the bus, any address
        uint16_t busHandled = 0;       ///< 66 - frames for this sensor with a good CRC
        uint16_t busCrcErrors = 0;     ///< 67 - NOT a line metric, see the note in format()
        uint16_t busBadLength = 0;     ///< 68
        uint16_t busFramingErrors = 0; ///< 69
        uint16_t busParityErrors = 0;  ///< 70
        uint16_t busNoiseErrors = 0;   ///< 71
        uint16_t busOverruns = 0;      ///< 72
        uint16_t busExceptions = 0;    ///< 73 - exception responses the sensor sent

        uint16_t resetCause = 0;       ///< 64 - RCC_CSR 31..24: 0x20 IWDG, 0x10 soft, 0x0C POR
        uint16_t runningSlot = 0;      ///< 74 - 0 = A, 1 = B
        uint16_t bootloaderVersion = 0;///< 77 - 0 = no bootloader
    };

    /**
     * @brief Format a snapshot as a single MQTT payload.
     *
     * Fields are emitted in order of what answers a question, because the payload budget is
     * 320 B and all 24 counters do not fit at once. A fixed prefix (`fw`, `slot`, `up`,
     * `good`, `fail`) must fit or this returns 0; the rest are appended while they fit and
     * `"trunc":1` is added if any were dropped, so the output is always valid JSON and a
     * truncated report says so rather than looking complete.
     *
     * Zero-valued counters are omitted, same convention as RoomSensorDiagnostics::format():
     * on a healthy sensor the interesting payload is short, and an absent key reads as zero.
     * `good` and `fail` are always emitted even at zero - "zero failures" is the answer, not
     * the absence of one.
     *
     * @return bytes written excluding the NUL, or 0 if even the prefix did not fit
     */
    inline size_t format(char* buf, size_t size, const Snapshot& s) {
        if (buf == nullptr || size == 0) {
            return 0;
        }
        buf[0] = '\0';

        // snprintf writes a TRUNCATED string when it does not fit, so every refusal below has
        // to clear the buffer again. Returning 0 while leaving a fragment behind is how a
        // caller ends up publishing half a payload.
        auto refuse = [&]() -> size_t {
            buf[0] = '\0';
            return 0;
        };

        if (!s.available) {
            const int n = snprintf(buf, size, "{\"err\":\"no_device\"}");
            if (n < 0 || static_cast<size_t>(n) >= size) {
                return refuse();
            }
            return static_cast<size_t>(n);
        }
        if (!s.ok) {
            // Distinguish the two reasons a read returns nothing: the original firmware has no
            // such registers (so this is permanent and expected), versus a failed transaction.
            const int n = snprintf(buf, size, "{\"err\":\"%s\",\"fw\":\"0x%04X\"}",
                                   s.openFirmware ? "read_failed" : "not_open_firmware",
                                   s.firmwareVersion);
            if (n < 0 || static_cast<size_t>(n) >= size) {
                return refuse();
            }
            return static_cast<size_t>(n);
        }

        int n = snprintf(buf, size,
                         "{\"fw\":\"0x%04X\",\"slot\":\"%c\",\"up\":%lu,\"good\":%u,\"fail\":%u",
                         s.firmwareVersion,
                         s.runningSlot == 0 ? 'A' : 'B',
                         static_cast<unsigned long>(s.uptimeSeconds),
                         static_cast<unsigned>(s.goodReadings),
                         static_cast<unsigned>(s.failedReadings));
        if (n < 0 || static_cast<size_t>(n) >= size) {
            // The prefix IS the report; without it there is nothing worth publishing.
            return refuse();
        }
        size_t used = static_cast<size_t>(n);
        bool truncated = false;

        // Reserve room for the closing brace and, if needed, the truncation marker.
        constexpr size_t TAIL_RESERVE = sizeof(",\"trunc\":1}");

        auto put = [&](const char* key, unsigned long value, bool skipIfZero) {
            if (skipIfZero && value == 0) {
                return;
            }
            if (truncated) {
                return;  // once something was dropped, keep the order stable and stop
            }
            const int w = snprintf(buf + used, size - used, ",\"%s\":%lu", key, value);
            if (w < 0 || static_cast<size_t>(w) + used + TAIL_RESERVE >= size) {
                buf[used] = '\0';  // undo the partial write
                truncated = true;
                return;
            }
            used += static_cast<size_t>(w);
        };
        auto putHex = [&](const char* key, unsigned value) {
            if (truncated) {
                return;
            }
            const int w = snprintf(buf + used, size - used, ",\"%s\":\"0x%04X\"", key, value);
            if (w < 0 || static_cast<size_t>(w) + used + TAIL_RESERVE >= size) {
                buf[used] = '\0';
                truncated = true;
                return;
            }
            used += static_cast<size_t>(w);
        };

        // The sensor's own failure breakdown - the reason this report exists.
        put("adct", s.adcTimeouts, true);
        put("rng", s.rangeErrors, true);
        put("ovf", s.adcOverflows, true);
        put("lastfail", s.lastFailReason, true);

        // Current state. `age` is reg 53, so 0 means the last good reading is this second.
        put("age", s.secondsSinceGood, true);
        putHex("st", s.status);
        put("res", s.resistance, true);
        put("tc", static_cast<unsigned long>(static_cast<uint16_t>(s.temperatureCenti)), false);

        // Bus health AS THE SENSOR SEES IT, which is the half we have never been able to see.
        // `bcrc` (reg 67) counts a foreign request+reply pair seen as one frame, so it is NOT
        // a line-quality metric and is expected to be non-zero on this shared bus - do not
        // read it as noise. `bexc` is exception responses the sensor itself sent.
        put("bf", s.busFrames, true);
        put("bh", s.busHandled, true);
        put("bcrc", s.busCrcErrors, true);
        put("blen", s.busBadLength, true);
        put("bfrm", s.busFramingErrors, true);
        put("bpar", s.busParityErrors, true);
        put("bnoi", s.busNoiseErrors, true);
        put("bovr", s.busOverruns, true);
        put("bexc", s.busExceptions, true);

        // Why it last restarted, and whether a bootloader is present.
        putHex("rst", s.resetCause);
        put("blv", s.bootloaderVersion, true);

        const int w = snprintf(buf + used, size - used, "%s}", truncated ? ",\"trunc\":1" : "");
        if (w < 0 || static_cast<size_t>(w) >= size - used) {
            return refuse();  // cannot close the object: do not publish half a payload
        }
        return used + static_cast<size_t>(w);
    }

}  // namespace RoomSensorRegisterReport

#endif  // ROOM_SENSOR_REGISTER_REPORT_H
