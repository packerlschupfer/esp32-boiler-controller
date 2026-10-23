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

    struct Readings {
        Temperature_t flow = 0;   // boiler output
        bool flowValid = false;
        Temperature_t tank = 0;   // DHW tank
        bool tankValid = false;
    };

    /// Which reading pulled the trigger, for the log line. Only meaningful while demanding.
    enum class Source : uint8_t { NONE, FLOW, TANK, BOTH };

    inline Source coldSource(const Readings& r) {
        const bool flowCold = r.flowValid && r.flow <= TRIGGER_TENTHS;
        const bool tankCold = r.tankValid && r.tank <= TRIGGER_TENTHS;
        if (flowCold && tankCold) return Source::BOTH;
        if (flowCold) return Source::FLOW;
        if (tankCold) return Source::TANK;
        return Source::NONE;
    }

    inline const char* toString(Source s) {
        switch (s) {
            case Source::FLOW: return "flow";
            case Source::TANK: return "tank";
            case Source::BOTH: return "flow+tank";
            case Source::NONE: return "none";
        }
        return "unknown";
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
    inline bool demand(bool currentlyDemanding, const Readings& r) {
        if (!currentlyDemanding) {
            return coldSource(r) != Source::NONE;
        }
        const bool flowClear = !r.flowValid || r.flow >= RELEASE_TENTHS;
        const bool tankClear = !r.tankValid || r.tank >= RELEASE_TENTHS;
        return !(flowClear && tankClear);
    }

} // namespace FrostProtectionPolicy

#endif // FROST_PROTECTION_POLICY_H
