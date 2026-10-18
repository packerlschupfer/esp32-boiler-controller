// src/hal/ANDRTF3TemperatureSensor.cpp
// HAL implementation for ANDRTF3 temperature sensor

#include "hal/ANDRTF3TemperatureSensor.h"
#include "core/SystemResourceProvider.h"
#include "config/SystemConstants.h"
#include "utils/Utils.h"
#include <TaskManager.h>
#include "LoggingMacros.h"
#include <Arduino.h>

namespace HAL {

ANDRTF3TemperatureSensor::ANDRTF3TemperatureSensor(andrtf3::ANDRTF3* andrtf3Device) 
    : device(andrtf3Device), initialized(false) {
}

bool ANDRTF3TemperatureSensor::initialize() {
    // Guard against double initialization
    if (initialized) {
        return true;
    }

    if (!device) {
        LOG_ERROR(TAG, "Device pointer is null");
        return false;
    }

    // ANDRTF3 doesn't have an explicit initialization method
    // Just check if device is ready for communication
    initialized = true;
    LOG_INFO(TAG, "ANDRTF3 HAL initialized for address %u", device->getDeviceAddress());

    return true;
}

ITemperatureSensor::Reading ANDRTF3TemperatureSensor::readTemperature(uint8_t channel) {
    Reading result = {0.0f, false, 0};
    
    // ANDRTF3 is single-channel, ignore channel parameter
    if (channel > 0) {
        LOG_WARN(TAG, "Channel %u requested but ANDRTF3 only has 1 channel", channel);
    }
    
    if (!initialized || !device) {
        LOG_ERROR(TAG, "Sensor not initialized");
        return result;
    }
    
    // Try asynchronous reading first for better responsiveness
    // Use 250ms timeout to allow for Modbus bus contention (library uses 200ms internally)
    static constexpr uint32_t ASYNC_TIMEOUT_MS = 250;

    if (requestAsyncReading()) {
        if (processAsyncReading(ASYNC_TIMEOUT_MS)) {
            andrtf3::ANDRTF3::TemperatureData tempData;
            if (device->getAsyncResult(tempData) && tempData.valid) {
                // Convert from fixed-point format (value * 10) to float
                result.temperature = tempData.celsius / 10.0f;
                result.valid = true;
                result.timestamp = tempData.timestamp;

                LOG_DEBUG(TAG, "Temperature: %.1f°C", result.temperature);
            } else {
                LOG_WARN(TAG, "Async read failed: %s", tempData.error.c_str());
            }
        } else {
            LOG_WARN(TAG, "Async read timeout");
        }
    }
    // No retry - if read failed, wait for next coordinator tick (5s)
    
    return result;
}

uint8_t ANDRTF3TemperatureSensor::getChannelCount() const {
    return 1;  // ANDRTF3 is a single-channel sensor
}

bool ANDRTF3TemperatureSensor::isReady() const {
    return initialized && device && device->isConnected();
}

const char* ANDRTF3TemperatureSensor::getName() const {
    return "ANDRTF3";
}

bool ANDRTF3TemperatureSensor::requestAsyncReading() {
    if (!device) {
        return false;
    }
    
    // Process any pending operations first
    device->process();
    
    // Check if a previous read is still pending
    if (!device->isReadComplete()) {
        LOG_DEBUG(TAG, "Previous async read still pending");
        return false;
    }
    
    // Request new temperature reading
    return device->requestTemperature();
}

bool ANDRTF3TemperatureSensor::processAsyncReading(uint32_t maxWaitMs) {
    if (!device) {
        return false;
    }

    uint32_t startTime = millis();
    uint32_t processCount = 0;
    const uint32_t MAX_PROCESS_ITERATIONS = maxWaitMs / 5;  // Process every 5ms

    while (!device->isReadComplete() &&
           Utils::elapsedMs(startTime) < maxWaitMs &&
           processCount < MAX_PROCESS_ITERATIONS) {
        device->process();
        vTaskDelay(pdMS_TO_TICKS(5));
        processCount++;

        // Feed watchdog every 50ms during async wait (every 10 iterations)
        if (processCount % 10 == 0) {
            (void)SRP::getTaskManager().feedWatchdog();
        }
    }

    if (processCount >= MAX_PROCESS_ITERATIONS) {
        LOG_DEBUG(TAG, "Reached max process iterations (%lu)", processCount);
    }

    return device->isReadComplete();
}

/**
 * @brief Factory function to create ANDRTF3 temperature sensor HAL
 */
ITemperatureSensor* createANDRTF3Sensor(andrtf3::ANDRTF3* device) {
    if (!device) {
        LOG_ERROR("ANDRTF3HAL", "Cannot create HAL with null device");
        return nullptr;
    }
    
    return new ANDRTF3TemperatureSensor(device);
}

} // namespace HAL