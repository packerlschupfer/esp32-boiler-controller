// src/shared/TempSensorMapping.cpp
#include "TempSensorMapping.h"
#include "core/SystemResourceProvider.h"
#include "events/SystemEventsGenerated.h"

// No external declarations needed - using SRP methods

std::vector<TempSensorMapping> sensorMappings = {
    {0, &SRP::getSensorReadings().boilerTempOutput, &SRP::getSensorReadings().isBoilerTempOutputValid, SystemEvents::SensorUpdate::BOILER_OUTPUT, SystemEvents::SensorUpdate::BOILER_OUTPUT_ERROR, true},
    {1, &SRP::getSensorReadings().boilerTempReturn, &SRP::getSensorReadings().isBoilerTempReturnValid, SystemEvents::SensorUpdate::BOILER_RETURN, SystemEvents::SensorUpdate::BOILER_RETURN_ERROR, true},
    {2, &SRP::getSensorReadings().wHeaterTempTank, &SRP::getSensorReadings().isWHeaterTempTankValid, SystemEvents::SensorUpdate::WATER_TANK, SystemEvents::SensorUpdate::WATER_TANK_ERROR, true},
    {3, &SRP::getSensorReadings().wHeaterTempOutput, &SRP::getSensorReadings().isWHeaterTempOutputValid, SystemEvents::SensorUpdate::WATER_OUTPUT, SystemEvents::SensorUpdate::WATER_OUTPUT_ERROR, true},
    {4, &SRP::getSensorReadings().wHeaterTempReturn, &SRP::getSensorReadings().isWHeaterTempReturnValid, SystemEvents::SensorUpdate::WATER_RETURN, SystemEvents::SensorUpdate::WATER_RETURN_ERROR, true},
    {5, &SRP::getSensorReadings().heatingTempReturn, &SRP::getSensorReadings().isHeatingTempReturnValid, SystemEvents::SensorUpdate::HEATING_RETURN, SystemEvents::SensorUpdate::HEATING_RETURN_ERROR, true},
    {6, &SRP::getSensorReadings().outsideTemp, &SRP::getSensorReadings().isOutsideTempValid, SystemEvents::SensorUpdate::OUTSIDE, SystemEvents::SensorUpdate::OUTSIDE_ERROR, true},
    // Channel 7 (inside temp) removed - now handled by ANDRTF3Task
};
