// include/modules/control/FrostProtectionPolicy.h
#ifndef FROST_PROTECTION_POLICY_H
#define FROST_PROTECTION_POLICY_H

#include "shared/Temperature.h"

/**
 * @brief Should the burner fire to keep the water from freezing?
 *
 * Header-only and natively testable, like BurnerHysteresisPolicy and SpaceHeatingPolicy.
 *
 * WHY THIS EXISTS (2026-10-23)
 *
 * It did not. `SystemConstants::Temperature::FREEZE_PROTECTION_TEMP_C` (5.0 °C, commented
 * "Anti-freeze activation") was referenced nowhere, and `wheater/tempSafeLimitLow`
 * ("Safety limit to prevent freezing of the boiler") was registered, range-checked,
 * persisted and published while being read by nothing - it was removed on 2026-10-23 rather
 * than left as a promise the firmware did not keep. Nothing in the code reacted to a LOW
 * temperature at all: SafetyInterlocks bounds pressure from below but not temperature, and
 * MIN_BOILER_TEMP_C is a floor on requested setpoints, not a trigger.
 *
 * What kept the water warm was a side effect of normal operation: the tank is recharged
 * whenever it falls below `wheater/tempLimitLow` - **40.0 on this installation**, not the
 * 45.0 the struct comment uses as its example, so 35 K of margin above freezing - and the
 * house is held at its room target. Both stop the moment heating and water are disabled: a
 * summer setting left on into a cold snap, or a long absence.
 *
 * WHAT THIS DOES NOT COVER, and cannot:
 *
 *  - **A dead controller.** CH1 BURNER_ENABLE is a normally-OPEN relay contact, so with the
 *    ESP32 unpowered, crashed or in emergency stop the burner circuit is open and the boiler
 *    cannot fire at all. The original Vaillant controller is not in the loop, and the STB and
 *    tandem thermostat can only ever STOP the burner, never start it. Only a hardware frost
 *    thermostat wired around the relay would close that gap.
 *  - **A dead room sensor.** In SPACE_HEATING, TemperatureSensorFallback::hasRequiredSensors()
 *    requires roomTempValid, so a room-sensor failure stops the burner however cold the water
 *    is. This guard deliberately does not bypass the sensor fallback: firing autonomously on
 *    an unvalidated sensor set is a worse failure than the one it would prevent.
 *  - **STALE readings, as of 2026-10-25.** A sensor that has stopped answering is treated
 *    exactly like an invalid one: it never triggers a burn. That is the user's call and it
 *    matches the asymmetry below - an unreadable sensor is not evidence of frost. The cost is
 *    that a frozen house behind a dead bus gets no protection from here either; what it buys
 *    is that the firmware never fires the burner on data it cannot vouch for.
 *
 * So this covers exactly one scenario - the system is healthy and simply is not heating -
 * which is also the likely one.
 *
 * WHY IT DOES NOT OVERRIDE "BOILER DISABLED". The caller lets frost demand override
 * HEATING_ENABLED (that is the point: heating off is when freezing happens) but NOT
 * BOILER_ENABLED. A drained-for-service system is a likely reason to switch the boiler off,
 * and firing a burner into a drained boiler is worse than frost. The pressure interlock
 * would probably catch it, but "probably" plus a never-calibrated pressure channel
 * (docs: ~0.2-0.3 BAR still unexplained) is not a safety argument.
 */
namespace FrostProtectionPolicy {

    /// Fire below this. 5.0 °C matches SystemConstants::Temperature::FREEZE_PROTECTION_TEMP_C,
    /// which existed as a constant with no logic behind it; a static_assert in
    /// HeatingControlTask.cpp keeps the two tied together.
    constexpr Temperature_t TRIGGER_TENTHS = 50;   // 5.0 °C

    /// Stop once everything readable is back above this. 5 K of hysteresis, so a thermometer
    /// sitting on the trigger cannot chatter the burner; at the stage-1 output this is a
    /// burn of a few minutes, not seconds.
    constexpr Temperature_t RELEASE_TENTHS = 100;  // 10.0 °C

    static_assert(RELEASE_TENTHS > TRIGGER_TENTHS, "frost release must be above the trigger");

    /**
     * @brief The three readings, each with a USABLE flag.
     *
     * "Usable" means valid AND FRESH, and the caller is responsible for folding staleness in -
     * see evaluateFrostDemand(). That matters because nothing clears
     * SharedSensorReadings::isBoilerTempOutputValid when reads simply STOP: a dead MB8ART
     * leaves the last value flagged valid forever. Before 2026-10-25 this guard read that flag
     * raw, so a bus failure froze the flow reading at whatever it last was - typically 30-40 °C
     * after a burn - and frost protection could never fire again however cold the house got,
     * with boiler/status/sensors still reporting sf:1. Measured during the first bus-release
     * window; see [[sensor-fallback-blind-when-idle]].
     *
     * Per-source timestamps, not one shared one: flow and tank come from the MB8ART
     * (lastBoilerTempUpdateTimestamp), room from the ANDRTF3 (insideTempGoodMs). Using the
     * shared lastUpdateTimestamp for the room would let a live MB8ART mask a dead room sensor,
     * which is the same mistake F5 fixed on the boiler side.
     */
    struct Readings {
        Temperature_t flow = 0;   // boiler output, MB8ART
        bool flowUsable = false;
        Temperature_t tank = 0;   // DHW tank, MB8ART
        bool tankUsable = false;
        Temperature_t room = 0;   // inside, ANDRTF3
        bool roomUsable = false;
    };

