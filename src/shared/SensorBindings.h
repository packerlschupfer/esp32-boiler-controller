#pragma once
#include <array>
#include "config/SensorIndices.h"
#include "shared/Temperature.h"
#include "MB8ART.h"

namespace SensorBindings {

    /**
     * @brief Runtime sensor pointer bindings (lives in RAM)
     *
     * This array is initialized once at startup and connects the logical
     * SensorIndex constants to their corresponding Temperature_t and validity
     * pointers in SharedSensorReadings.
     */
    extern std::array<mb8art::SensorBinding, 8> bindings;

    /**
     * @brief Initialize pointer bindings based on SensorIndex assignments
     *
     * This function MUST be called during system initialization before
     * any sensor operations. It connects the SensorIndex constants to the
     * actual SharedSensorReadings struct members.
     */
    void initialize();

    /**
     * @brief Get temperature pointer for a sensor
     * @param index Logical sensor index (use SensorIndex constants)
     * @return Pointer to Temperature_t value
     */
    inline Temperature_t* getTemperaturePtr(uint8_t index) {
        return bindings[index].temperaturePtr;
    }

    /**
     * @brief Get validity pointer for a sensor
     * @param index Logical sensor index (use SensorIndex constants)
     * @return Pointer to validity bool
     */
    inline bool* getValidityPtr(uint8_t index) {
        return bindings[index].validityPtr;
    }

    /**
     * @brief Get the entire binding array for binding to MB8ART
     * @return Reference to binding array
     */
    inline const std::array<mb8art::SensorBinding, 8>& getBindingArray() {
        return bindings;
    }
}

namespace ANDRTF3Bindings {
    /**
     * @brief ANDRTF3 sensor bindings (simple - just 2 sensors)
     */
    extern Temperature_t* insideTempPtr;
    extern bool* insideTempValidPtr;
    extern float* insideHumidityPtr;
    extern bool* insideHumidityValidPtr;

    /**
     * @brief Initialize ANDRTF3 pointer bindings
     */
    void initialize();
}
