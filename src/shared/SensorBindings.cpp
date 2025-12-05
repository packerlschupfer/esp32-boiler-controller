#include "SensorBindings.h"
#include "config/SensorIndices.h"
#include "SharedSensorReadings.h"
#include "core/SystemResourceProvider.h"

namespace SensorBindings {

    std::array<mb8art::SensorBinding, 8> bindings = {};

    void initialize() {
        auto& readings = SRP::getSensorReadings();

        // Bind logical functions to their data variables
        // This is the ONLY place that connects SensorIndex to SharedSensorReadings
        bindings[SensorIndex::BOILER_OUTPUT]  = {&readings.boilerTempOutput,  &readings.isBoilerTempOutputValid};
        bindings[SensorIndex::BOILER_RETURN]  = {&readings.boilerTempReturn,  &readings.isBoilerTempReturnValid};
        bindings[SensorIndex::WATER_TANK]     = {&readings.waterHeaterTempTank,   &readings.isWaterHeaterTempTankValid};
        bindings[SensorIndex::WATER_OUTPUT]   = {&readings.waterHeaterTempOutput, &readings.isWaterHeaterTempOutputValid};
        bindings[SensorIndex::WATER_RETURN]   = {&readings.waterHeaterTempReturn, &readings.isWaterHeaterTempReturnValid};
        bindings[SensorIndex::HEATING_RETURN] = {&readings.heatingTempReturn, &readings.isHeatingTempReturnValid};
        bindings[SensorIndex::OUTSIDE]        = {&readings.outsideTemp,       &readings.isOutsideTempValid};
        bindings[SensorIndex::PRESSURE_CHANNEL] = {nullptr, nullptr};  // Pressure handled separately (not Temperature_t)
    }
}

namespace ANDRTF3Bindings {

    Temperature_t* insideTempPtr = nullptr;
    bool* insideTempValidPtr = nullptr;
    float* insideHumidityPtr = nullptr;
    bool* insideHumidityValidPtr = nullptr;

    void initialize() {
        auto& readings = SRP::getSensorReadings();

        // Bind ANDRTF3 sensors to SharedSensorReadings
        insideTempPtr = &readings.insideTemp;
        insideTempValidPtr = &readings.isInsideTempValid;
        insideHumidityPtr = &readings.insideHumidity;
        insideHumidityValidPtr = &readings.isInsideHumidityValid;
    }
}
