// src/utils/ErrorHandler.h
#ifndef ERROR_HANDLER_H
#define ERROR_HANDLER_H

#include <cstdint>
#include <string>
#include "config/ProjectConfig.h"

// Forward declaration
class ErrorLogFRAM;

/**
 * @brief Error Return Type Conventions (L10)
 *
 * Use Result<void> for:
 * - Action functions that can fail (activate, deactivate, initialize, etc.)
 * - Operations that need to communicate WHY they failed
 * - High-level API functions exposed to other modules
 *
 * Use bool for:
 * - Predicates and checks (isValid, canOperate, hasError, etc.)
 * - Simple success/failure where the reason is obvious
 * - Low-level operations that log internally
 *
 * IMPORTANT: Always check Result<void> returns from:
 * - BurnerSystemController::activate*, deactivate, setPowerLevel, switchMode
 * - Any function that affects burner/pump state
 * - Safety-critical operations
 *
 * Exception: emergencyShutdown() is best-effort and always succeeds.
 */

/**
 * @brief Unified error codes for the system
 */
enum class SystemError : uint32_t {
    SUCCESS = 0,
    
    // General errors (1-99)
    UNKNOWN_ERROR = 1,
    INVALID_PARAMETER = 2,
    TIMEOUT = 3,
    NOT_INITIALIZED = 4,
    ALREADY_INITIALIZED = 5,
    MEMORY_ALLOCATION_FAILED = 6,
    
    // Mutex/Thread errors (100-199)
    MUTEX_CREATE_FAILED = 100,
    MUTEX_TIMEOUT = 101,
    MUTEX_NOT_OWNED = 102,
    TASK_CREATE_FAILED = 110,
    TASK_NOT_FOUND = 111,
    
    // Network errors (200-299)
    NETWORK_NOT_CONNECTED = 200,
    NETWORK_INIT_FAILED = 201,
    NETWORK_TIMEOUT = 202,
    NETWORK_ERROR = 203,
    ETHERNET_PHY_ERROR = 210,
    ETHERNET_LINK_DOWN = 211,
    
    // MQTT errors (300-399)
    MQTT_NOT_CONNECTED = 300,
    MQTT_CONNECT_FAILED = 301,
    MQTT_PUBLISH_FAILED = 302,
    MQTT_SUBSCRIBE_FAILED = 303,
    MQTT_BROKER_UNREACHABLE = 304,
    
    // Modbus errors (400-499)
    MODBUS_TIMEOUT = 400,
    MODBUS_CRC_ERROR = 401,
    MODBUS_INVALID_RESPONSE = 402,
    MODBUS_DEVICE_NOT_FOUND = 403,
    MODBUS_INIT_FAILED = 404,
    MODBUS_COMMUNICATION_ERROR = 405,
    
    // Device errors (450-499)
    DEVICE_NOT_INITIALIZED = 450,
    
    // Sensor errors (500-599)
    SENSOR_READ_FAILED = 500,
    SENSOR_NOT_READY = 501,
    SENSOR_OUT_OF_RANGE = 502,
    SENSOR_INVALID_DATA = 503,
    SENSOR_FAILURE = 504,
    
    // Relay errors (600-699)
    RELAY_OPERATION_FAILED = 600,
    RELAY_SAFETY_INTERLOCK = 601,
    RELAY_NOT_RESPONDING = 602,
    RELAY_FAULT = 603,
    RELAY_VERIFICATION_FAILED = 604,

    // Pump errors (610-629)
    PUMP_PROTECTION_ACTIVE = 610,
    PUMP_VERIFICATION_FAILED = 611,
    PUMP_MOTOR_FAULT = 612,

    // System errors (700-799)
    SYSTEM_OVERHEATED = 700,
    SYSTEM_LOW_MEMORY = 701,
    SYSTEM_WATCHDOG_TIMEOUT = 702,
    SYSTEM_FAILSAFE_TRIGGERED = 703,
    TEMPERATURE_CRITICAL = 704,
    IGNITION_FAILURE = 705,
    WATCHDOG_INIT_FAILED = 706,
    EMERGENCY_STOP = 707,
    
