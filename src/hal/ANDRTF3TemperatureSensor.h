// src/hal/ANDRTF3TemperatureSensor.h
// HAL interface for ANDRTF3 temperature sensor

#pragma once

#include "hal/HardwareAbstractionLayer.h"
#include <ANDRTF3.h>

namespace HAL {

/**
 * @brief ANDRTF3 temperature sensor HAL implementation
 * 
 * This HAL wrapper provides a consistent interface for the ANDRTF3
 * wall-mount RS485 temperature sensor. The sensor provides temperature
 * readings in fixed-point format (value * 10).
 */
class ANDRTF3TemperatureSensor : public ITemperatureSensor {
private:
    andrtf3::ANDRTF3* device;
    bool initialized;
    static constexpr const char* TAG = "ANDRTF3HAL";
    
public:
    /**
     * @brief Construct ANDRTF3 temperature sensor HAL
     * @param andrtf3Device Pointer to ANDRTF3 device instance
     */
    explicit ANDRTF3TemperatureSensor(andrtf3::ANDRTF3* andrtf3Device);
    
    /**
     * @brief Initialize the sensor
     * @return true if successful
     */
    bool initialize() override;
    
    /**
     * @brief Read temperature from sensor
     * @param channel Sensor channel (ignored for ANDRTF3 - single channel)
     * @return Temperature reading with validity flag
     */
    Reading readTemperature(uint8_t channel = 0) override;
    
    /**
     * @brief Get number of available channels
     * @return Always returns 1 for ANDRTF3
     */
    uint8_t getChannelCount() const override;
    
    /**
     * @brief Check if sensor is ready
     * @return true if sensor is initialized and connected
     */
    bool isReady() const override;
    
    /**
     * @brief Get sensor name
     * @return "ANDRTF3"
     */
    const char* getName() const override;
    
private:
    /**
     * @brief Request asynchronous temperature reading
     * @return true if request was sent successfully
     */
    bool requestAsyncReading();
    
    /**
     * @brief Process pending asynchronous operations
     * @param maxWaitMs Maximum time to wait for response
     * @return true if reading completed successfully
     */
    bool processAsyncReading(uint32_t maxWaitMs = 100);
};

/**
 * @brief Factory function to create ANDRTF3 temperature sensor HAL
 * @param device ANDRTF3 device instance
 * @return New ANDRTF3TemperatureSensor instance
 */
ITemperatureSensor* createANDRTF3Sensor(andrtf3::ANDRTF3* device);

} // namespace HAL