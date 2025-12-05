// src/shared/SharedSensorReadings.cpp
#include "SharedSensorReadings.h"

// Initialize shared sensor readings with default values
SharedSensorReadings sharedSensorReadings = {
    // Boiler readings (using TEMP_INVALID for uninitialized temperatures)
    .boilerTempOutput = TEMP_INVALID,
    .boilerTempReturn = TEMP_INVALID,
    .isBoilerTempOutputValid = false,
    .isBoilerTempReturnValid = false,

    // Water heater readings
    .waterHeaterTempTank = TEMP_INVALID,
    .waterHeaterTempOutput = TEMP_INVALID,
    .waterHeaterTempReturn = TEMP_INVALID,
    .isWaterHeaterTempTankValid = false,
    .isWaterHeaterTempOutputValid = false,
    .isWaterHeaterTempReturnValid = false,

    // Heating system readings
    .heatingTempReturn = TEMP_INVALID,
    .isHeatingTempReturnValid = false,

    // Environment readings
    .outsideTemp = TEMP_INVALID,
    .insideTemp = TEMP_INVALID,
    .insideHumidity = 0.0f,  // Keep humidity as float
    .isOutsideTempValid = false,
    .isInsideTempValid = false,
    .isInsideHumidityValid = false,

    .lastUpdateTimestamp = 0
};

// NOTE: Mutex is managed by SharedResourceManager (accessed via SRP::getSensorReadingsMutex())
// Do NOT create a static mutex here - it would be created before FreeRTOS scheduler starts
