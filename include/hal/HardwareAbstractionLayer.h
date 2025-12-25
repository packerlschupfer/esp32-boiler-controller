// include/hal/HardwareAbstractionLayer.h
#pragma once

#include <cstdint>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

/**
 * @brief Hardware Abstraction Layer for ESPlan Boiler Controller
 * 
 * This HAL provides a consistent interface to hardware components,
 * making the system more portable and testable.
 */

namespace HAL {

// Forward declarations
class ITemperatureSensor;
class IRelay;
class IDigitalInput;
class IAnalogInput;
class IRTC;
class IWatchdog;
class INetwork;

/**
 * @brief Temperature sensor interface
 */
class ITemperatureSensor {
public:
    struct Reading {
        float temperature;    // Temperature in Celsius
        bool valid;          // True if reading is valid
        uint32_t timestamp;  // Timestamp in milliseconds
    };

    virtual ~ITemperatureSensor() = default;
    
    /**
     * @brief Initialize the sensor
     * @return true if successful
     */
    virtual bool initialize() = 0;
    
    /**
     * @brief Read temperature from sensor
     * @param channel Sensor channel (for multi-channel sensors)
     * @return Temperature reading
     */
    virtual Reading readTemperature(uint8_t channel = 0) = 0;
    
    /**
     * @brief Get number of available channels
     * @return Number of channels
     */
    virtual uint8_t getChannelCount() const = 0;
    
    /**
     * @brief Check if sensor is ready
     * @return true if sensor is ready for readings
     */
    virtual bool isReady() const = 0;
    
    /**
     * @brief Get sensor name
     * @return Sensor identification string
     */
    virtual const char* getName() const = 0;
};

/**
 * @brief Relay control interface
 */
class IRelay {
public:
    enum class State {
        OFF = 0,
        ON = 1,
        UNKNOWN = 2
    };
    
    using StateChangeCallback = std::function<void(uint8_t channel, State newState)>;
    
    virtual ~IRelay() = default;
    
    /**
     * @brief Initialize the relay module
     * @return true if successful
     */
    virtual bool initialize() = 0;
    
    /**
     * @brief Set relay state
     * @param channel Relay channel
     * @param state Desired state
     * @return true if successful
     */
    virtual bool setState(uint8_t channel, State state) = 0;
    
    /**
     * @brief Get relay state
     * @param channel Relay channel
     * @return Current state
     */
    virtual State getState(uint8_t channel) const = 0;
    
    /**
     * @brief Get number of relay channels
     * @return Number of channels
     */
    virtual uint8_t getChannelCount() const = 0;
    
    /**
     * @brief Register state change callback
     * @param callback Function to call on state change
     */
    virtual void onStateChange(StateChangeCallback callback) = 0;
    
    /**
     * @brief Get relay module name
     * @return Module identification string
     */
    virtual const char* getName() const = 0;
};

/**
 * @brief Digital input interface (for switches, buttons, etc.)
 */
class IDigitalInput {
public:
    using InputChangeCallback = std::function<void(uint8_t pin, bool state)>;
    
    virtual ~IDigitalInput() = default;
    
    /**
     * @brief Initialize digital input
     * @param pin GPIO pin number
     * @param pullUp Enable internal pull-up
     * @return true if successful
     */
    virtual bool initialize(uint8_t pin, bool pullUp = true) = 0;
    
    /**
     * @brief Read current state
     * @return true if HIGH, false if LOW
     */
    virtual bool read() const = 0;
    
    /**
     * @brief Register change callback
     * @param callback Function to call on state change
     */
    virtual void onChange(InputChangeCallback callback) = 0;
};

/**
 * @brief Real-Time Clock interface
 */
class IRTC {
public:
    struct DateTime {
        uint16_t year;
        uint8_t month;
        uint8_t day;
        uint8_t hour;
        uint8_t minute;
        uint8_t second;
        uint8_t dayOfWeek;  // 0 = Sunday
    };
    
    virtual ~IRTC() = default;
    
    /**
     * @brief Initialize RTC
     * @return true if successful
     */
    virtual bool initialize() = 0;
    
    /**
     * @brief Get current date/time
     * @return Current date/time
     */
    virtual DateTime getDateTime() = 0;
    
    /**
     * @brief Set date/time
     * @param dt New date/time
     * @return true if successful
     */
    virtual bool setDateTime(const DateTime& dt) = 0;
    
    /**
     * @brief Check if RTC has lost power
     * @return true if power was lost
     */
    virtual bool hasLostPower() = 0;
    
    /**
     * @brief Get RTC temperature (if available)
     * @return Temperature in Celsius, or NaN if not available
     */
    virtual float getTemperature() = 0;
};

/**
 * @brief Watchdog interface
 */
class IWatchdog {
public:
    virtual ~IWatchdog() = default;
    
