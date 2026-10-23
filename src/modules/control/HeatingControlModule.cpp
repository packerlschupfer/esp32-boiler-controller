// src/modules/control/HeatingControlModule.cpp
#include "modules/control/HeatingControlModule.h"
#include "modules/control/HeatingCurve.h"  // NOMINAL_ROOM_TENTHS fallback
#include "modules/control/RoomTempFallbackPolicy.h"  // hold the last good room reading
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

    // The curve's base room temperature. A sensor marked invalid carries TEMP_INVALID
    // (INT16_MIN = -32768), not a plausible value, and it used to be passed in regardless:
    // on 2026-10-23 08:34:13 that made diff 3281.4 °C, overflowed the curve polynomial and
    // put raw = -755418.5 °C on the diagnostics topic. The clamp masked it, so the
    // commanded target stayed safe, but the arithmetic was undefined behaviour.
    //
    // Resolved FIRST, because the room-deviation shift below depends on the answer:
    // RoomTempFallbackPolicy holds the last valid reading for up to MAX_HOLD_MS, so a
    // few-second Modbus dropout reproduces the previous cycle's output exactly instead of
    // substituting a guess. See RoomTempFallbackPolicy.h for why the hold expires and why a
    // held reading still earns the shift.
    //
    // The warnings below are edge-triggered. This function runs on the ~1 Hz diagnostics
    // path (HeatingControlTask), so a plain LOG_WARN would repeat once a second for as long
    // as the fault lasts - from boot until the first successful read, and forever with an
    // unplugged outside sensor. One line when it breaks and one when it recovers is the
    // whole signal; the repetition only buys log pressure on a device where a single LOG_*
    // costs ~1312 B of stack. Statics are safe here: every caller of this function is
    // HeatingControlTask, a single task.
    Temperature_t effectiveInside = readings.insideTemp;
    RoomTempFallbackPolicy::Source insideSource = RoomTempFallbackPolicy::Source::MEASURED;
    static bool insideWarned = false;
    if (readings.isInsideTempValid) {
        // Timestamp from the SENSOR's last good read, not from millis() here. The flag stays
        // true through up to three failed reads (~15 s, ANDRTF3Task), so recording "now" on
        // every valid-looking cycle would keep resetting the hold window against data that
        // was already going stale. Falls back to millis() only if the field was never set.
        const uint32_t readAt = (readings.insideTempGoodMs != 0) ? readings.insideTempGoodMs
                                                                 : millis();
        RoomTempFallbackPolicy::record(lastRoom_, readings.insideTemp, readAt);
        if (insideWarned) {
            LOG_INFO(TAG, "Room sensor valid again - heating curve back on the real reading");
            insideWarned = false;
        }
    } else {
        const RoomTempFallbackPolicy::Substitute sub =
            RoomTempFallbackPolicy::substitute(lastRoom_, millis(),
                                               settings.targetTemperatureInside);
        effectiveInside = sub.value;
        insideSource = sub.source;
        if (!insideWarned) {
            LOG_WARN(TAG, "Room sensor invalid - heating curve using %d (0.1°C) from %s",
                     effectiveInside, RoomTempFallbackPolicy::toString(insideSource));
            insideWarned = true;
        }
    }

    // Weather-compensated mode: apply room temp deviation curve shift (Parallelverschiebung)
    // If room is cold relative to target → shift curve UP (more heat)
    // If room is warm relative to target → shift curve DOWN (less heat)
    //
    // Gated on the room value being an actual MEASUREMENT (fresh or held) rather than on
    // isInsideTempValid: holding the reading and then dropping its shift term would change
    // the curve output during a dropout, which is the one thing the hold exists to prevent.
    // For the TARGET and NOMINAL substitutions the deviation is zero by construction, so
    // skipping the shift and applying it would agree anyway - except that TARGET would then
    // depend on a float multiply of zero, so it is skipped explicitly.
    if (settings.useWeatherCompensatedControl &&
        RoomTempFallbackPolicy::isMeasurement(insideSource) &&
        settings.targetTemperatureInside > 0) {
        // Room deviation = target - actual (positive if room is cold)
        Temperature_t roomDeviation = tempSub(settings.targetTemperatureInside, effectiveInside);
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

    // Outside has no meaningful substitute: a weather curve without a weather reading is
    // not a curve, and there is no outer loop to catch an error the way the room loop
    // catches a wrong room assumption. Return the floor and say so on the diagnostics topic.
    // HeatingControlTask already refuses to request heating while isOutsideTempValid is
    // false, so this is a backstop for the room-mode path and for any future caller.
    static bool outsideWarned = false;
    if (!readings.isOutsideTempValid) {
        if (!outsideWarned) {
            LOG_WARN(TAG, "Outside sensor invalid - heating curve pinned to the flow floor");
            outsideWarned = true;
        }
        if (debugOut != nullptr) {
            debugOut->rawTarget = settings.burner_low_limit;
            debugOut->shift = static_cast<int32_t>(curveShift);
            debugOut->effectiveInside = effectiveInside;
            debugOut->insideSource = insideSource;
        }
        return settings.burner_low_limit;
    }
    if (outsideWarned) {
        LOG_INFO(TAG, "Outside sensor valid again - heating curve resumed");
        outsideWarned = false;
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
        debugOut->effectiveInside = effectiveInside;
        debugOut->insideSource = insideSource;
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
