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

// Removed 2026-10-19: readTemperaturesThroughHAL() and controlRelaysThroughHAL(),
// two "Example of using HAL to ..." functions. Both were dead - defined here, declared
// in no header, zero callers - but the relay one was worse than dead. It drove
// burnerRelay->setState() directly, bypassing BurnerRequestManager, BurnerAntiFlapping,
// the rate limiter and RelayCommandPolicy's pump protection, and its relay indices were
// wrong against RelayIndices.h: it labelled index 1 "heating pump" (POWER_BOOST, full
// 42.2 kW) and index 2 "water pump" (WATER_MODE), while the real pumps are 4 and 5.
// Copied as the "example" it claimed to be, it would fire the burner at full power with
// no pump running - the Sept 2026 overheat. The temperature one read channel 4 as
// "outside"; channel 4 is the 4-20 mA pressure input and outside is CH3.

// setupHALCallbacks() removed in the same pass: also dead, also an "example", and it
// repeated the same wrong relay labels in its switch (case 1 "Heating pump",
// case 2 "Water pump") with empty bodies.

} // namespace HAL
