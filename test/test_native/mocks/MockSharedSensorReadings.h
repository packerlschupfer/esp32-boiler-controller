/**
 * @file MockSharedSensorReadings.h
 * @brief Mock SharedSensorReadings for testing without FreeRTOS
 *
 * Updated to match real SharedSensorReadings.h structure
 */

#ifndef MOCK_SHARED_SENSOR_READINGS_H
#define MOCK_SHARED_SENSOR_READINGS_H

#include "../../include/shared/Temperature.h"

// Mock Pressure_t if not available
#ifndef PRESSURE_T_DEFINED
typedef int16_t Pressure_t;
#define PRESSURE_T_DEFINED
#endif

struct SharedSensorReadings {
    // Boiler temperature readings (in tenths of degrees Celsius)
    Temperature_t boilerTempOutput = 0;
    Temperature_t boilerTempReturn = 0;
    bool isBoilerTempOutputValid = false;
    bool isBoilerTempReturnValid = false;

    // Water heater readings (in tenths of degrees Celsius)
    Temperature_t waterHeaterTempTank = 0;
    bool isWaterHeaterTempTankValid = false;

    // Environment temperature readings (in tenths of degrees Celsius)
    Temperature_t outsideTemp = 0;
    Temperature_t insideTemp = 0;
    float insideHumidity = 0.0f;
    bool isOutsideTempValid = false;
    bool isInsideTempValid = false;
    bool isInsideHumidityValid = false;

    // System pressure reading (in hundredths of BAR)
    Pressure_t systemPressure = 0;
    bool isSystemPressureValid = false;

    // Timestamps for freshness validation
    uint32_t lastUpdateTimestamp = 0;
    uint32_t lastPressureUpdateTimestamp = 0;

    // Legacy accessor methods for backward compatibility with existing tests
    // (Using methods instead of references to avoid copy/assignment issues)
    Temperature_t& waterTemp() { return waterHeaterTempTank; }
    Temperature_t& returnTemp() { return boilerTempReturn; }

    // Constructor with default invalid values
    SharedSensorReadings() :
        boilerTempOutput(TEMP_INVALID),
        boilerTempReturn(TEMP_INVALID),
        isBoilerTempOutputValid(false),
        isBoilerTempReturnValid(false),
        waterHeaterTempTank(TEMP_INVALID),
        isWaterHeaterTempTankValid(false),
        outsideTemp(TEMP_INVALID),
        insideTemp(TEMP_INVALID),
        insideHumidity(0.0f),
        isOutsideTempValid(false),
        isInsideTempValid(false),
        isInsideHumidityValid(false),
        systemPressure(0),
        isSystemPressureValid(false),
        lastUpdateTimestamp(0),
        lastPressureUpdateTimestamp(0) {}

    // Helper to set all temperatures valid with reasonable values
    void setAllValid(Temperature_t boilerOut = 500, Temperature_t boilerRet = 450,
                     Temperature_t waterTank = 450, Temperature_t outside = 100,
                     Temperature_t inside = 200) {
        boilerTempOutput = boilerOut;
        boilerTempReturn = boilerRet;
        waterHeaterTempTank = waterTank;
        outsideTemp = outside;
        insideTemp = inside;

        isBoilerTempOutputValid = true;
        isBoilerTempReturnValid = true;
        isWaterHeaterTempTankValid = true;
        isOutsideTempValid = true;
        isInsideTempValid = true;

        lastUpdateTimestamp = 1;  // Non-zero indicates data received
    }

    // Helper to invalidate all sensors
    void setAllInvalid() {
        boilerTempOutput = TEMP_INVALID;
        boilerTempReturn = TEMP_INVALID;
        waterHeaterTempTank = TEMP_INVALID;
        outsideTemp = TEMP_INVALID;
        insideTemp = TEMP_INVALID;

        isBoilerTempOutputValid = false;
        isBoilerTempReturnValid = false;
        isWaterHeaterTempTankValid = false;
        isOutsideTempValid = false;
        isInsideTempValid = false;

        lastUpdateTimestamp = 0;
    }
};

#endif // MOCK_SHARED_SENSOR_READINGS_H