    /// Which readings are cold, as a bitmask - three sensors would need seven enum values.
    constexpr uint8_t COLD_FLOW = 1u << 0;
    constexpr uint8_t COLD_TANK = 1u << 1;
    constexpr uint8_t COLD_ROOM = 1u << 2;

    inline uint8_t coldMask(const Readings& r) {
        uint8_t m = 0;
        if (r.flowUsable && r.flow <= TRIGGER_TENTHS) m |= COLD_FLOW;
        if (r.tankUsable && r.tank <= TRIGGER_TENTHS) m |= COLD_TANK;
        if (r.roomUsable && r.room <= TRIGGER_TENTHS) m |= COLD_ROOM;
        return m;
    }

    /// Fixed strings for the log line - every combination, so no buffer is needed.
    inline const char* describe(uint8_t mask) {
        switch (mask & (COLD_FLOW | COLD_TANK | COLD_ROOM)) {
            case 0:                                    return "none";
            case COLD_FLOW:                            return "flow";
            case COLD_TANK:                            return "tank";
            case COLD_ROOM:                            return "room";
            case COLD_FLOW | COLD_TANK:                return "flow+tank";
            case COLD_FLOW | COLD_ROOM:                return "flow+room";
            case COLD_TANK | COLD_ROOM:                return "tank+room";
            default:                                   return "flow+tank+room";
        }
    }

    /**
     * @brief Latching demand: trigger on a valid cold reading, release only when clear.
     *
     * @param currentlyDemanding the caller's latch from the previous evaluation
     *
     * Asymmetric on purpose, in two different ways:
     *
     *  - TRIGGER needs a VALID reading below TRIGGER_TENTHS. An invalid reading never starts
     *    a burn; an unreadable sensor is not evidence of frost, and TEMP_INVALID is
     *    INT16_MIN, which would otherwise read as -3276.8 °C and fire instantly.
     *  - RELEASE treats an invalid reading as "not blocking". So if every reading goes
     *    invalid mid-burn the demand DROPS rather than continuing to fire blind - stopping is
     *    the safe direction when the controller can no longer see what it is heating, and the
     *    sensor-failure machinery is better placed to react than this guard is.
     */
    /// One latch, over a chosen subset of the sensors. Trigger needs a usable cold reading;
    /// release treats an unusable reading as "not blocking", so a burn stops rather than
    /// continuing blind.
    inline bool latch(bool currentlyDemanding, const Readings& r, uint8_t watch) {
        if (!currentlyDemanding) {
            return (coldMask(r) & watch) != 0;
        }
        bool clear = true;
        if (watch & COLD_FLOW) clear = clear && (!r.flowUsable || r.flow >= RELEASE_TENTHS);
        if (watch & COLD_TANK) clear = clear && (!r.tankUsable || r.tank >= RELEASE_TENTHS);
        if (watch & COLD_ROOM) clear = clear && (!r.roomUsable || r.room >= RELEASE_TENTHS);
        return !clear;
    }

    /**
     * @brief Space-heating frost demand: the boiler's own water, or the house.
     *
     * ROOM was added 2026-10-25 (user decision). The house freezing is the thing this guard is
     * actually for - "in case someone disabled something or set some values too low" - and the
     * flow reading only sees it indirectly, after the boiler itself has gone cold.
     */
    inline bool heatDemand(bool currentlyDemanding, const Readings& r) {
        return latch(currentlyDemanding, r, COLD_FLOW | COLD_ROOM);
    }

    /**
     * @brief DHW frost demand: the tank, which the heating circuit cannot reach.
     *
     * Split from the heating latch 2026-10-25. The tank used to trigger heating demand, which
     * warms the house and does nothing whatsoever for the tank - so the tank had no frost
     * protection at all, only the appearance of it. A cold tank needs a water CHARGE: the
     * water-mode relay, the valve and the water pump.
     *
     * This essentially never fires. Normal operation recharges the tank below
     * wheater/tempLimitLow, 40.0 on this installation, so 35 K above freezing; reaching 5 °C
     * means water heating was off for a very long time in a very cold house. A spurious
     * trigger is also cheap: the 10 °C release stops the charge almost at once, and the normal
     * DHW limits still bound it.
     */
    inline bool waterDemand(bool currentlyDemanding, const Readings& r) {
        return latch(currentlyDemanding, r, COLD_TANK);
    }

} // namespace FrostProtectionPolicy

#endif // FROST_PROTECTION_POLICY_H
