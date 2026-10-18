// src/modules/control/BurnerPowerController.cpp
#include "BurnerPowerController.h"
#include "core/SystemResourceProvider.h"
#include "shared/SharedSensorReadings.h"
#include "shared/Temperature.h"
#include "config/SystemConstants.h"
#include "utils/MutexRetryHelper.h"
#include <esp_log.h>

static const char* TAG = "BurnerPower";

bool BurnerPowerController::shouldIncreasePower(bool requestedHighPower) {
    // SAFETY CHECK: Block high power if temperature already near limit
    // This prevents overshoot when PID requests high power but boiler is already hot.
    // Threshold lives in SystemConstants::Temperature beside MAX/CRITICAL_BOILER_TEMP_C.
    constexpr Temperature_t HIGH_POWER_LIMIT_TEMP =
        SystemConstants::Temperature::HIGH_POWER_LIMIT_TEMP_C;

    auto guard = MutexRetryHelper::acquireGuard(
        SRP::getSensorReadingsMutex(),
        "SensorReadings-PowerCheck"
    );
    if (!guard) {
        // FAIL CLOSED. This used to fall through to `return requestedHighPower`, i.e. it
        // granted FULL power precisely when it could not check the temperature ceiling -
        // the one case where the check matters. Reachable: MQTTPublisher.cpp:109 holds the
        // sensor mutex in a function-scoped guard while building the sensor JSON and
        // taking two further mutexes inside that hold, which is long enough for all of
        // MutexRetryHelper's attempts to fail (same hold that latched SENSOR_FAILURE, see
        // SensorErrorBitPolicy.h). Denying HIGH costs a slower recovery for one cycle and
        // is re-evaluated on the next; granting it risks overshoot above
        // HIGH_POWER_LIMIT_TEMP with no reading to stop it.
        LOG_WARN(TAG, "Sensor mutex unavailable - denying high power this cycle");
        return false;
    }

    {
        const SharedSensorReadings& readings = SRP::getSensorReadings();
        // An invalid reading is treated the same way, and for the same reason: no
        // temperature means no ceiling check.
        if (!readings.isBoilerTempOutputValid) {
            LOG_WARN(TAG, "Boiler output invalid - denying high power this cycle");
            return false;
        }
        if (readings.boilerTempOutput >= HIGH_POWER_LIMIT_TEMP) {
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