    /**
     * @brief Initialize watchdog
     * @param timeoutMs Timeout in milliseconds
     * @return true if successful
     */
    virtual bool initialize(uint32_t timeoutMs) = 0;
    
    /**
     * @brief Feed/reset the watchdog
     */
    virtual void feed() = 0;
    
    /**
     * @brief Enable or disable watchdog
     * @param enable true to enable, false to disable
     */
    virtual void setEnabled(bool enable) = 0;
    
    /**
     * @brief Check if watchdog is enabled
     * @return true if enabled
     */
    virtual bool isEnabled() const = 0;
};

/**
 * @brief Network interface
 */
class INetwork {
public:
    enum class State {
        DISCONNECTED,
        CONNECTING,
        CONNECTED,
        ERROR
    };
    
    using StateChangeCallback = std::function<void(State newState)>;
    
    virtual ~INetwork() = default;
    
    /**
     * @brief Initialize network interface
     * @return true if successful
     */
    virtual bool initialize() = 0;
    
    /**
     * @brief Connect to network
     * @return true if successful
     */
    virtual bool connect() = 0;
    
    /**
     * @brief Disconnect from network
     */
    virtual void disconnect() = 0;
    
    /**
     * @brief Get current connection state
     * @return Current state
     */
    virtual State getState() const = 0;
    
    /**
     * @brief Check if connected
     * @return true if connected
     */
    virtual bool isConnected() const = 0;
    
    /**
     * @brief Get IP address
     * @return IP address as string
     */
    virtual const char* getIPAddress() const = 0;
    
    /**
     * @brief Register state change callback
     * @param callback Function to call on state change
     */
    virtual void onStateChange(StateChangeCallback callback) = 0;
};

/**
 * @brief Hardware configuration structure
 */
struct HardwareConfig {
    // Temperature sensors
    ITemperatureSensor* boilerTempSensor = nullptr;
    ITemperatureSensor* waterTempSensor = nullptr;
    ITemperatureSensor* outsideTempSensor = nullptr;
    ITemperatureSensor* roomTempSensor = nullptr;
    
    // Relay modules
    IRelay* burnerRelay = nullptr;
    IRelay* pumpRelay = nullptr;
    
    // Digital inputs
    IDigitalInput* flameSensor = nullptr;
    IDigitalInput* emergencyStop = nullptr;
    
    // System components
    IRTC* rtc = nullptr;
    IWatchdog* watchdog = nullptr;
    INetwork* network = nullptr;
};

/**
 * @brief Hardware abstraction layer singleton
 */
class HardwareAbstractionLayer {
private:
    static HardwareAbstractionLayer* instance;
    HardwareConfig config;
    SemaphoreHandle_t mutex;
    
    HardwareAbstractionLayer() {
        mutex = xSemaphoreCreateMutex();
    }
    
public:
    static HardwareAbstractionLayer& getInstance() {
        if (!instance) {
            instance = new HardwareAbstractionLayer();
        }
        return *instance;
    }
    
    /**
     * @brief Configure hardware components
     * @param cfg Hardware configuration
     */
    void configure(const HardwareConfig& cfg) {
        if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            config = cfg;
            xSemaphoreGive(mutex);
        }
    }
    
    /**
     * @brief Get hardware configuration
     * @return Current configuration
     */
    const HardwareConfig& getConfig() const {
        return config;
    }
    
    /**
     * @brief Initialize all configured hardware
     * @return true if all hardware initialized successfully
     */
    bool initializeAll() {
        bool success = true;
        
        // Initialize temperature sensors
        if (config.boilerTempSensor) {
            success &= config.boilerTempSensor->initialize();
        }
        if (config.waterTempSensor) {
            success &= config.waterTempSensor->initialize();
        }
        if (config.outsideTempSensor) {
            success &= config.outsideTempSensor->initialize();
        }
        if (config.roomTempSensor) {
            success &= config.roomTempSensor->initialize();
        }
        
        // Initialize relays
        if (config.burnerRelay) {
            success &= config.burnerRelay->initialize();
        }
        if (config.pumpRelay) {
            success &= config.pumpRelay->initialize();
        }
        
        // Initialize inputs
        if (config.flameSensor) {
            success &= config.flameSensor->initialize(0);  // Pin would be configured
        }
        if (config.emergencyStop) {
            success &= config.emergencyStop->initialize(0);  // Pin would be configured
        }
        
        // Initialize system components
        if (config.rtc) {
            success &= config.rtc->initialize();
        }
        if (config.watchdog) {
            success &= config.watchdog->initialize(30000);  // 30 second timeout
        }
        if (config.network) {
            success &= config.network->initialize();
        }
        
        return success;
    }
};

} // namespace HAL