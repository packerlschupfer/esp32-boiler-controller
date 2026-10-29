// include/modules/control/EmergencyStopRelease.h
#ifndef EMERGENCY_STOP_RELEASE_H
#define EMERGENCY_STOP_RELEASE_H

#include <cstdint>

/**
 * @brief Manual release of a latched emergency stop (header-only, native-testable).
 *
 * CentralizedFailsafe::emergencyStop() sets EMERGENCY_STOP and clears BOILER_ENABLED
 * (critical temperature, stale sensor data during operation, burner request watchdog).
 * Nothing released it except TemperatureSensorFallback on sensor recovery, so the
 * burner stayed blocked and the pumps ran until a reboot. The MQTT command
 * boiler/cmd/emergency_reset releases it, but only once the causes have cleared.
 */
namespace EmergencyStopRelease {

    enum class Result : uint8_t {
        RELEASED,
        NOT_ACTIVE,
        BOILER_RE_ENABLED,      // latch was already clear, but BOILER_ENABLED was not restored
        TEMPERATURE_HIGH,       // boiler output or return at or above the operating limit
        SENSORS_UNAVAILABLE,    // required sensors missing, stale or failing
        SYSTEM_ERRORS           // critical error bits (sensor, Modbus, relay) still set
    };

    struct Conditions {
        bool emergencyActive;
        bool temperaturesOk;
        bool sensorsOk;
        bool systemErrorsClear;
    };

    /**
     * @brief The safety conditions alone, ignoring whether the latch is set.
     *
     * Split out 2026-10-25 so re-enabling the boiler can be gated on exactly the same
     * conditions as releasing the latch. releaseAfterSensorRecovery() clears EMERGENCY_STOP
     * but deliberately leaves the boiler disabled for a human to restart; the operator's
     * emergency_reset then has to do that restart, and it must not do it over a hot boiler or
     * a failed sensor just because the latch happens to be clear.
     */
    inline Result evaluateSafety(const Conditions& c) {
        if (!c.temperaturesOk) {
            return Result::TEMPERATURE_HIGH;
        }
        if (!c.sensorsOk) {
            return Result::SENSORS_UNAVAILABLE;
        }
        if (!c.systemErrorsClear) {
            return Result::SYSTEM_ERRORS;
        }
        return Result::RELEASED;
    }

    inline Result evaluate(const Conditions& c) {
        if (!c.emergencyActive) {
            return Result::NOT_ACTIVE;
        }
        return evaluateSafety(c);
    }

    inline const char* toString(Result r) {
        switch (r) {
            case Result::RELEASED:            return "emergency_released";
            case Result::NOT_ACTIVE:          return "emergency_not_active";
            case Result::BOILER_RE_ENABLED:   return "boiler_re_enabled";
            case Result::TEMPERATURE_HIGH:    return "emergency_release_refused:temperature_high";
            case Result::SENSORS_UNAVAILABLE: return "emergency_release_refused:sensors_unavailable";
            case Result::SYSTEM_ERRORS:       return "emergency_release_refused:system_errors";
        }
        return "emergency_release_refused:unknown";
    }

    // What latched the emergency stop. Sensor recovery (TemperatureSensorFallback
    // SHUTDOWN -> NORMAL) may only release a stop caused by stale sensor data.
    enum class Cause : uint8_t {
        NONE,
        SENSOR_STALE,
        OTHER               // critical temperature, request watchdog, failsafe level
    };

    // A second trigger while latched with a different cause makes the latch OTHER
    inline Cause mergeCause(bool alreadyLatched, Cause latched, Cause incoming) {
        if (!alreadyLatched || latched == Cause::NONE) {
            return incoming;
        }
        return latched == incoming ? latched : Cause::OTHER;
    }

    inline bool releasableBySensorRecovery(bool emergencyActive, Cause cause) {
        return emergencyActive && cause == Cause::SENSOR_STALE;
    }

    /**
     * EMERGENCY_STOP is a level latch: BurnerControlTask reads it without clearing and
     * stops the burner once per onset (clearing it there ended the pump heat dissipation
     * within seconds and left emergency_reset nothing to release).
     */
    inline bool onsetDetected(bool emergencySet, bool& wasSet) {
        const bool onset = emergencySet && !wasSet;
        wasSet = emergencySet;
        return onset;
    }

    // Heat dissipation while latched: both pumps run until the boiler output has cooled
    constexpr int16_t DISSIPATION_END_TENTHS = 600;      // 60.0 °C - pumps may stop below this
    constexpr int16_t DISSIPATION_RESTART_TENTHS = 650;  // 65.0 °C - pumps on again (hysteresis)

    /**
     * @brief Blind dissipation is BOUNDED, like the after-release rule below.
     *
     * This used to `return true` unconditionally when the reading was unusable - no cap, no
     * time input - while dissipationAfterRelease() capped the identical case at
     * maxWithoutReadingMs. The asymmetry had no recorded rationale and it is the latched side
     * that can last forever, because nothing periodic clears a latch.
     *
     * What it cost: if the MB8ART dies permanently while the burner is firing,
     * continuousSafetyMonitor latches a SENSOR_STALE stop at 15 s and both pumps are forced on
     * and re-asserted every cycle. emergency_reset cannot release it either - the idle fallback
     * refresh drives the sensor fallback to SHUTDOWN and sets Error::SENSOR_FAILURE, so the
     * release refuses with system_errors until reads resume. So the only exits were the module
     * returning or a reboot. Meanwhile the water pump circulates the DHW tank through a boiler
     * that cooled to cellar temperature within half an hour, and the heating pump moves that
     * heat into the radiators: the tank drains over hours, and the pumps run for days if the
     * owner is away. Found by the 2026-10-28 review.
     *
     * Bounded is right because WITHOUT A READING WE CANNOT KNOW WE ARE DISSIPATING ANYTHING.
     * The cast-iron block's after-heat moves into the water within minutes; past that the pumps
     * are not removing heat, they are redistributing cold. The cap is generous rather than
     * tight - the point is that it terminates, not that it is exact.
     */
    constexpr uint32_t MAX_BLIND_DISSIPATION_MS = 900000;  // 15 min

    inline bool dissipationPumpOn(bool outputUsable, int16_t outputTenths, bool wasOn,
                                  uint32_t sinceLatchMs,
                                  uint32_t maxWithoutReadingMs = MAX_BLIND_DISSIPATION_MS) {
        if (!outputUsable) {
            // Blind: circulate, but not forever.
            return sinceLatchMs < maxWithoutReadingMs;
        }
        return wasOn ? outputTenths >= DISSIPATION_END_TENTHS
                     : outputTenths >= DISSIPATION_RESTART_TENTHS;
    }

    /**
     * After a release (emergency_reset below 110 °C, or sensor recovery) the pumps keep
     * dissipating until the boiler output is below 60.0 °C; without a usable reading for
     * at most maxWithoutReadingMs. Before, both pumps stopped at once, even at 100 °C
     * (review 2026-09-14).
     */
    inline bool dissipationAfterRelease(bool outputUsable, int16_t outputTenths,
                                        uint32_t sinceReleaseMs, uint32_t maxWithoutReadingMs) {
        if (!outputUsable) {
            return sinceReleaseMs < maxWithoutReadingMs;
        }
        return outputTenths >= DISSIPATION_END_TENTHS;
    }

} // namespace EmergencyStopRelease

#endif // EMERGENCY_STOP_RELEASE_H
