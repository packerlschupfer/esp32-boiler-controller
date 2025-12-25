// src/hal/HardwareFactory.cpp
// Factory for creating HAL implementations

#include "hal/HardwareAbstractionLayer.h"
#include "core/SystemResourceProvider.h"
#include "LoggingMacros.h"
#include <ANDRTF3.h>
#include <RYN4.h>
#include <MB8ART.h>

// Forward declarations
class DS3231Controller;

// Forward declarations of factory functions
namespace HAL {
    ITemperatureSensor* createMB8ARTSensor(MB8ART* device, uint8_t channels);
    ITemperatureSensor* createANDRTF3Sensor(andrtf3::ANDRTF3* device);
    IRelay* createRYN4RelayModule(RYN4* device, uint8_t channels);
    IRTC* createDS3231RTC(DS3231Controller* device);
    bool configureMB8ARTHAL(MB8ART* mb8art);
    bool configureANDRTF3HAL(andrtf3::ANDRTF3* andrtf3);
    bool configureRYN4HAL(RYN4* ryn4);
}

namespace HAL {

// Static instance
HardwareAbstractionLayer* HardwareAbstractionLayer::instance = nullptr;

// Guard to prevent duplicate configuration
static bool halConfigured = false;

/**
 * @brief Configure HAL with actual hardware implementations
 *
 * This function should be called during system initialization
 * after all hardware devices are created.
 */
bool configureHardwareAbstractionLayer(MB8ART* mb8art, RYN4* ryn4, DS3231Controller* rtc) {
    static const char* TAG = "HALConfig";

    // Prevent duplicate configuration
    if (halConfigured) {
        LOG_DEBUG(TAG, "HAL already configured - skipping");
        return true;
    }

    LOG_INFO(TAG, "Configuring Hardware Abstraction Layer");

    auto& hal = HardwareAbstractionLayer::getInstance();
    HardwareConfig config = hal.getConfig();  // Preserve existing config (e.g., roomTempSensor)

    // Create temperature sensor HAL (skip if already configured by individual HAL config)
    if (mb8art && !config.boilerTempSensor) {
        // MB8ART has 8 channels - map them according to system design
        // Channel 0-1: Boiler temps
        // Channel 2-3: Water heater temps
        // Channel 4: Outside temp
        // Channel 5-6: Room temps (ANDRTF3 sensors)
        // Channel 7: Spare

        config.boilerTempSensor = createMB8ARTSensor(mb8art, 8);
        config.waterTempSensor = config.boilerTempSensor;  // Same device, different channels
        config.outsideTempSensor = config.boilerTempSensor;  // Same device, different channels

        LOG_INFO(TAG, "Configured MB8ART temperature sensors");
    } else if (!mb8art) {
        LOG_WARN(TAG, "No MB8ART device provided");
    }

    // Create relay HAL (skip if already configured by individual HAL config)
    if (ryn4 && !config.burnerRelay) {
        // RYN4 has 4 relays - map them according to system design
        // Relay 0: Burner control
        // Relay 1: Heating pump
        // Relay 2: Water heater pump
        // Relay 3: Spare/auxiliary

        config.burnerRelay = createRYN4RelayModule(ryn4, 4);
        config.pumpRelay = config.burnerRelay;  // Same device, different channels

        LOG_INFO(TAG, "Configured RYN4 relay module");
    } else if (!ryn4) {
        LOG_WARN(TAG, "No RYN4 device provided");
    }

    // Create RTC HAL (skip if already configured)
    if (rtc && !config.rtc) {
        config.rtc = createDS3231RTC(rtc);
        LOG_INFO(TAG, "Configured DS3231 RTC");
    } else if (!rtc) {
        LOG_WARN(TAG, "No DS3231 device provided");
    }
    
    // Configure the HAL
    hal.configure(config);
    
    // Initialize all hardware through HAL
    bool success = hal.initializeAll();
    
    if (success) {
        halConfigured = true;
        LOG_INFO(TAG, "Hardware Abstraction Layer initialized successfully");
    } else {
        LOG_ERROR(TAG, "Failed to initialize some hardware components");
    }

    return success;
}

/**
 * @brief Configure HAL with actual hardware implementations including ANDRTF3
 *
 * This overloaded function includes support for ANDRTF3 room temperature sensor.
 * Should be called during system initialization after all hardware devices are created.
 */
bool configureHardwareAbstractionLayer(MB8ART* mb8art, RYN4* ryn4, DS3231Controller* rtc,
                                     andrtf3::ANDRTF3* andrtf3) {
    static const char* TAG = "HALConfig";

    // First configure the base hardware
    bool success = configureHardwareAbstractionLayer(mb8art, ryn4, rtc);

    // Then add ANDRTF3 if provided (and not already configured)
    if (andrtf3) {
        success = configureANDRTF3HAL(andrtf3) && success;
    } else {
        LOG_WARN(TAG, "No ANDRTF3 device provided");
    }

    return success;
}

/**
 * @brief Configure just the MB8ART portion of HAL
 *
 * This function can be called independently to configure MB8ART HAL
 * as soon as the device is ready, without waiting for other devices.
 * This ensures temperature sensor HAL is available even if RYN4 fails.
 */
bool configureMB8ARTHAL(MB8ART* mb8art) {
    static const char* TAG = "MB8ARTHAL";

    if (!mb8art) {
        LOG_WARN(TAG, "No MB8ART device provided");
        return false;
    }

    auto& hal = HardwareAbstractionLayer::getInstance();
    HardwareConfig config = hal.getConfig();

    // Check if already configured
    if (config.boilerTempSensor) {
        LOG_DEBUG(TAG, "MB8ART HAL already configured - skipping");
        return true;
    }

    // Create MB8ART sensor HAL
    // MB8ART has 8 channels - map them according to system design
    // Channel 0-1: Boiler temps
    // Channel 2-3: Water heater temps
    // Channel 4: Outside temp
    // Channel 5-6: Room temps (ANDRTF3 sensors)
    // Channel 7: Spare
    config.boilerTempSensor = createMB8ARTSensor(mb8art, 8);
    config.waterTempSensor = config.boilerTempSensor;   // Same device, different channels
    config.outsideTempSensor = config.boilerTempSensor; // Same device, different channels

    if (config.boilerTempSensor) {
        // Re-configure HAL with updated config
        hal.configure(config);

        // Initialize the sensor
        if (config.boilerTempSensor->initialize()) {
            LOG_INFO(TAG, "MB8ART HAL initialized for address 0x%02X", mb8art->getServerAddress());
            return true;
        } else {
            LOG_ERROR(TAG, "Failed to initialize MB8ART sensor");
            return false;
        }
    } else {
        LOG_ERROR(TAG, "Failed to create MB8ART HAL");
        return false;
    }
}

/**
 * @brief Configure just the ANDRTF3 portion of HAL
 *
 * This function can be called independently to configure ANDRTF3 HAL
 * as soon as the device is ready, without waiting for other devices.
 * This prevents timing issues where ANDRTF3Task starts before HAL is configured.
 */
bool configureANDRTF3HAL(andrtf3::ANDRTF3* andrtf3) {
    static const char* TAG = "ANDRTF3HAL";

    if (!andrtf3) {
        LOG_WARN(TAG, "No ANDRTF3 device provided");
        return false;
    }

    auto& hal = HardwareAbstractionLayer::getInstance();
    HardwareConfig config = hal.getConfig();

    // Check if already configured
    if (config.roomTempSensor) {
        LOG_DEBUG(TAG, "ANDRTF3 HAL already configured - skipping");
        return true;
    }

    // Create ANDRTF3 sensor HAL for room temperature
    config.roomTempSensor = createANDRTF3Sensor(andrtf3);

    if (config.roomTempSensor) {
        // Re-configure HAL with updated config
        hal.configure(config);

        // Initialize the new sensor (initialize() logs its own message)
        if (config.roomTempSensor->initialize()) {
            return true;
        } else {
            LOG_ERROR(TAG, "Failed to initialize ANDRTF3 sensor");
            return false;
        }
    } else {
        LOG_ERROR(TAG, "Failed to create ANDRTF3 HAL");
        return false;
    }
}

/**
 * @brief Configure just the RYN4 portion of HAL
 *
 * This function can be called independently to configure RYN4 HAL
 * as soon as the device is ready, without waiting for other devices.
 * This ensures relay control via HAL is available even if other devices fail.
 */
bool configureRYN4HAL(RYN4* ryn4) {
    static const char* TAG = "RYN4HAL";

    if (!ryn4) {
        LOG_WARN(TAG, "No RYN4 device provided");
        return false;
    }

    auto& hal = HardwareAbstractionLayer::getInstance();
    HardwareConfig config = hal.getConfig();

    // Check if already configured
    if (config.burnerRelay) {
        LOG_DEBUG(TAG, "RYN4 HAL already configured - skipping");
        return true;
    }

    // Create RYN4 relay HAL
    // RYN4 has 4 relays - map them according to system design
    // Relay 0: Burner control
    // Relay 1: Heating pump
    // Relay 2: Water heater pump
    // Relay 3: Spare/auxiliary
    config.burnerRelay = createRYN4RelayModule(ryn4, 4);
    config.pumpRelay = config.burnerRelay;  // Same device, different channels

    if (config.burnerRelay) {
        // Re-configure HAL with updated config
        hal.configure(config);

        // Initialize the relay module
        if (config.burnerRelay->initialize()) {
            LOG_INFO(TAG, "RYN4 HAL initialized for address 0x%02X", ryn4->getServerAddress());
            return true;
        } else {
            LOG_ERROR(TAG, "Failed to initialize RYN4 relay module");
            return false;
        }
    } else {
        LOG_ERROR(TAG, "Failed to create RYN4 HAL");
        return false;
    }
}

/**
 * @brief Example of using HAL to read temperatures
 */
void readTemperaturesThroughHAL() {
    static const char* TAG = "HALExample";
    
    auto& hal = HardwareAbstractionLayer::getInstance();
    const auto& config = hal.getConfig();
    
    if (config.boilerTempSensor) {
        // Read boiler output temperature (channel 0)
        auto reading = config.boilerTempSensor->readTemperature(0);
        if (reading.valid) {
            LOG_INFO(TAG, "Boiler output temp: %.1f°C", reading.temperature);
        }
        
        // Read boiler return temperature (channel 1)
        reading = config.boilerTempSensor->readTemperature(1);
        if (reading.valid) {
            LOG_INFO(TAG, "Boiler return temp: %.1f°C", reading.temperature);
        }
    }
    
    if (config.waterTempSensor) {
        // Read water tank temperature (channel 2)
        auto reading = config.waterTempSensor->readTemperature(2);
        if (reading.valid) {
            LOG_INFO(TAG, "Water tank temp: %.1f°C", reading.temperature);
        }
    }
    
    if (config.outsideTempSensor) {
        // Read outside temperature (channel 4)
        auto reading = config.outsideTempSensor->readTemperature(4);
        if (reading.valid) {
            LOG_INFO(TAG, "Outside temp: %.1f°C", reading.temperature);
        }
    }
    
    if (config.roomTempSensor) {
        // Read room temperature from ANDRTF3
        auto reading = config.roomTempSensor->readTemperature();
        if (reading.valid) {
            LOG_INFO(TAG, "Room temp: %.1f°C", reading.temperature);
        }
    }
}

/**
 * @brief Example of using HAL to control relays
 */
void controlRelaysThroughHAL(bool burnerOn, bool heatingPumpOn, bool waterPumpOn) {
    static const char* TAG = "HALExample";
    
    auto& hal = HardwareAbstractionLayer::getInstance();
    const auto& config = hal.getConfig();
    
    if (config.burnerRelay) {
        // Control burner (relay 0)
        IRelay::State burnerState = burnerOn ? IRelay::State::ON : IRelay::State::OFF;
        if (config.burnerRelay->setState(0, burnerState)) {
            LOG_INFO(TAG, "Burner set to %s", burnerOn ? "ON" : "OFF");
        }
        
        // Control heating pump (relay 1)
        IRelay::State heatingPumpState = heatingPumpOn ? IRelay::State::ON : IRelay::State::OFF;
        if (config.burnerRelay->setState(1, heatingPumpState)) {
            LOG_INFO(TAG, "Heating pump set to %s", heatingPumpOn ? "ON" : "OFF");
        }
        
        // Control water pump (relay 2)
        IRelay::State waterPumpState = waterPumpOn ? IRelay::State::ON : IRelay::State::OFF;
        if (config.burnerRelay->setState(2, waterPumpState)) {
            LOG_INFO(TAG, "Water pump set to %s", waterPumpOn ? "ON" : "OFF");
        }
    }
}

/**
 * @brief Example of using HAL with callbacks
 */
void setupHALCallbacks() {
    // static const char* TAG = "HALExample";  // Unused in this example
    
    auto& hal = HardwareAbstractionLayer::getInstance();
    const auto& config = hal.getConfig();
    
    // Set up relay state change callback
    if (config.burnerRelay) {
        config.burnerRelay->onStateChange([](uint8_t channel, IRelay::State newState) {
            const char* stateStr = (newState == IRelay::State::ON) ? "ON" : "OFF";
            LOG_INFO("HALCallback", "Relay %u changed to %s", channel, stateStr);
            
            // Could trigger events or update shared state here
            switch(channel) {
                case 0:  // Burner
                    // Update burner state in shared resources
                    break;
                case 1:  // Heating pump
                    // Update heating pump state
                    break;
                case 2:  // Water pump
                    // Update water pump state
                    break;
            }
        });
    }
}

} // namespace HAL