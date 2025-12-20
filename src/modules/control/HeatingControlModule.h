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

    // Calculate the target temperature for space heating based on shared sensor readings and system settings
    Temperature_t calculateSpaceHeatingTargetTemp(const SharedSensorReadings& readings, const SystemSettings& settings) const;

    // Check if heating conditions are met based on temperature and hysteresis
    bool checkHeatingConditions(const SharedSensorReadings& readings, Temperature_t targetTemperature, Temperature_t hysteresis) const;

    // Start and stop heating
    void startHeating();
    void stopHeating();

private:
    // Logging tag
    static constexpr const char* TAG = "HeatingControl";
};

#endif // HEATING_CONTROL_MODULE_H
