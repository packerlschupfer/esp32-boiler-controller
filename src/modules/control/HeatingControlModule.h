// src/modules/control/HeatingControlModule.h
#ifndef HEATING_CONTROL_MODULE_H
#define HEATING_CONTROL_MODULE_H

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include "shared/SharedSensorReadings.h"
#include "shared/Temperature.h"
#include "config/SystemSettings.h"
#include "core/SystemResourceProvider.h"
#include "modules/control/RoomTempFallbackPolicy.h"

// Note: Access these through SystemResourceProvider (SRP):
// - SRP::getSystemSettings() for SystemSettings

/**
 * @brief Heating control module for space heating
 *
 * This module handles heating curve calculations and heating on/off control.
 * Power level control (OFF/HALF/FULL) is handled by BoilerTempControlTask.
 *
 * The heating curve calculates target boiler temperature based on:
 * - Inside (room) temperature
 * - Outside temperature
 * - Curve coefficient and shift parameters
 */
class HeatingControlModule {
public:
    // Constructor
    HeatingControlModule(EventGroupHandle_t systemEventGroup, SemaphoreHandle_t sensorMutex);

    // Destructor
    ~HeatingControlModule();

    // Initialize the heating control module
    void initialize();

    /**
     * @brief What the curve computed before clamping, for the
     *        boiler/status/heating/control diagnostics topic (2026-10-18).
     *
     * Needed because the clamped result sits flat on the h/sLo floor in mild weather and says
     * nothing about what the curve actually asked for, and because the room-deviation
     * contribution is otherwise invisible - it is folded into the returned target.
     */
    struct CurveDebug {
        int32_t rawTarget = 0;  // pre-clamp, tenths (can be negative)
        int32_t shift = 0;      // curveShift*10 + room-deviation term, tenths
        // What the curve actually used as its room temperature, and where it came from.
        // Needed because a held reading looks exactly like a measured one otherwise.
        Temperature_t effectiveInside = 0;
        RoomTempFallbackPolicy::Source insideSource = RoomTempFallbackPolicy::Source::MEASURED;
    };

    /**
     * @brief The room temperature to control on, and where it came from.
     *
     * One resolver for both consumers of the room reading: the curve's base temperature and
     * the weather-mode ON/OFF backstop. They used to disagree - the curve held the last
     * reading through a dropout (2026-10-23) while the ON/OFF decision read
     * isInsideTempValid raw and silently dropped its backstop, which turned the burner ON
     * when the sensor was pulled (2026-10-25). Sharing lastRoom_ is the point: a second
     * remembered sample could age differently from this one.
     *
     * Records the reading when valid, substitutes per RoomTempFallbackPolicy when not.
     * Callers that must not act on a guess check RoomTempFallbackPolicy::isMeasurement().
     *
     * Takes the room target as a plain value, not the SystemSettings reference: the ON/OFF
     * caller runs holding the sensor mutex with its settings already snapshotted, and
     * reaching back for SystemSettings there would either read it unsynchronised or nest the
     * settings mutex inside the sensor mutex - the H2 ordering this task deliberately avoids.
     *
     * @param roomTarget heating/targetTemperatureInside, tenths; <= 0 means unset
     */
    RoomTempFallbackPolicy::Substitute resolveRoomForControl(const SharedSensorReadings& readings,
                                                             Temperature_t roomTarget) const;

    // Calculate the target temperature for space heating based on shared sensor readings and system settings
    Temperature_t calculateSpaceHeatingTargetTemp(const SharedSensorReadings& readings,
                                                  const SystemSettings& settings,
                                                  CurveDebug* debugOut = nullptr) const;

    // Check if heating conditions are met based on temperature and hysteresis
    bool checkHeatingConditions(const SharedSensorReadings& readings, Temperature_t targetTemperature, Temperature_t hysteresis) const;

    // Start and stop heating
    void startHeating();
    void stopHeating();

private:
    // Logging tag
    static constexpr const char* TAG = "HeatingControl";

    // The last valid room reading, so a brief sensor dropout holds the previous value
    // instead of guessing - see RoomTempFallbackPolicy.h. `mutable` because
    // calculateSpaceHeatingTargetTemp() is const and is also called by the diagnostics
    // publisher; both callers live in HeatingControlTask, a single task, and both pass the
    // same sensor snapshot, so there is nothing to serialise and nothing to disagree about.
    mutable RoomTempFallbackPolicy::Sample lastRoom_;
};

#endif // HEATING_CONTROL_MODULE_H