    // Configuration errors (800-899)
    CONFIG_INVALID = 800,
    CONFIG_MISSING = 801,
    CONFIG_CORRUPTED = 802
};

/**
 * @brief Application Result type for error handling
 * 
 * Our own Result type for application-level error handling.
 * This coexists with ModbusDevice's Result template.
 */
template<typename T>
class Result {
private:
    bool success_;
    T value_;
    SystemError error_;
    std::string message_;

public:
    // Success constructor
    explicit Result(const T& value) 
        : success_(true), value_(value), error_(SystemError::SUCCESS), message_("") {}
    
    // Error constructor
    Result(SystemError error, const std::string& message = "") 
        : success_(false), value_{}, error_(error), message_(message) {}
    
    bool isSuccess() const { return success_; }
    bool isError() const { return !success_; }
    
    const T& value() const { return value_; }
    SystemError error() const { return error_; }
    const std::string& message() const { return message_; }
};

// Specialization for void
template<>
class Result<void> {
private:
    bool success_;
    SystemError error_;
    std::string message_;

public:
    // Success constructor
    Result() : success_(true), error_(SystemError::SUCCESS), message_("") {}
    
    // Error constructor
    Result(SystemError error, const std::string& message = "") 
        : success_(false), error_(error), message_(message) {}
    
    bool isSuccess() const { return success_; }
    bool isError() const { return !success_; }
    
    SystemError error() const { return error_; }
    const std::string& message() const { return message_; }
};

/**
 * @brief Error handler utility class
 */
class ErrorHandler {
public:
    /**
     * @brief Convert error code to string
     */
    static const char* errorToString(SystemError error) {
        switch (error) {
            case SystemError::SUCCESS: return "Success";
            case SystemError::UNKNOWN_ERROR: return "Unknown error";
            case SystemError::INVALID_PARAMETER: return "Invalid parameter";
            case SystemError::TIMEOUT: return "Operation timeout";
            case SystemError::NOT_INITIALIZED: return "Not initialized";
            case SystemError::ALREADY_INITIALIZED: return "Already initialized";
            case SystemError::MEMORY_ALLOCATION_FAILED: return "Memory allocation failed";
            
            case SystemError::MUTEX_CREATE_FAILED: return "Mutex creation failed";
            case SystemError::MUTEX_TIMEOUT: return "Mutex timeout";
            case SystemError::MUTEX_NOT_OWNED: return "Mutex not owned";
            case SystemError::TASK_CREATE_FAILED: return "Task creation failed";
            case SystemError::TASK_NOT_FOUND: return "Task not found";
            
            case SystemError::NETWORK_NOT_CONNECTED: return "Network not connected";
            case SystemError::NETWORK_INIT_FAILED: return "Network init failed";
            case SystemError::NETWORK_TIMEOUT: return "Network timeout";
            case SystemError::ETHERNET_PHY_ERROR: return "Ethernet PHY error";
            case SystemError::ETHERNET_LINK_DOWN: return "Ethernet link down";
            
            case SystemError::MQTT_NOT_CONNECTED: return "MQTT not connected";
            case SystemError::MQTT_CONNECT_FAILED: return "MQTT connect failed";
            case SystemError::MQTT_PUBLISH_FAILED: return "MQTT publish failed";
            case SystemError::MQTT_SUBSCRIBE_FAILED: return "MQTT subscribe failed";
            case SystemError::MQTT_BROKER_UNREACHABLE: return "MQTT broker unreachable";
            
            case SystemError::MODBUS_TIMEOUT: return "Modbus timeout";
            case SystemError::MODBUS_CRC_ERROR: return "Modbus CRC error";
            case SystemError::MODBUS_INVALID_RESPONSE: return "Modbus invalid response";
            case SystemError::MODBUS_DEVICE_NOT_FOUND: return "Modbus device not found";
            case SystemError::MODBUS_INIT_FAILED: return "Modbus init failed";
            case SystemError::MODBUS_COMMUNICATION_ERROR: return "Modbus communication error";
            
            case SystemError::SENSOR_READ_FAILED: return "Sensor read failed";
            case SystemError::SENSOR_NOT_READY: return "Sensor not ready";
            case SystemError::SENSOR_OUT_OF_RANGE: return "Sensor out of range";
            case SystemError::SENSOR_INVALID_DATA: return "Sensor invalid data";
            case SystemError::SENSOR_FAILURE: return "Sensor failure";
            
            case SystemError::RELAY_OPERATION_FAILED: return "Relay operation failed";
            case SystemError::RELAY_SAFETY_INTERLOCK: return "Relay safety interlock";
            case SystemError::RELAY_NOT_RESPONDING: return "Relay not responding";
            case SystemError::RELAY_FAULT: return "Relay fault";

            case SystemError::PUMP_PROTECTION_ACTIVE: return "Pump motor protection active (30s minimum)";
            case SystemError::PUMP_VERIFICATION_FAILED: return "Pump verification failed";
            case SystemError::PUMP_MOTOR_FAULT: return "Pump motor fault";

            case SystemError::SYSTEM_OVERHEATED: return "System overheated";
            case SystemError::SYSTEM_LOW_MEMORY: return "System low memory";
            case SystemError::SYSTEM_WATCHDOG_TIMEOUT: return "System watchdog timeout";
            case SystemError::SYSTEM_FAILSAFE_TRIGGERED: return "System failsafe triggered";
            case SystemError::TEMPERATURE_CRITICAL: return "Temperature critical";
            case SystemError::IGNITION_FAILURE: return "Ignition failure";
            case SystemError::WATCHDOG_INIT_FAILED: return "Watchdog init failed";
            case SystemError::EMERGENCY_STOP: return "Emergency stop";
            
            case SystemError::CONFIG_INVALID: return "Config invalid";
            case SystemError::CONFIG_MISSING: return "Config missing";
            case SystemError::CONFIG_CORRUPTED: return "Config corrupted";
            
            case SystemError::DEVICE_NOT_INITIALIZED: return "Device not initialized";
            
            default: return "Unknown error code";
        }
    }
    
