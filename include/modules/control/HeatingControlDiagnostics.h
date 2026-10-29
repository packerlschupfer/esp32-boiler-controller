// include/modules/control/HeatingControlDiagnostics.h
#ifndef HEATING_CONTROL_DIAGNOSTICS_H
#define HEATING_CONTROL_DIAGNOSTICS_H

#include <cstdint>
#include <cstdio>

#include "modules/control/RoomTempFallbackPolicy.h"  // Source, for the `isrc` field

/**
 * @brief Payload for boiler/status/heating/control (header-only, native-testable).
 *
 * Answers "why is the heating off, and what is the curve actually asking for" from the
 * firmware's own state, so Home Assistant does not have to infer it from templates. Four
 * things were unobservable before this: the PRE-CLAMP curve output, the room-deviation
 * contribution on its own, which control mode is in force, and whether demand is blocked by
 * the outside-threshold gate or by the room backstop.
 *
 * All temperatures are TENTHS of degC, signed, matching t.* on boiler/status/sensors.
 * Built with snprintf into a caller-supplied buffer: this publishes every 10 s and the
 * project avoids heap on runtime paths.
 */
namespace HeatingControlDiagnostics {

    /// Why space-heating demand is currently off (or that it is on).
    ///
    /// Enumerator names are deliberately NOT the obvious words: Arduino's
    /// esp32-hal-gpio.h does `#define DISABLED 0x00` (and RISING/FALLING/CHANGE/ANALOG),
    /// so a `DISABLED` enumerator expands to a numeric constant and the compiler reports
    /// the error inside the framework header, which is a confusing place to debug it.
    /// The JSON strings in toString() are unchanged - the wire format is the contract.
    enum class Block : uint8_t {
        OK,              // heating is wanted
        // Nothing has been evaluated yet - the value this starts at, for the ~4 s between boot
        // and the first checkIfSpaceHeatingNeededEvent(). It needs to exist because the two
        // obvious stand-ins are both lies: OK means "heating is wanted" (c1ed1b0 fixed exactly
        // that - HA rendered "Heating" with dem:false for 4 s at every boot), and SENSOR_INVALID
        // means a reading is broken, which wrote a "Sensor fault" event into Home Assistant's
        // history on EVERY reboot. That is worse than untidy: it trains an operator to discount
        // a genuinely alarming string, and it makes a real sensor fault indistinguishable from
        // a restart when reading history back. Added 2026-10-29.
        STARTUP,
        OUTSIDE_GATE,    // outside >= outsideThreshold + OUTSIDE_THRESHOLD_HYSTERESIS (weather mode)
        ROOM_BACKSTOP,   // room > targetTemp + roomOverheatMargin (weather mode)
        ROOM_SATISFIED,  // room thermostat satisfied (room mode)
        SENSOR_INVALID,  // outside or room reading invalid - weather mode fails safe
        SUMMER_OVERRIDE, // heatingOverrideOff (summer mode)
        HEATING_DISABLED,// heatingEnabled false
        // Heating is running ONLY because frost protection demanded it - nothing else wanted
        // heat. Added 2026-10-26: a frost burn published dem:true with blk:"none", which reads
        // as ordinary demand, and `cmd` showed the curve output while the burner was actually
        // commanded to the flow FLOOR. An operator seeing a burn with the heating switch off
        // had nothing in this payload to explain it.
        FROST
    };

    /// Which clamp bound decided the commanded flow target.
    enum class Clamp : uint8_t {
        NO_CLAMP,  // the curve result was used as-is
        AT_FLOOR,  // h/sLo won - weather compensation is currently inert
        AT_CEIL    // h/sHi won
    };

    struct State {
        bool weatherMode = false;
        bool demand = false;
        Block block = Block::OK;
        Clamp clamp = Clamp::NO_CLAMP;
        int32_t rawTarget = 0;   // pre-clamp curve output, tenths (can be negative)
        int32_t shift = 0;       // curveShift*10 + room-deviation term, tenths
        int32_t commanded = 0;   // post-clamp flow target, tenths
        int16_t outside = 0;     // the values THIS evaluation used, so one payload is
        int16_t room = 0;        // internally consistent rather than needing to be paired
        int16_t roomTarget = 0;  // with separately published sensors
        // Where `room` came from. The curve substitutes a held, target or nominal value
        // when the sensor is invalid (RoomTempFallbackPolicy), and a substituted value is
        // otherwise indistinguishable from a measured one in `room` - which would make this
        // topic quietly lie about its own inputs. The raw sensor field stays on
        // boiler/status/sensors as t.i.
        RoomTempFallbackPolicy::Source roomSource = RoomTempFallbackPolicy::Source::MEASURED;
    };

    inline const char* toString(Block b) {
        switch (b) {
            case Block::OK:               return "none";
            case Block::STARTUP:          return "startup";
            case Block::OUTSIDE_GATE:     return "outside";
            case Block::ROOM_BACKSTOP:    return "backstop";
            case Block::ROOM_SATISFIED:   return "roomsat";
            case Block::SENSOR_INVALID:   return "sensor";
            case Block::SUMMER_OVERRIDE:  return "override";
            case Block::HEATING_DISABLED: return "disabled";
            case Block::FROST:            return "frost";
        }
        return "unknown";
    }

    inline const char* toString(Clamp c) {
        switch (c) {
            case Clamp::NO_CLAMP: return "none";
            case Clamp::AT_FLOOR: return "floor";
            case Clamp::AT_CEIL:  return "ceil";
        }
        return "unknown";
    }

    /**
     * @brief Serialise to JSON.
     * @return bytes written, or 0 if the buffer was too small (caller must not publish).
     *
     * Worst case measured at 133 B against MQTTTask's 319 B payload limit, which since
     * 2026-10-18 REFUSES an oversized payload rather than truncating it - so a silent cut
     * here would become a dropped publish, not malformed JSON. The 0 return is still checked
     * by the caller rather than trusted.
     */
    inline size_t format(char* buf, size_t size, const State& s) {
        if (buf == nullptr || size == 0) {
            return 0;
        }
        const int n = snprintf(buf, size,
            "{\"mode\":\"%s\",\"dem\":%s,\"blk\":\"%s\",\"raw\":%ld,\"shift\":%ld,"
            "\"cmd\":%ld,\"clp\":\"%s\",\"o\":%d,\"i\":%d,\"isrc\":\"%s\",\"tgt\":%d}",
            s.weatherMode ? "weather" : "room",
            s.demand ? "true" : "false",
            toString(s.block),
            static_cast<long>(s.rawTarget),
            static_cast<long>(s.shift),
            static_cast<long>(s.commanded),
            toString(s.clamp),
            static_cast<int>(s.outside),
            static_cast<int>(s.room),
            RoomTempFallbackPolicy::toString(s.roomSource),
            static_cast<int>(s.roomTarget));
        if (n < 0 || static_cast<size_t>(n) >= size) {
            return 0;  // truncated - refuse rather than publish a fragment
        }
        return static_cast<size_t>(n);
    }

    /// True when anything a subscriber cares about changed (avoids republishing every cycle).
    inline bool differs(const State& a, const State& b) {
        return a.weatherMode != b.weatherMode || a.demand != b.demand ||
               a.block != b.block || a.clamp != b.clamp ||
               a.rawTarget != b.rawTarget || a.shift != b.shift ||
               a.commanded != b.commanded || a.outside != b.outside ||
               a.room != b.room || a.roomTarget != b.roomTarget ||
               a.roomSource != b.roomSource;
    }

} // namespace HeatingControlDiagnostics

#endif // HEATING_CONTROL_DIAGNOSTICS_H
