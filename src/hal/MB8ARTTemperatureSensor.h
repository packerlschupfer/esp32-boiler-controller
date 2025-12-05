// src/hal/MB8ARTTemperatureSensor.h
// HAL interface for MB8ART temperature sensor

#pragma once

#include "hal/HardwareAbstractionLayer.h"
#include <MB8ART.h>

namespace HAL {

/**
 * @brief MB8ART temperature sensor HAL implementation
 *
 * This HAL wrapper provides a consistent interface for the MB8ART
 * 8-channel PT1000 temperature sensor module via Modbus RTU.
 */
class MB8ARTTemperatureSensor : public ITemperatureSensor {
private:
    MB8ART* device;
    uint8_t channelCount;
    bool initialized;
    static constexpr const char* TAG = "MB8ARTHAL";

public:
    /**
     * @brief Construct MB8ART temperature sensor HAL
     * @param mb8artDevice Pointer to MB8ART device instance
     * @param channels Number of active channels (default 8)
     */
    explicit MB8ARTTemperatureSensor(MB8ART* mb8artDevice, uint8_t channels = 8);

    /**
     * @brief Initialize the sensor
     * @return true if successful
     */
    bool initialize() override;

    /**
     * @brief Read temperature from sensor
     * @param channel Sensor channel (0-7)
     * @return Temperature reading with validity flag
     */
    Reading readTemperature(uint8_t channel = 0) override;

    /**
     * @brief Get number of available channels
     * @return Number of configured channels (up to 8)
     */
    uint8_t getChannelCount() const override;

    /**
     * @brief Check if sensor is ready
     * @return true if sensor is initialized and connected
     */
    bool isReady() const override;

    /**
     * @brief Get sensor name
     * @return "MB8ART"
     */
    const char* getName() const override;
};

/**
 * @brief Factory function to create MB8ART temperature sensor HAL
 * @param device MB8ART device instance
 * @param channels Number of active channels
 * @return New MB8ARTTemperatureSensor instance
 */
ITemperatureSensor* createMB8ARTSensor(MB8ART* device, uint8_t channels = 8);

} // namespace HAL
