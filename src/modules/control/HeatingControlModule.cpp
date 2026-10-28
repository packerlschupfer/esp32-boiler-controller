// src/modules/control/HeatingControlModule.cpp
#include "modules/control/HeatingControlModule.h"
#include "modules/control/HeatingCurve.h"  // NOMINAL_ROOM_TENTHS fallback, anchoring()
#include "modules/control/SpaceHeatingPolicy.h"  // orderFlowLimits
#include <cmath>  // lroundf, to scale roomCurveShiftFactor into the policy x10 integer
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

RoomTempFallbackPolicy::Substitute HeatingControlModule::resolveRoomForControl(
    const SharedSensorReadings& readings, Temperature_t roomTarget) const {
    // The warnings below are edge-triggered. This runs on the ~1 Hz diagnostics path and on
    // the ON/OFF decision path (both HeatingControlTask), so a plain LOG_WARN would repeat
    // for as long as the fault lasts - from boot until the first successful read, and
    // forever with an unplugged sensor. One line when it breaks and one when it recovers is
    // the whole signal; the repetition only buys log pressure on a device where a single
    // LOG_* costs ~1312 B of stack. Statics are safe here: every caller is
    // HeatingControlTask, a single task.
    static bool insideWarned = false;

    // The decision itself lives in RoomTempFallbackPolicy::resolve(), including the
    // insideTempGoodMs-not-millis() timestamp choice. It moved there on 2026-10-25 so it could
    // be tested: this function needs FreeRTOS and SRP to reach, which is why the review found
    // the resolver untestable. What is left here is the edge-triggered logging, which is the
    // only part that genuinely belongs in the firmware.
    const RoomTempFallbackPolicy::Substitute sub = RoomTempFallbackPolicy::resolve(
        lastRoom_, readings.isInsideTempValid, readings.insideTemp,
        readings.insideTempGoodMs, millis(), roomTarget);

    if (RoomTempFallbackPolicy::Source::MEASURED == sub.source) {
        if (insideWarned) {
            LOG_INFO(TAG, "Room sensor valid again - back on the real reading");
            insideWarned = false;
        }
    } else if (!insideWarned) {
        LOG_WARN(TAG, "Room sensor invalid - using %d (0.1°C) from %s",
                 sub.value, RoomTempFallbackPolicy::toString(sub.source));
        insideWarned = true;
    }
    return sub;
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
    const RoomTempFallbackPolicy::Substitute room =
        resolveRoomForControl(readings, settings.targetTemperatureInside);
    Temperature_t effectiveInside = room.value;
    RoomTempFallbackPolicy::Source insideSource = room.source;

    // Weather-compensated mode: anchor the curve on the TARGET and put the room deviation
    // in the shift (Parallelverschiebung). Room cold -> curve UP, room warm -> curve DOWN.
    //
    // Both halves come from HeatingCurve::anchoring() as a pair. Until 2026-10-26 this
    // function passed the live room reading as the curve's own base AS WELL as deriving the
    // shift from it, so the room fed the curve twice and the terms fought: the net
    // sensitivity was +0.70 K/K at 12 C outside, i.e. a COLDER house got a LOWER flow
    // target. See HeatingCurve::anchoring() for the measurements and why no value of
    // roomCurveShiftFactor can fix it.
    //
    // Gated on the room value being an actual MEASUREMENT (fresh or held) rather than on
    // isInsideTempValid: holding the reading and then dropping its shift term would change
    // the curve output during a dropout, which is the one thing the hold exists to prevent.
    // For the TARGET and NOMINAL substitutions the policy returns the old behaviour
    // unchanged - with TARGET the two are identical anyway, and with NOMINAL there is no
    // target to anchor on.
    //
    // The factor is scaled to x10 here because the policy is integer-only, so the host tests
    // reproduce the firmware exactly rather than approximately.
    const HeatingCurve::Anchoring anchor = HeatingCurve::anchoring(
        curveShift,
        settings.targetTemperatureInside,
        effectiveInside,
        static_cast<int16_t>(lroundf(settings.roomTempCurveShiftFactor * 10.0f)),
        settings.useWeatherCompensatedControl,
        RoomTempFallbackPolicy::isMeasurement(insideSource));

    const Temperature_t curveInside = anchor.inside;
    if (anchor.shift != curveShift) {
        char devBuf[16];
        formatTemp(devBuf, sizeof(devBuf),
                   tempSub(settings.targetTemperatureInside, effectiveInside));
        LOG_DEBUG(TAG, "Weather mode: room deviation %s°C → shift %d → %d (0.1°C), "
                       "curve anchored on the target",
                  devBuf, static_cast<int>(curveShift), static_cast<int>(anchor.shift));
    }
    curveShift = anchor.shift;

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

    // Order the curve's own clamp pair before using it, for the same reason the water path
    // does: BurnerRequestManager orders the pair it receives, but by then this function has
    // already clamped the curve output to the RAW, possibly inverted, pair - so the ordering
    // downstream had nothing left to recover. With h/sLo 60.0 and h/sHi 50.0 (each inside its
    // own registered range) the curve was capped at 50.0 here regardless. Found 2026-10-28.
    Temperature_t curveLow = settings.burner_low_limit;
    Temperature_t curveHigh = settings.heating_high_limit;
    (void)SpaceHeatingPolicy::orderFlowLimits(curveLow, curveHigh);

    int32_t rawTarget = 0;
    Temperature_t result = HeatingControlModuleFixedPoint::calculateHeatingCurveTarget(
        curveInside,
        readings.outsideTemp,
        curveCoeff,
        curveShift,
        curveLow,
        curveHigh,
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
