// src/modules/control/WheaterControlModule.h
#ifndef WHEATER_CONTROL_MODULE_H
#define WHEATER_CONTROL_MODULE_H

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include "shared/SharedSensorReadings.h"
#include "shared/Temperature.h"
#include "config/SystemSettings.h"
#include "core/SystemResourceProvider.h"

/**
 * @brief Water heating control module
 *
 * This module calculates the boiler target temperature for water heating.
 * Power level control (OFF/HALF/FULL) is handled by BoilerTempControlTask.
 *
 * The target calculation is simple:
 * - Boiler target = water tank temperature + charge delta
 * - Boiler needs to be 5-10°C hotter than tank to charge it effectively
 */
class WheaterControlModule {
public:
    // Constructor
    WheaterControlModule();

    // Destructor
    ~WheaterControlModule();

    // Initialize the water heating control module
    void initialize();

    /**
     * @brief Calculate boiler target for water heating
     * @param readings Current sensor readings
     * @param settings System settings (contains charge delta)
     * @return Target boiler output temperature (tank temp + delta)
     */
    Temperature_t calculateWaterHeatingTargetTemp(const SharedSensorReadings& readings,
                                                   const SystemSettings& settings) const;

private:
    // Logging tag
    static constexpr const char* TAG = "WheaterControl";
};

#endif // WHEATER_CONTROL_MODULE_H
