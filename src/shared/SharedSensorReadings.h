// src/shared/SharedSensorReadings.h
#ifndef SHARED_SENSOR_READINGS_H
#define SHARED_SENSOR_READINGS_H

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "shared/Temperature.h"
#include "shared/Pressure.h"

struct SharedSensorReadings {
    // Boiler temperature readings (in tenths of degrees Celsius)
    // All fields initialized to prevent garbage values at startup
    Temperature_t boilerTempOutput = 0;
    Temperature_t boilerTempReturn = 0;
    bool isBoilerTempOutputValid = false;
    bool isBoilerTempReturnValid = false;

    // Water heater readings (in tenths of degrees Celsius)
    Temperature_t waterHeaterTempTank = 0;    // Renamed from wHeaterTempTank
    Temperature_t waterHeaterTempOutput = 0;  // Renamed from wHeaterTempOutput
    Temperature_t waterHeaterTempReturn = 0;  // Renamed from wHeaterTempReturn
    bool isWaterHeaterTempTankValid = false;  // Renamed from isWHeaterTempTankValid
    bool isWaterHeaterTempOutputValid = false; // Renamed from isWHeaterTempOutputValid
    bool isWaterHeaterTempReturnValid = false; // Renamed from isWHeaterTempReturnValid

    // Heating system readings (in tenths of degrees Celsius)
    Temperature_t heatingTempReturn = 0;
    bool isHeatingTempReturnValid = false;

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
