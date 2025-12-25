// src/hal/RYN4RelayModule.cpp
// HAL implementation for RYN4 relay module

#include "hal/RYN4RelayModule.h"
#include "LoggingMacros.h"

namespace HAL {

RYN4RelayModule::RYN4RelayModule(RYN4* ryn4Device, uint8_t channels)
    : device(ryn4Device), channelCount(channels), initialized(false) {
    lastStates.resize(channels, State::UNKNOWN);
}

bool RYN4RelayModule::initialize() {
    // Guard against double initialization
    if (initialized) {
        return true;
    }

    if (!device) {
        LOG_ERROR(TAG, "Device pointer is null");
        return false;
    }

    // RYN4 is initialized elsewhere, just check if it's ready
    if (device->isInitialized()) {
        initialized = true;
        LOG_INFO(TAG, "RYN4 HAL initialized with %u channels", channelCount);

        // Initialize all relays to OFF as safe default
        // We'll track state as we control the relays
        std::fill(lastStates.begin(), lastStates.end(), State::OFF);
        LOG_INFO(TAG, "Initialized all relays to OFF state");

        return true;
    }

    LOG_WARN(TAG, "RYN4 device not yet initialized");
    return false;
}

bool RYN4RelayModule::setState(uint8_t channel, State state) {
    if (!initialized || !device || channel >= channelCount) {
        return false;
    }

    // RYN4 uses 1-based relay indexing
    uint8_t relayIndex = channel + 1;

    // Use DELAY-safe methods (cancel any active DELAY timers)
    ryn4::RelayErrorCode result;
    if (state == State::ON) {
        result = device->turnOnRelay(relayIndex);  // DELAY 0 + ON
    } else {
        result = device->turnOffRelay(relayIndex);  // DELAY 0
    }

    if (result == ryn4::RelayErrorCode::SUCCESS) {
        // Check if state actually changed
        if (lastStates[channel] != state) {
            lastStates[channel] = state;

            // Notify callback if registered
            if (stateChangeCallback) {
                stateChangeCallback(channel, state);
            }
        }
        return true;
    }

    LOG_ERROR(TAG, "Failed to set relay %u to %s",
              channel, (state == State::ON) ? "ON" : "OFF");
    return false;
}

IRelay::State RYN4RelayModule::getState(uint8_t channel) const {
    if (!initialized || !device || channel >= channelCount) {
        return State::UNKNOWN;
    }

    // Return cached state - RYN4 library API issue prevents direct read
    return lastStates[channel];
}

uint8_t RYN4RelayModule::getChannelCount() const {
    return channelCount;
}

void RYN4RelayModule::onStateChange(StateChangeCallback callback) {
    stateChangeCallback = callback;
}

const char* RYN4RelayModule::getName() const {
    return "RYN4";
}

IRelay* createRYN4RelayModule(RYN4* device, uint8_t channels) {
    return new RYN4RelayModule(device, channels);
}

} // namespace HAL
