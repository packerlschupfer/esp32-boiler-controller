// src/hal/MB8ARTTemperatureSensor.cpp
// HAL implementation for MB8ART temperature sensor

#include "hal/MB8ARTTemperatureSensor.h"
#include "LoggingMacros.h"
#include "core/SystemResourceProvider.h"

namespace HAL {

MB8ARTTemperatureSensor::MB8ARTTemperatureSensor(MB8ART* mb8artDevice, uint8_t channels)
    : device(mb8artDevice), channelCount(channels), initialized(false) {}

bool MB8ARTTemperatureSensor::initialize() {
    // Skip if already initialized
    if (initialized) {
        return true;
    }

    if (!device) {
        LOG_ERROR(TAG, "Device pointer is null");
        return false;
    }

    // MB8ART is initialized elsewhere, just check if it's ready
    if (device->isInitialized()) {
        initialized = true;
        LOG_INFO(TAG, "MB8ART HAL initialized with %u channels", channelCount);
        return true;
    }

    LOG_WARN(TAG, "MB8ART device not yet initialized");
    return false;
}

ITemperatureSensor::Reading MB8ARTTemperatureSensor::readTemperature(uint8_t channel) {
    Reading result = {0.0f, false, 0};

    if (!initialized || !device || channel >= channelCount) {
        return result;
    }

    // Get all temperatures from device
    auto deviceResult = device->getData(IDeviceInstance::DeviceDataType::TEMPERATURE);

    if (deviceResult.isOk() &&
        !deviceResult.value().empty() &&
        channel < deviceResult.value().size()) {

        result.temperature = deviceResult.value()[channel];
        result.valid = true;
        result.timestamp = millis();
    }

    return result;
}

uint8_t MB8ARTTemperatureSensor::getChannelCount() const {
    return channelCount;
}

bool MB8ARTTemperatureSensor::isReady() const {
    return initialized && device && device->isInitialized();
}

const char* MB8ARTTemperatureSensor::getName() const {
    return "MB8ART";
}

ITemperatureSensor* createMB8ARTSensor(MB8ART* device, uint8_t channels) {
    return new MB8ARTTemperatureSensor(device, channels);
}

} // namespace HAL