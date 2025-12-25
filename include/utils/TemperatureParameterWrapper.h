#ifndef TEMPERATURE_PARAMETER_WRAPPER_H
#define TEMPERATURE_PARAMETER_WRAPPER_H

#include "shared/Temperature.h"
#include <PersistentStorage.h>
#include <functional>

/**
 * @brief Wrapper class to enable Temperature_t parameters to work with PersistentStorage float interface
 * 
 * This class provides a bridge between Temperature_t fixed-point values and the float-based
 * PersistentStorage system. It maintains a float shadow value that stays synchronized with
 * the Temperature_t value for storage purposes.
 */
class TemperatureParameterWrapper {
public:
    /**
     * @brief Constructor
     * @param tempPtr Pointer to the Temperature_t value to wrap
     * @param floatShadow Pointer to a float shadow value for storage
     */
    TemperatureParameterWrapper(Temperature_t* tempPtr, float* floatShadow)
        : temperaturePtr(tempPtr), floatShadowPtr(floatShadow) {
        // Initialize shadow with current temperature value
        if (temperaturePtr && floatShadowPtr) {
            *floatShadowPtr = tempToFloat(*temperaturePtr);
        }
    }
    
    /**
     * @brief Register a Temperature_t parameter with PersistentStorage
     * @param storage PersistentStorage instance
     * @param name Parameter name
     * @param tempPtr Pointer to Temperature_t value
     * @param floatShadow Pointer to float shadow value
     * @param minVal Minimum temperature in Celsius
     * @param maxVal Maximum temperature in Celsius
     * @param description Parameter description
     * @param access Access level
     * @return Registration result
     */
    static PersistentStorage::Result registerTemperature(
        PersistentStorage* storage,
        const std::string& name,
        Temperature_t* tempPtr,
        float* floatShadow,
        float minVal,
        float maxVal,
        const std::string& description = "",
        ParameterInfo::Access access = ParameterInfo::ACCESS_READ_WRITE) {
        
        // Register the float shadow with PersistentStorage
        auto result = storage->registerFloat(name, floatShadow, minVal, maxVal, description, access);
        
        if (result == PersistentStorage::Result::SUCCESS) {
            // Set up onChange callback to sync Temperature_t when float changes
            storage->setOnChange(name, [tempPtr, floatShadow](const std::string& name, const void* value) {
                if (tempPtr && floatShadow) {
                    // Update Temperature_t from the changed float value
                    *tempPtr = tempFromFloat(*floatShadow);
                }
            });
        }
        
        return result;
    }
    
    /**
     * @brief Sync Temperature_t to float shadow (call before saving)
     */
    void syncToFloat() {
        if (temperaturePtr && floatShadowPtr) {
            *floatShadowPtr = tempToFloat(*temperaturePtr);
        }
    }
    
    /**
     * @brief Sync float shadow to Temperature_t (call after loading)
     */
    void syncFromFloat() {
        if (temperaturePtr && floatShadowPtr) {
            *temperaturePtr = tempFromFloat(*floatShadowPtr);
        }
    }

private:
    Temperature_t* temperaturePtr;
    float* floatShadowPtr;
};

/**
 * @brief Helper class to manage Temperature_t fields with float shadows for SystemSettings
 * 
 * This class maintains float shadow values for all Temperature_t fields in SystemSettings
 * to enable compatibility with PersistentStorage.
 */
struct SystemSettingsTemperatureShadows {
    // Water heater temperature shadows
    float wHeaterConfTempLimitLow;   // Start heating when tank drops below this
    float wHeaterConfTempLimitHigh;  // Stop heating when tank rises above this
    float wHeaterConfTempSafeLimitHigh;
    float wHeaterConfTempSafeLimitLow;
    
    // Heating temperature shadows
    float targetTemperatureInside;
    float burner_low_limit;
    float burner_high_limit;
    float heating_low_limit;
    float heating_high_limit;
    float water_heating_low_limit;
    float water_heating_high_limit;
    float heating_hysteresis;

    // Sensor offset shadows (MB8ART channels) - int32_t for registerInt (tenths of °C)
    int32_t boilerOutputOffset;
    int32_t boilerReturnOffset;
    int32_t waterTankOffset;
    int32_t waterOutputOffset;
    int32_t waterReturnOffset;
    int32_t heatingReturnOffset;
    int32_t outsideTempOffset;
    // ANDRTF3
    int32_t roomTempOffset;
    // Pressure (hundredths of BAR for registerInt)
    int32_t pressureOffset;
    
