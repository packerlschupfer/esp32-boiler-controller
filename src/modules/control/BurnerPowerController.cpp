// src/modules/control/BurnerPowerController.cpp
#include "BurnerPowerController.h"
#include "core/SystemResourceProvider.h"
#include "shared/SharedSensorReadings.h"
#include "shared/Temperature.h"
#include "utils/MutexRetryHelper.h"
#include <esp_log.h>

static const char* TAG = "BurnerPower";

bool BurnerPowerController::shouldIncreasePower(bool requestedHighPower) {
    // SAFETY CHECK: Block high power if temperature already near limit
    // This prevents overshoot when PID requests high power but boiler is already hot
    constexpr Temperature_t HIGH_POWER_LIMIT_TEMP = 800;  // 80.0°C - above this, LOW power only

    auto guard = MutexRetryHelper::acquireGuard(
        SRP::getSensorReadingsMutex(),
        "SensorReadings-PowerCheck"
    );
    if (guard) {
        const SharedSensorReadings& readings = SRP::getSensorReadings();
        if (readings.isBoilerTempOutputValid &&
            readings.boilerTempOutput >= HIGH_POWER_LIMIT_TEMP) {
            // Temperature too high for full power - force LOW regardless of PID request
            if (requestedHighPower) {
                LOG_INFO(TAG, "Blocking high power: boiler temp %d.%d°C >= limit %d.%d°C",
                        readings.boilerTempOutput / 10, abs(readings.boilerTempOutput % 10),
                        HIGH_POWER_LIMIT_TEMP / 10, HIGH_POWER_LIMIT_TEMP % 10);
            }
            return false;
        }
    }

    // Use PID-driven power level decision from HeatingControl/WheaterControl
    // The solenoid gas valve can switch frequently, so we trust PID's calculation
    return requestedHighPower;
}

bool BurnerPowerController::shouldDecreasePower(bool requestedHighPower) {
    // Use PID-driven power level decision - decrease if PID requests LOW power
    // The solenoid gas valve can switch frequently, so we trust PID's calculation
    return !requestedHighPower;
}
