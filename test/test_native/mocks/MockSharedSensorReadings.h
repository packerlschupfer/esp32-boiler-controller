/**
 * @file MockSharedSensorReadings.h
 * @brief Mock SharedSensorReadings for testing without FreeRTOS
 */

#ifndef MOCK_SHARED_SENSOR_READINGS_H
#define MOCK_SHARED_SENSOR_READINGS_H

#include "../../include/shared/Temperature.h"

struct SharedSensorReadings {
    Temperature_t boilerTempInput;
    Temperature_t boilerTempOutput;
    Temperature_t waterTemp;
    Temperature_t returnTemp;
    Temperature_t exhaustTemp;
    Temperature_t pumpTemp;
    Temperature_t insideTemp;
    Temperature_t outsideTemp;
    
    // Constructor with default values
    SharedSensorReadings() :
        boilerTempInput(TEMP_INVALID),
        boilerTempOutput(TEMP_INVALID),
        waterTemp(TEMP_INVALID),
        returnTemp(TEMP_INVALID),
        exhaustTemp(TEMP_INVALID),
        pumpTemp(TEMP_INVALID),
        insideTemp(TEMP_INVALID),
        outsideTemp(TEMP_INVALID) {}
};

#endif // MOCK_SHARED_SENSOR_READINGS_H