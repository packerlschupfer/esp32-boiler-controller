// src/modules/control/HeatingControlModule.cpp
#include "modules/control/HeatingControlModule.h"
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
    LOG_INFO(TAG, "Note: PID control removed - power levels determined by BoilerTempController");
}

Temperature_t HeatingControlModule::calculateSpaceHeatingTargetTemp(const SharedSensorReadings& readings, const SystemSettings& settings) const {
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

    Temperature_t result = HeatingControlModuleFixedPoint::calculateHeatingCurveTarget(
        readings.insideTemp,
        readings.outsideTemp,
        curveCoeff,
        curveShift,
        settings.burner_low_limit,
        settings.heating_high_limit
    );

    char insideBuf[16], outsideBuf[16], resultBuf[16];
    formatTemp(insideBuf, sizeof(insideBuf), readings.insideTemp);
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
