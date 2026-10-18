// include/utils/ErrorRecovery.h
#ifndef ERROR_RECOVERY_H
#define ERROR_RECOVERY_H

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/event_groups.h>
#include <functional>
#include <vector>
#include <cstdint>

/**
 * @file ErrorRecovery.h
 * @brief Centralized error recovery system for the boiler controller
 * 
 * This module provides a structured approach to error recovery with:
 * - Hierarchical recovery strategies
 * - Exponential backoff for retries
 * - System-wide error coordination
 * - Recovery action callbacks
 */

class ErrorRecovery {
public:
    // Error severity levels
    enum class Severity {
        INFO,       // Informational only
        WARNING,    // System can continue but degraded
        ERROR,      // Recoverable error
        CRITICAL,   // System must take immediate action
        FATAL       // System cannot continue
    };

    // Error categories
    enum class Category {
        SENSOR,         // Temperature sensor failures
        COMMUNICATION,  // MQTT, Modbus, etc.
        HARDWARE,       // Relay, burner control
        NETWORK,        // Ethernet connectivity
        MEMORY,         // Low memory conditions
        SYSTEM          // General system errors
    };

    // Recovery strategies
    enum class Strategy {
        NONE,           // No recovery needed
        RETRY,          // Simple retry with backoff
        FALLBACK,       // Use fallback/default values
        RESTART_TASK,   // Restart the affected task
        RESTART_MODULE, // Restart a specific module
        SAFE_MODE,      // Enter safe mode
        REBOOT          // Full system reboot
    };

    // Recovery action callback
    using RecoveryAction = std::function<bool()>;

    // Error context information
    struct ErrorContext {
        Category category;
        Severity severity;
        uint32_t errorCode;
        const char* source;      // Task/module name
        const char* description;
        uint32_t timestamp;
        uint32_t occurrenceCount;
    };

    // Recovery plan for an error
    struct RecoveryPlan {
        Strategy primaryStrategy;
        Strategy fallbackStrategy;
        uint32_t maxRetries;
        uint32_t retryDelayMs;
        float backoffMultiplier;
        RecoveryAction customAction;
    };

    // Initialize the error recovery system
    static bool initialize();

    // Report an error and get recovery plan
    static RecoveryPlan reportError(
        Category category,
        Severity severity,
        uint32_t errorCode,
        const char* source,
        const char* description
    );

    // Execute a recovery plan
    static bool executeRecovery(const RecoveryPlan& plan, const ErrorContext& context);

    // Register custom recovery action for specific error
    static void registerRecoveryAction(
        Category category,
        uint32_t errorCode,
        RecoveryAction action
    );

    // Check if system is in recovery mode
    static bool isInRecovery();

    // Get current system health score (0-100)
    static uint8_t getHealthScore();

    // Clear error history for a category
    static void clearErrors(Category category);

    // Emergency shutdown procedure
    static void emergencyShutdown(const char* reason);

private:
    // Error tracking structure
    struct ErrorRecord {
        ErrorContext context;
        uint32_t firstOccurrence;
        uint32_t lastOccurrence;
        uint32_t totalCount;
        bool recovered;
    };

    // Recovery state
    struct RecoveryState {
        bool inRecovery;
        Category activeCategory;
        uint32_t recoveryStartTime;
        uint32_t retryCount;
        Strategy currentStrategy;
    };

    // Static members
    static SemaphoreHandle_t recoveryMutex_;
    static EventGroupHandle_t recoveryEventGroup_;
    static std::vector<ErrorRecord> errorHistory_;
    static std::vector<std::pair<uint32_t, RecoveryAction>> customActions_;
    static RecoveryState recoveryState_;
    static bool initialized_;

    // Event bits for recovery coordination
    static constexpr uint32_t RECOVERY_IN_PROGRESS_BIT = (1UL << 0);
    static constexpr uint32_t SAFE_MODE_ACTIVE_BIT = (1UL << 1);
    static constexpr uint32_t EMERGENCY_SHUTDOWN_BIT = (1UL << 2);

    // Helper methods
    static RecoveryPlan getDefaultPlan(Category category, Severity severity);
    static uint32_t calculateBackoff(uint32_t baseDelay, uint32_t retryCount, float multiplier);
    static void updateHealthScore();
    static bool shouldEscalate(const ErrorContext& context);
};

// Convenience macros for error reporting
#define REPORT_ERROR(category, code, desc) \
    ErrorRecovery::reportError(ErrorRecovery::Category::category, \
                              ErrorRecovery::Severity::ERROR, \
                              code, __FUNCTION__, desc)

#define REPORT_WARNING(category, code, desc) \
    ErrorRecovery::reportError(ErrorRecovery::Category::category, \
                              ErrorRecovery::Severity::WARNING, \
                              code, __FUNCTION__, desc)

#define REPORT_CRITICAL(category, code, desc) \
    ErrorRecovery::reportError(ErrorRecovery::Category::category, \
                              ErrorRecovery::Severity::CRITICAL, \
                              code, __FUNCTION__, desc)

// Error codes for each category
namespace ErrorCode {
    // Sensor errors (0x1000 - 0x1FFF)
    constexpr uint32_t SENSOR_TIMEOUT = 0x1001;
    constexpr uint32_t SENSOR_INVALID_READING = 0x1002;
    constexpr uint32_t SENSOR_COMMUNICATION_FAILURE = 0x1003;
    constexpr uint32_t SENSOR_CALIBRATION_ERROR = 0x1004;
    
    // Communication errors (0x2000 - 0x2FFF)
    constexpr uint32_t MQTT_CONNECTION_LOST = 0x2001;
    constexpr uint32_t MQTT_PUBLISH_FAILED = 0x2002;
    constexpr uint32_t MODBUS_TIMEOUT = 0x2003;
    constexpr uint32_t MODBUS_CRC_ERROR = 0x2004;
    
    // Hardware errors (0x3000 - 0x3FFF)
    constexpr uint32_t RELAY_CONTROL_FAILED = 0x3001;
    constexpr uint32_t BURNER_IGNITION_FAILED = 0x3002;
    constexpr uint32_t PUMP_NOT_RESPONDING = 0x3003;
    constexpr uint32_t SAFETY_INTERLOCK_TRIGGERED = 0x3004;
    
    // Network errors (0x4000 - 0x4FFF)
    constexpr uint32_t ETHERNET_LINK_DOWN = 0x4001;
    constexpr uint32_t DHCP_TIMEOUT = 0x4002;
    constexpr uint32_t DNS_RESOLUTION_FAILED = 0x4003;
    
    // Memory errors (0x5000 - 0x5FFF)
    constexpr uint32_t HEAP_LOW = 0x5001;
    constexpr uint32_t STACK_OVERFLOW = 0x5002;
    constexpr uint32_t ALLOCATION_FAILED = 0x5003;
    
    // System errors (0x6000 - 0x6FFF)
    constexpr uint32_t WATCHDOG_TIMEOUT = 0x6001;
    constexpr uint32_t TASK_CREATION_FAILED = 0x6002;
    constexpr uint32_t CONFIGURATION_INVALID = 0x6003;
}

#endif // ERROR_RECOVERY_H