// src/hal/RYN4RelayModule.h
// HAL interface for RYN4 relay module

#pragma once

#include "hal/HardwareAbstractionLayer.h"
#include <RYN4.h>
#include <vector>

namespace HAL {

/**
 * @brief RYN4 relay module HAL implementation
 *
 * This HAL wrapper provides a consistent interface for the RYN4
 * 4-channel relay module via Modbus RTU.
 */
class RYN4RelayModule : public IRelay {
private:
    RYN4* device;
    uint8_t channelCount;
    bool initialized;
    StateChangeCallback stateChangeCallback;
    std::vector<State> lastStates;
    static constexpr const char* TAG = "RYN4HAL";

public:
    /**
     * @brief Construct RYN4 relay module HAL
     * @param ryn4Device Pointer to RYN4 device instance
     * @param channels Number of relay channels (default 4)
     */
    explicit RYN4RelayModule(RYN4* ryn4Device, uint8_t channels = 4);

    /**
     * @brief Initialize the relay module
     * @return true if successful
     */
    bool initialize() override;

    /**
     * @brief Set relay state
     * @param channel Relay channel (0-3)
     * @param state Desired state (ON/OFF)
     * @return true if successful
     */
    bool setState(uint8_t channel, State state) override;

    /**
     * @brief Get relay state
     * @param channel Relay channel (0-3)
     * @return Current state (cached)
     */
    State getState(uint8_t channel) const override;

    /**
     * @brief Get number of relay channels
     * @return Number of channels (typically 4)
     */
    uint8_t getChannelCount() const override;

    /**
     * @brief Register state change callback
     * @param callback Function to call on state change
     */
    void onStateChange(StateChangeCallback callback) override;

    /**
     * @brief Get relay module name
     * @return "RYN4"
     */
    const char* getName() const override;
};

/**
 * @brief Factory function to create RYN4 relay module HAL
 * @param device RYN4 device instance
 * @param channels Number of relay channels
 * @return New RYN4RelayModule instance
 */
IRelay* createRYN4RelayModule(RYN4* device, uint8_t channels = 4);

} // namespace HAL