    /**
     * @brief Log error with context
     */
    static void logError(const char* tag, SystemError error, const char* context = nullptr);
    
    /**
     * @brief Clear rate limit for a specific error (when error is resolved)
     */
    static void clearErrorRateLimit(SystemError error);
    
    /**
     * @brief Log critical error with optional details
     */
    static void logCriticalError(SystemError error, const char* details = nullptr);
    
    /**
     * @brief Handle critical errors that require system action
     */
    static void handleCriticalError(SystemError error) {
        logCriticalError(error);
        
        switch (error) {
            case SystemError::SYSTEM_OVERHEATED:
            case SystemError::SYSTEM_FAILSAFE_TRIGGERED:
            case SystemError::RELAY_SAFETY_INTERLOCK:
            case SystemError::TEMPERATURE_CRITICAL:
                // Enter failsafe mode
                enterFailsafeMode(error);
                break;
                
            case SystemError::SYSTEM_LOW_MEMORY:
                // Try to free memory
                attemptMemoryRecovery();
                break;
                
            case SystemError::SYSTEM_WATCHDOG_TIMEOUT:
                // System will reset automatically
                break;
                
            default:
                // Log and continue
                break;
        }
    }
    
    // Failsafe mode (public for main.cpp)
    static void enterFailsafeMode(SystemError reason);
    
private:
    // Returns true if memory recovery was successful, false otherwise
    static bool attemptMemoryRecovery();
};

// Convenience macros for error handling
#define CHECK_ERROR(result) \
    do { \
        if ((result).isError()) { \
            ErrorHandler::logError(LOG_TAG_MAIN, (result).error(), __FUNCTION__); \
            return SystemResult<void>((result).error(), (result).message()); \
        } \
    } while(0)

#define CHECK_ERROR_RETURN(result, returnValue) \
    do { \
        if ((result).isError()) { \
            ErrorHandler::logError(LOG_TAG_MAIN, (result).error(), __FUNCTION__); \
            return returnValue; \
        } \
    } while(0)

#endif // ERROR_HANDLER_H