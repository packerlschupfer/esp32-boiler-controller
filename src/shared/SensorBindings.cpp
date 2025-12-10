#include "SensorBindings.h"
#include "config/SensorIndices.h"
#include "config/ProjectConfig.h"
#include "SharedSensorReadings.h"
#include "core/SystemResourceProvider.h"

namespace SensorBindings {

    std::array<mb8art::SensorBinding, 8> bindings = {};

    void initialize() {
        auto& readings = SRP::getSensorReadings();

        // Bind logical functions to their data variables
        // This is the ONLY place that connects SensorIndex to SharedSensorReadings

        // Core sensors (always enabled)
        bindings[SensorIndex::BOILER_OUTPUT]  = {&readings.boilerTempOutput,  &readings.isBoilerTempOutputValid};
        bindings[SensorIndex::BOILER_RETURN]  = {&readings.boilerTempReturn,  &readings.isBoilerTempReturnValid};
        bindings[SensorIndex::WATER_TANK]     = {&readings.waterHeaterTempTank, &readings.isWaterHeaterTempTankValid};
        bindings[SensorIndex::OUTSIDE]        = {&readings.outsideTemp,       &readings.isOutsideTempValid};
        bindings[SensorIndex::PRESSURE_CHANNEL] = {nullptr, nullptr};  // CH4 - Pressure handled separately

        // Optional sensors (enable via ENABLE_SENSOR_* flags in ProjectConfig.h)
#ifdef ENABLE_SENSOR_WATER_TANK_TOP
        bindings[SensorIndex::WATER_TANK_TOP] = {&readings.waterTankTopTemp, &readings.isWaterTankTopTempValid};
#else
        bindings[SensorIndex::WATER_TANK_TOP] = {nullptr, nullptr};  // Disabled
#endif

#ifdef ENABLE_SENSOR_WATER_RETURN
        bindings[SensorIndex::WATER_RETURN]   = {&readings.waterHeaterTempReturn, &readings.isWaterHeaterTempReturnValid};
#else
        bindings[SensorIndex::WATER_RETURN]   = {nullptr, nullptr};  // Disabled
#endif

#ifdef ENABLE_SENSOR_HEATING_RETURN
        bindings[SensorIndex::HEATING_RETURN] = {&readings.heatingTempReturn, &readings.isHeatingTempReturnValid};
#else
        bindings[SensorIndex::HEATING_RETURN] = {nullptr, nullptr};  // Disabled
#endif
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
