// src/diagnostics/MQTTDiagnostics.h
#ifndef MQTT_DIAGNOSTICS_H
#define MQTT_DIAGNOSTICS_H

#include <string>
#include <functional>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "ArduinoJson.h"
#include "monitoring/HealthMonitor.h"
#include "config/SystemConstants.h"

/**
 * @brief MQTT Diagnostics Publisher
 * 
 * This module publishes system diagnostic information to MQTT topics
 * for remote monitoring and analysis.
 * 
 * Topics structure:
 * - {base}/diagnostics/health - Overall system health
 * - {base}/diagnostics/memory - Memory statistics
 * - {base}/diagnostics/tasks - Task information
 * - {base}/diagnostics/sensors - Sensor status
 * - {base}/diagnostics/relays - Relay status
 * - {base}/diagnostics/network - Network statistics
 * - {base}/diagnostics/errors - Error log
 * - {base}/diagnostics/performance - Performance metrics
 * - {base}/diagnostics/pid - PID controller status
 * - {base}/diagnostics/burner - Burner state machine status
 * - {base}/diagnostics/maintenance - Predictive maintenance alerts
 */
class MQTTDiagnostics {
public:
    // Callback type for MQTT publishing
    using PublishCallback = std::function<bool(const char* topic, const char* payload, int qos, bool retain)>;
    
    // Diagnostic update intervals (ms)
    struct UpdateIntervals {
        uint32_t health = 60000;        // 1 minute
        uint32_t memory = 300000;       // 5 minutes
        uint32_t tasks = 300000;        // 5 minutes
        uint32_t sensors = 30000;       // 30 seconds
        uint32_t relays = 10000;        // 10 seconds
        uint32_t network = 60000;       // 1 minute
        uint32_t performance = 60000;   // 1 minute
        uint32_t pid = 5000;           // 5 seconds (when active)
        uint32_t burner = 5000;        // 5 seconds
        uint32_t maintenance = 3600000; // 1 hour
        uint32_t queues = 10000;        // 10 seconds
        // uint32_t ble = 60000;           // 1 minute - BLE removed
    };
    
private:
    static const char* TAG;
    static MQTTDiagnostics* instance;
    
    // Configuration
    std::string baseTopic;
    PublishCallback publishCallback;
    UpdateIntervals intervals;
    bool enabled;
    
    // Task handle
    TaskHandle_t taskHandle;
    
    // Timing tracking
    struct LastPublishTimes {
        TickType_t health = 0;
        TickType_t memory = 0;
        TickType_t tasks = 0;
        TickType_t sensors = 0;
        TickType_t relays = 0;
        TickType_t network = 0;
        TickType_t performance = 0;
        TickType_t pid = 0;
        TickType_t burner = 0;
        TickType_t maintenance = 0;
        TickType_t queues = 0;
        // TickType_t ble = 0;  // BLE removed
    } lastPublish;
    
    // Performance tracking
    struct PerformanceMetrics {
        uint32_t loopCount = 0;
        uint32_t maxLoopTime = 0;
        uint32_t avgLoopTime = 0;
        uint32_t publishCount = 0;
        uint32_t publishFailures = 0;
        TickType_t startTime = 0;
    } metrics;
    
    // Mutex for thread safety
    SemaphoreHandle_t mutex;
    
public:
    /**
     * @brief Get singleton instance
     */
    static MQTTDiagnostics* getInstance();

    #ifdef UNIT_TEST
    /**
     * @brief Reset singleton state for testing
     *
     * TESTING ONLY: Deletes and recreates the singleton instance.
     * Only compiled when UNIT_TEST is defined (test environments).
     *
     * Warning: This calls cleanup() which deletes the instance entirely.
     * After reset, initialize() must be called again before use.
     */
    static void resetForTesting() {
        cleanup();
        // getInstance() will create a new instance when next called
    }
    #endif

    /**
     * @brief Initialize MQTT diagnostics
     * @param baseTopic Base topic for all diagnostics (e.g., "boiler/system")
     * @param publishCallback Function to publish MQTT messages
     * @param taskStackSize Stack size for diagnostics task
     * @param taskPriority Priority for diagnostics task
     */
    bool initialize(const std::string& baseTopic, 
                   PublishCallback publishCallback,
                   uint32_t taskStackSize = 8192,
                   UBaseType_t taskPriority = 2);
    
    /**
     * @brief Enable/disable diagnostics publishing
     */
    void setEnabled(bool enable) { enabled = enable; }
    
    /**
     * @brief Check if diagnostics are enabled
     */
    bool isEnabled() const { return enabled; }
    
    /**
     * @brief Set update intervals
     */
    void setUpdateIntervals(const UpdateIntervals& newIntervals) { intervals = newIntervals; }
    
    /**
     * @brief Force immediate update of all diagnostics
     */
    void forceUpdate();
    
    /**
     * @brief Publish error event immediately
     */
    void publishError(const char* component, const char* error, const char* details = nullptr);
    
    /**
     * @brief Publish maintenance alert
     */
    void publishMaintenanceAlert(const char* component, const char* alert, int severity);
    
    /**
     * @brief Clear diagnostics buffers for emergency memory recovery
     * @return Estimated bytes of memory freed
     */
    static size_t clearDiagnosticsBuffers();
    
    /**
     * @brief Restore normal diagnostic operation after memory recovery
     */
    static void restoreNormalOperation();
    
    /**
     * @brief Get current memory usage by diagnostics
     * @return Estimated memory currently used
     */
    static size_t getMemoryUsage();
    
    /**
     * @brief Publish diagnostics data (public interface for other components)
     * @param subtopic The subtopic under diagnostics/
     * @param payload The JSON payload
     * @param retain Whether to retain the message
     */
    bool publishDiagnostics(const char* subtopic, const char* payload, bool retain = true) {
        return publish(subtopic, payload, retain);
    }
    
    /**
     * @brief Clean up the singleton instance
     * Call this during system shutdown to prevent memory leak
     */
    static void cleanup();
    
private:
    MQTTDiagnostics();
    ~MQTTDiagnostics();
    
    // Task function
    static void diagnosticsTask(void* pvParameters);
    void runDiagnostics();
    
    // Publishing functions
    void publishHealthStatus();
    void publishMemoryStatus();
    void publishTaskStatus();
    void publishSensorStatus();
    void publishRelayStatus();
    void publishNetworkStatus();
    void publishPerformanceMetrics();
    void publishPIDStatus();
    void publishBurnerStatus();
    void publishMaintenanceStatus();
    void publishQueueStatus();
    // void publishBLEStatus();  // BLE removed
    
    // Helper functions
    bool shouldPublish(TickType_t& lastTime, uint32_t interval);
    bool publish(const char* subtopic, const JsonDocument& doc, bool retain = true);
    bool publish(const char* subtopic, const char* payload, bool retain = true);
    // getFullTopic method removed - using snprintf directly instead
    
    // Data collection helpers
    void collectTaskInfo(JsonDocument& doc);
    void collectMemoryInfo(JsonDocument& doc);
    void collectSensorInfo(JsonDocument& doc);
    void collectRelayInfo(JsonDocument& doc);
    void collectNetworkInfo(JsonDocument& doc);
};

#endif // MQTT_DIAGNOSTICS_H