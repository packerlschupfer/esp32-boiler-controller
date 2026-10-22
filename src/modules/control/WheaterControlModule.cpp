// src/modules/control/WheaterControlModule.cpp
#include "modules/control/WheaterControlModule.h"
#include "config/SystemConstants.h"
#include "modules/control/WaterChargePolicy.h"
#include "LoggingMacros.h"

WheaterControlModule::WheaterControlModule() {
    // Nothing to initialize - PID code removed
}

WheaterControlModule::~WheaterControlModule() {
    // Nothing to clean up - PID code removed
}

void WheaterControlModule::initialize() {
    LOG_INFO(TAG, "WheaterControlModule initialized.");
}

Temperature_t WheaterControlModule::calculateWaterHeatingTargetTemp(
    const SharedSensorReadings& readings,
    const SystemSettings& settings) const {

    // Water heating target = tank temperature + charge delta
    // Boiler needs to be 5-10°C hotter than tank to charge it effectively

    // NOTE: calculateWaterHeatingTargetTemp() currently has no callers - WheaterControlTask
    // computes its own target (see TaskInitializer.cpp:307). Kept in sync with that copy
    // via WaterChargePolicy so the two cannot drift if this is ever wired up; the
    // wire-or-delete decision is still open.
    Temperature_t chargeDelta = tempFromFloat(
        WaterChargePolicy::chargeDeltaOrDefault(settings.wHeaterConfTempChargeDelta));
    Temperature_t targetTemp = tempAdd(readings.waterHeaterTempTank, chargeDelta);

    char tankBuf[16], deltaBuf[16], targetBuf[16];
    formatTemp(tankBuf, sizeof(tankBuf), readings.waterHeaterTempTank);
    formatTemp(deltaBuf, sizeof(deltaBuf), chargeDelta);
    formatTemp(targetBuf, sizeof(targetBuf), targetTemp);
    LOG_DEBUG(TAG, "Water target: tank %s + delta %s = %s", tankBuf, deltaBuf, targetBuf);

    // Apply safety limits (from SystemConstants)
    const Temperature_t MIN_TARGET = SystemConstants::WaterHeating::MIN_TARGET_TEMP;
    const Temperature_t MAX_TARGET = SystemConstants::WaterHeating::MAX_TARGET_TEMP;

    if (targetTemp < MIN_TARGET) {
        targetTemp = MIN_TARGET;
    } else if (targetTemp > MAX_TARGET) {
        targetTemp = MAX_TARGET;
    }

    return targetTemp;
}
