// src/modules/control/WheaterControlModule.cpp
#include "modules/control/WheaterControlModule.h"
#include "config/SystemConstants.h"
#include "LoggingMacros.h"

WheaterControlModule::WheaterControlModule() {
    // Nothing to initialize - PID code removed
}

WheaterControlModule::~WheaterControlModule() {
    // Nothing to clean up - PID code removed
}

void WheaterControlModule::initialize() {
    LOG_INFO(TAG, "WheaterControlModule initialized.");
    LOG_INFO(TAG, "Note: PID control removed - power levels determined by BoilerTempController");
}

Temperature_t WheaterControlModule::calculateWaterHeatingTargetTemp(
    const SharedSensorReadings& readings,
    const SystemSettings& settings) const {

    // Water heating target = tank temperature + charge delta
    // Boiler needs to be 5-10°C hotter than tank to charge it effectively

    Temperature_t chargeDelta = tempFromFloat(settings.wHeaterConfTempChargeDelta);
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
