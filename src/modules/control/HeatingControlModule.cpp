// src/modules/control/HeatingControlModule.cpp
#include "modules/control/HeatingControlModule.h"
#include "modules/control/HeatingCurve.h"  // NOMINAL_ROOM_TENTHS fallback
#include "modules/control/HeatingControlModuleFixedPoint.h"  // For fixed-point heating curve calculations
#include "shared/Temperature.h"  // For temperature conversions
#include "config/SystemConstants.h"  // For magic number constants
#include "LoggingMacros.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "core/SystemResourceProvider.h"
#include "events/SystemEventsGenerated.h"

[[maybe_unused]] static const char* TAG = "HeatingControl";

HeatingControlModule::HeatingControlModule(EventGroupHandle_t systemEventGroup, SemaphoreHandle_t sensorMutex) {
    // Parameters are ignored - kept for backward compatibility
    // The module now uses SystemResourceProvider internally
    (void)systemEventGroup;
    (void)sensorMutex;
}

HeatingControlModule::~HeatingControlModule() {
    // Nothing to clean up - PID code removed
}

void HeatingControlModule::initialize() {
    LOG_INFO(TAG, "HeatingControlModule initialized.");
}

Temperature_t HeatingControlModule::calculateSpaceHeatingTargetTemp(
    const SharedSensorReadings& readings, const SystemSettings& settings,
    CurveDebug* debugOut) const {
    // Use fixed-point calculations from HeatingControlModuleFixedPoint
    // Convert float coefficients to fixed-point (scaled by 100)
    int16_t curveCoeff = static_cast<int16_t>(settings.heating_curve_coeff * 100);
    Temperature_t curveShift = tempFromFloat(settings.heating_curve_shift);

    // Weather-compensated mode: apply room temp deviation curve shift (Parallelverschiebung)
    // If room is cold relative to target → shift curve UP (more heat)
    // If room is warm relative to target → shift curve DOWN (less heat)
    if (settings.useWeatherCompensatedControl && readings.isInsideTempValid &&
        settings.targetTemperatureInside > 0) {
        // Room deviation = target - actual (positive if room is cold)
        Temperature_t roomDeviation = tempSub(settings.targetTemperatureInside, readings.insideTemp);
        // Curve shift adjustment = deviation * factor
        // e.g., room 1°C cold with factor 2.0 → curve +2°C
        float shiftAdjust = tempToFloat(roomDeviation) * settings.roomTempCurveShiftFactor;
        curveShift = tempAdd(curveShift, tempFromFloat(shiftAdjust));

        char devBuf[16], adjustBuf[16];
        formatTemp(devBuf, sizeof(devBuf), roomDeviation);
        snprintf(adjustBuf, sizeof(adjustBuf), "%.1f", shiftAdjust);
        LOG_DEBUG(TAG, "Weather mode: room deviation %s°C → curve shift +%s°C",
                 devBuf, adjustBuf);
    }

    // The curve's base temperature. A sensor marked invalid carries TEMP_INVALID
    // (INT16_MIN = -32768), not a plausible value, and it used to be passed in regardless:
    // on 2026-10-23 08:34:13 that made diff 3281.4 °C, overflowed the curve polynomial and
    // put raw = -755418.5 °C on the diagnostics topic. The clamp masked it, so the
    // commanded target stayed safe, but the arithmetic was undefined behaviour.
    //
    // With no room reading, assume the room is AT target. That is the same assumption the
    // skipped room-deviation shift above already makes - zero deviation means inside equals
    // target - so the curve falls back to the nominal line for the current outside
    // temperature instead of to a wrong one. If the target is also unset, use the curve's
    // own nominal room temperature.
    Temperature_t effectiveInside = readings.insideTemp;
    if (!readings.isInsideTempValid) {
        effectiveInside = (settings.targetTemperatureInside > 0)
                              ? settings.targetTemperatureInside
                              : HeatingCurve::NOMINAL_ROOM_TENTHS;
        LOG_WARN(TAG, "Room sensor invalid - heating curve using %d (0.1°C) as its base",
                 effectiveInside);
    }

    // Outside has no meaningful substitute: a weather curve without a weather reading is
    // not a curve. Return the floor and say so on the diagnostics topic. HeatingControlTask
    // already refuses to request heating while isOutsideTempValid is false, so this is a
    // backstop for the room-mode path and for any future caller.
    if (!readings.isOutsideTempValid) {
        LOG_WARN(TAG, "Outside sensor invalid - heating curve pinned to the flow floor");
        if (debugOut != nullptr) {
            debugOut->rawTarget = settings.burner_low_limit;
            debugOut->shift = static_cast<int32_t>(curveShift);
        }
        return settings.burner_low_limit;
    }

    int32_t rawTarget = 0;
    Temperature_t result = HeatingControlModuleFixedPoint::calculateHeatingCurveTarget(
        effectiveInside,
        readings.outsideTemp,
        curveCoeff,
        curveShift,
        settings.burner_low_limit,
        settings.heating_high_limit,
        &rawTarget
    );

    if (debugOut != nullptr) {
        debugOut->rawTarget = rawTarget;
        // curveShift here already carries the room-deviation term added above when weather
        // compensation is on, which is exactly the value the diagnostics topic reports.
        debugOut->shift = static_cast<int32_t>(curveShift);
    }

    char insideBuf[16], outsideBuf[16], resultBuf[16];
    formatTemp(insideBuf, sizeof(insideBuf), effectiveInside);
    formatTemp(outsideBuf, sizeof(outsideBuf), readings.outsideTemp);
    formatTemp(resultBuf, sizeof(resultBuf), result);
    LOG_DEBUG(TAG, "Calculated target temp: %s, based on inside: %s, outside: %s",
             resultBuf, insideBuf, outsideBuf);
    return result;
}

bool HeatingControlModule::checkHeatingConditions(const SharedSensorReadings& readings, Temperature_t targetTemperature, Temperature_t hysteresis) const {
    bool heatingRequired = false;
    EventBits_t currentSystemState = xEventGroupGetBits(SRP::getSystemStateEventGroup());

    // Check if heating is enabled
    if ((currentSystemState & SystemEvents::SystemState::HEATING_ENABLED) != 0) {

        // Determine if heating should be turned on or off based on temperature and hysteresis
        if (readings.insideTemp < tempSub(targetTemperature, hysteresis)) {
            heatingRequired = true;
        } else if (readings.insideTemp >= tempAdd(targetTemperature, hysteresis)) {
            heatingRequired = false;
        }
    } else {
        // Heating is disabled; ensure it's off
        heatingRequired = false;
    }

    char insideBuf[16], targetBuf[16], hystBuf[16];
    formatTemp(insideBuf, sizeof(insideBuf), readings.insideTemp);
    formatTemp(targetBuf, sizeof(targetBuf), targetTemperature);
    formatTemp(hystBuf, sizeof(hystBuf), hysteresis);
    LOG_DEBUG(TAG, "Heating required: %s (inside: %s, target: %s, hyst: %s)",
             heatingRequired ? "Yes" : "No", insideBuf, targetBuf, hystBuf);
    return heatingRequired;
}

void HeatingControlModule::startHeating() {
    LOG_INFO(TAG, "Starting heating...");
    xEventGroupSetBits(SRP::getSystemStateEventGroup(), SystemEvents::SystemState::HEATING_ON);
}

void HeatingControlModule::stopHeating() {
    LOG_INFO(TAG, "Stopping heating...");
    xEventGroupClearBits(SRP::getSystemStateEventGroup(), SystemEvents::SystemState::HEATING_ON);
}
