// src/shared/SharedSensorReadings.h
#ifndef SHARED_SENSOR_READINGS_H
#define SHARED_SENSOR_READINGS_H

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "shared/Temperature.h"
#include "shared/Pressure.h"
#include "config/ProjectConfig.h"

struct SharedSensorReadings {
    // Boiler temperature readings (in tenths of degrees Celsius)
    // All fields initialized to prevent garbage values at startup
    Temperature_t boilerTempOutput = 0;
    Temperature_t boilerTempReturn = 0;
    bool isBoilerTempOutputValid = false;
    bool isBoilerTempReturnValid = false;

    // Water heater readings (in tenths of degrees Celsius)
    Temperature_t waterHeaterTempTank = 0;
    bool isWaterHeaterTempTankValid = false;

    // Optional water sensors (enable via ENABLE_SENSOR_* flags)
#ifdef ENABLE_SENSOR_WATER_TANK_TOP
    Temperature_t waterTankTopTemp = 0;       // CH5 - Top of tank (stratification)
    bool isWaterTankTopTempValid = false;
#endif

#ifdef ENABLE_SENSOR_WATER_RETURN
    Temperature_t waterHeaterTempReturn = 0;  // CH6 - Water return
    bool isWaterHeaterTempReturnValid = false;
#endif

    // Optional heating sensor
#ifdef ENABLE_SENSOR_HEATING_RETURN
    Temperature_t heatingTempReturn = 0;      // CH7 - Heating return
    bool isHeatingTempReturnValid = false;
#endif

    // Environment temperature readings (in tenths of degrees Celsius)
    Temperature_t outsideTemp = 0;
    Temperature_t insideTemp = 0;
    float insideHumidity = 0.0f;  // Keep humidity as float (not temperature)
    bool isOutsideTempValid = false;
    bool isInsideTempValid = false;
    bool isInsideHumidityValid = false;

    // System pressure reading (in hundredths of BAR)
    Pressure_t systemPressure = 0;
    bool isSystemPressureValid = false;

    // Timestamps initialized to 0 - checked before use for freshness validation
    uint32_t lastUpdateTimestamp = 0;
    uint32_t lastPressureUpdateTimestamp = 0;

    // Add more fields as required...
};

// Note: Access to shared sensor readings should be done through
// SystemResourceProvider (SRP) methods:
// - SRP::getSensorReadings() - Get reference to shared readings
// - SRP::getSensorReadingsMutex() - Get mutex for thread-safe access

#endif // SHARED_SENSOR_READINGS_H
