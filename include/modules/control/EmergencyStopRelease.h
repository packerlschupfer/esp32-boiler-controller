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

    inline Result evaluate(const Conditions& c) {
        if (!c.emergencyActive) {
            return Result::NOT_ACTIVE;
        }
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

    inline const char* toString(Result r) {
        switch (r) {
            case Result::RELEASED:            return "emergency_released";
            case Result::NOT_ACTIVE:          return "emergency_not_active";
            case Result::TEMPERATURE_HIGH:    return "emergency_release_refused:temperature_high";
            case Result::SENSORS_UNAVAILABLE: return "emergency_release_refused:sensors_unavailable";
            case Result::SYSTEM_ERRORS:       return "emergency_release_refused:system_errors";
        }
        return "emergency_release_refused:unknown";
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

    inline bool dissipationPumpOn(bool outputUsable, int16_t outputTenths, bool wasOn) {
        if (!outputUsable) {
            return true;  // invalid or stale boiler output: keep circulating
        }
        return wasOn ? outputTenths >= DISSIPATION_END_TENTHS
                     : outputTenths >= DISSIPATION_RESTART_TENTHS;
    }

} // namespace EmergencyStopRelease

#endif // EMERGENCY_STOP_RELEASE_H
