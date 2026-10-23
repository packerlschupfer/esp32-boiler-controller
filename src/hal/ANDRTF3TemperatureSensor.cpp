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

// The one ANDRTF3 device, remembered by createANDRTF3Sensor() so getRoomSensorStats() can
// reach the library's counters. There is exactly one room sensor in this system, and the
// tasks hold only an ITemperatureSensor*, which deliberately knows nothing about them.
static andrtf3::ANDRTF3* g_roomSensorDevice = nullptr;

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
    
    // The library's "async" trio is synchronous - confirmed with its maintainer 2026-10-24
    // and now documented as such upstream: requestTemperature() performs the whole
    // transaction, isReadComplete() always returns true, and getAsyncResult() hands back the
    // stored result. So there is nothing to wait for.
    //
    // This used to spin a processAsyncReading() loop with a 250 ms "timeout to allow for
    // Modbus bus contention (library uses 200ms internally)". Both halves were wrong: the
    // loop condition `!device->isReadComplete()` is false immediately, so its body never ran,
    // and the library's bus-mutex timeout is MODBUS_MUTEX_TIMEOUT_MS (2000 ms). The loop and
    // its timeout are gone rather than left looking load-bearing.
    //
    // Why not make it genuinely async: RS485 is half-duplex, so the bus mutex is held for
    // request AND response however the call is shaped. Async would only stop THIS task from
    // blocking - it would not reduce bus occupancy, which was the reason I asked for it.
    if (requestAsyncReading()) {
        andrtf3::ANDRTF3::TemperatureData tempData;
        if (device->getAsyncResult(tempData) && tempData.valid) {
            // Convert from fixed-point format (value * 10) to float
            result.temperature = tempData.celsius / 10.0f;
            result.valid = true;
            result.timestamp = tempData.timestamp;  // millis() of this good read

            LOG_DEBUG(TAG, "Temperature: %.1f°C", result.temperature);
        } else {
            // errorKind is the machine-readable form; the string is kept for the log line.
            LOG_WARN(TAG, "Read failed: %s (kind %u)", tempData.error.c_str(),
                     static_cast<unsigned>(tempData.errorKind));
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

/**
 * @brief Factory function to create ANDRTF3 temperature sensor HAL
 */
ITemperatureSensor* createANDRTF3Sensor(andrtf3::ANDRTF3* device) {
    if (!device) {
        LOG_ERROR("ANDRTF3HAL", "Cannot create HAL with null device");
        return nullptr;
    }

    g_roomSensorDevice = device;
    return new ANDRTF3TemperatureSensor(device);
}

RoomSensorStats getRoomSensorStats() {
    RoomSensorStats stats;
    andrtf3::ANDRTF3* dev = g_roomSensorDevice;
    if (dev == nullptr) {
        return stats;  // available stays false
    }
    stats.available = true;
    for (size_t i = 0; i < RoomSensorStats::KIND_COUNT; ++i) {
        stats.counts[i] = dev->getReadErrorCount(static_cast<andrtf3::ANDRTF3::ReadError>(i));
    }
    stats.lastGoodAgeMs = dev->getLastGoodReadAgeMs();
    stats.lastKind = static_cast<uint8_t>(dev->getLastReadError());
    return stats;
}

} // namespace HAL