    /**
     * @brief Initialize shadows from Temperature_t values
     */
    void initializeFromSettings(const SystemSettings& settings) {
        wHeaterConfTempLimitLow = tempToFloat(settings.wHeaterConfTempLimitLow);
        wHeaterConfTempLimitHigh = tempToFloat(settings.wHeaterConfTempLimitHigh);
        wHeaterConfTempSafeLimitHigh = tempToFloat(settings.wHeaterConfTempSafeLimitHigh);
        wHeaterConfTempSafeLimitLow = tempToFloat(settings.wHeaterConfTempSafeLimitLow);
        targetTemperatureInside = tempToFloat(settings.targetTemperatureInside);
        burner_low_limit = tempToFloat(settings.burner_low_limit);
        burner_high_limit = tempToFloat(settings.burner_high_limit);
        heating_low_limit = tempToFloat(settings.heating_low_limit);
        heating_high_limit = tempToFloat(settings.heating_high_limit);
        water_heating_low_limit = tempToFloat(settings.water_heating_low_limit);
        water_heating_high_limit = tempToFloat(settings.water_heating_high_limit);
        heating_hysteresis = tempToFloat(settings.heating_hysteresis);
        // Sensor offsets - direct int16_t to int32_t (no float conversion needed)
        boilerOutputOffset = static_cast<int32_t>(settings.boilerOutputOffset);
        boilerReturnOffset = static_cast<int32_t>(settings.boilerReturnOffset);
        waterTankOffset = static_cast<int32_t>(settings.waterTankOffset);
        waterOutputOffset = static_cast<int32_t>(settings.waterOutputOffset);
        waterReturnOffset = static_cast<int32_t>(settings.waterReturnOffset);
        heatingReturnOffset = static_cast<int32_t>(settings.heatingReturnOffset);
        outsideTempOffset = static_cast<int32_t>(settings.outsideTempOffset);
        roomTempOffset = static_cast<int32_t>(settings.roomTempOffset);
        pressureOffset = static_cast<int32_t>(settings.pressureOffset);  // Already hundredths of BAR
    }
    
    /**
     * @brief Apply shadow values back to Temperature_t fields
     */
    void applyToSettings(SystemSettings& settings) {
        settings.wHeaterConfTempLimitLow = tempFromFloat(wHeaterConfTempLimitLow);
        settings.wHeaterConfTempLimitHigh = tempFromFloat(wHeaterConfTempLimitHigh);
        settings.wHeaterConfTempSafeLimitHigh = tempFromFloat(wHeaterConfTempSafeLimitHigh);
        settings.wHeaterConfTempSafeLimitLow = tempFromFloat(wHeaterConfTempSafeLimitLow);
        settings.targetTemperatureInside = tempFromFloat(targetTemperatureInside);
        settings.burner_low_limit = tempFromFloat(burner_low_limit);
        settings.burner_high_limit = tempFromFloat(burner_high_limit);
        settings.heating_low_limit = tempFromFloat(heating_low_limit);
        settings.heating_high_limit = tempFromFloat(heating_high_limit);
        settings.water_heating_low_limit = tempFromFloat(water_heating_low_limit);
        settings.water_heating_high_limit = tempFromFloat(water_heating_high_limit);
        settings.heating_hysteresis = tempFromFloat(heating_hysteresis);
        // Sensor offsets - direct int32_t to int16_t (no float conversion needed)
        settings.boilerOutputOffset = static_cast<Temperature_t>(boilerOutputOffset);
        settings.boilerReturnOffset = static_cast<Temperature_t>(boilerReturnOffset);
        settings.waterTankOffset = static_cast<Temperature_t>(waterTankOffset);
        settings.waterOutputOffset = static_cast<Temperature_t>(waterOutputOffset);
        settings.waterReturnOffset = static_cast<Temperature_t>(waterReturnOffset);
        settings.heatingReturnOffset = static_cast<Temperature_t>(heatingReturnOffset);
        settings.outsideTempOffset = static_cast<Temperature_t>(outsideTempOffset);
        settings.roomTempOffset = static_cast<Temperature_t>(roomTempOffset);
        settings.pressureOffset = static_cast<int16_t>(pressureOffset);  // Already hundredths of BAR
    }
};

#endif // TEMPERATURE_PARAMETER_WRAPPER_H