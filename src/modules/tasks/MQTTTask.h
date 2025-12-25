// src/modules/tasks/MQTTTask.h
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <memory>
#include <atomic>  // Round 20 Issue #5: For atomic circuit breaker state
#include "config/ProjectConfig.h"
#include "config/SystemConstants.h"
#include "MQTTManager.h"
#include "core/QueueManager.h"

/**
 * MQTT message priority levels
 */
enum class MQTTPriority : uint8_t {
    PRIORITY_CRITICAL = 0, // Safety-critical: burner commands, emergency events - bypasses queue when under pressure
    PRIORITY_HIGH = 1,     // Sensor data - always processed first
    PRIORITY_MEDIUM = 2,   // Status updates
    PRIORITY_LOW = 3       // Config/parameters - can be delayed
};

/**
 * Structure for MQTT publish requests
 */
struct MQTTPublishRequest {
    char topic[64];      // Topic path
    char payload[320];   // Increased to handle sensor status JSON (was 192, too small)
    uint8_t qos;
    bool retain;
    MQTTPriority priority;  // Message priority for queue management
    TickType_t timestamp;
};

/**
 * @brief MQTT communication task for Boiler Controller
 * 
 * Manages MQTT connection, reconnection, message publishing and subscription handling
 */
class MQTTTask {
public:
    /**
     * Initialize the MQTT task
     * @return true if initialization successful, false otherwise
     */
    static bool init();

    /**
     * Start the MQTT task
     * @return true if task started successfully, false otherwise
     */
    static bool start();

    /**
     * Stop the MQTT task
     */
    static void stop();

    /**
     * Check if the task is running
     * @return true if task is running, false otherwise
     */
    static bool isRunning();

    /**
     * Get the task handle
     * @return Task handle or nullptr if not running
     */
    static TaskHandle_t getTaskHandle();

    /**
     * Get the MQTT manager instance
     * @return Pointer to MQTTManager or nullptr if not initialized
     */
    static MQTTManager* getMQTTManager();

    /**
     * Check if MQTT is connected
     * @return true if connected, false otherwise
     */
    static bool isConnected();

    /**
     * Publish a message (thread-safe)
     * @param topic The topic to publish to
     * @param payload The message payload
     * @param qos Quality of Service level
     * @param retain Whether to retain the message
     * @param priority Message priority (default MEDIUM)
     * @return true if published successfully, false otherwise
     */
    static bool publish(const char* topic, const char* payload, int qos = 0, bool retain = false, 
                       MQTTPriority priority = MQTTPriority::PRIORITY_MEDIUM);

    /**
     * Subscribe to a topic (thread-safe)
     * @param topic The topic to subscribe to
     * @param callback The callback function for received messages
     * @param qos Quality of Service level
     * @return true if subscribed successfully, false otherwise
     */
    static bool subscribe(const char* topic, std::function<void(const char*)> callback, int qos = 0);

    /**
     * Check if MQTT queues are under pressure (backpressure signaling)
     * Producers should call this before publishing non-critical messages
     * @return true if queues are congested and low-priority messages should be skipped
     */
    static bool isUnderPressure();

    /**
     * Check if a message with given priority should be throttled
     * HIGH priority: never throttled
     * MEDIUM priority: throttled when queue > 80% full
     * LOW priority: throttled when queue > 50% full
     * @param priority The priority of the message to check
     * @return true if the message should be skipped due to backpressure
     */
    static bool shouldThrottle(MQTTPriority priority);

    /**
     * Get current queue utilization percentage (0-100)
     * @return Combined utilization of high and normal priority queues
     */
    static uint8_t getQueueUtilization();

private:
    MQTTTask() = delete;  // Prevent instantiation
    
    /**
     * Main task function
     */
    static void taskFunction(void* parameter);
    
    static TaskHandle_t taskHandle_;
    static bool isRunning_;
    static MQTTManager* mqttManager_;
    static SemaphoreHandle_t mqttMutex_;
    static std::shared_ptr<QueueManager::ManagedQueue> highPriorityQueue_;    // For sensor data
    static std::shared_ptr<QueueManager::ManagedQueue> normalPriorityQueue_;  // For config/status
    static uint32_t lastReconnectAttempt_;
    static uint32_t currentReconnectDelay_;
    static uint8_t reconnectAttempts_;
    // Round 20 Issue #5: Made atomic to prevent race conditions during disconnect handling
    static std::atomic<uint8_t> consecutiveDisconnects_;     // Circuit breaker: track failures
    static uint32_t circuitBreakerCooldownEnd_;              // Circuit breaker: cooldown end time
    static std::atomic<bool> circuitBreakerOpen_;            // Circuit breaker: open state
    static constexpr uint32_t MIN_RECONNECT_INTERVAL_MS = SystemConstants::Tasks::MQTT::MIN_RECONNECT_INTERVAL_MS;
    static constexpr uint32_t MAX_RECONNECT_INTERVAL_MS = SystemConstants::Tasks::MQTT::MAX_RECONNECT_INTERVAL_MS;
    static constexpr uint32_t CONNECTION_CHECK_INTERVAL_MS = SystemConstants::Tasks::MQTT::CONNECTION_CHECK_INTERVAL_MS;
    static constexpr uint8_t MAX_RECONNECT_ATTEMPTS = 10;
    static constexpr uint32_t CIRCUIT_BREAKER_COOLDOWN_MS = 600000;  // 10 minutes cooldown
    static const size_t HIGH_PRIORITY_QUEUE_SIZE = 3;    // Optimized for typical usage
    static const size_t NORMAL_PRIORITY_QUEUE_SIZE = 5;   // Reduced from 10 - saves 1.6KB
    
    // Remove duplicate declarations - these are already declared above
    
    /**
     * Process queued publish requests
     */
    static void processPublishQueue();
    
    /**
     * Initialize MQTT connection
     */
    static void initializeMQTT();
    
    /**
     * Handle MQTT reconnection
     */
    static void handleReconnection();
    
    /**
     * Process MQTT events and maintain connection
     */
    static void processMQTT();
    
    /**
     * Publish system health (heap, uptime, etc.)
     */
    static void publishSystemStatus();

    /**
     * Publish system state (enabled/disabled, active/inactive for heating/water)
     */
    static void publishSystemState();

    /**
     * Publish sensor data
     */
    static void publishSensorData();
    
    /**
     * Setup standard subscriptions
     */
    static void setupSubscriptions();

    /**
     * Retry failed subscriptions
     */
    static void retryFailedSubscriptions();

    /**
     * Handle control commands received via MQTT
     * @param topic The topic the command was received on
     * @param payload The command payload
     */
    static void handleControlCommand(const char* topic, const char* payload);
    static void handleErrorCommand(const char* topic, const char* payload);
    static void handleSchedulerCommand(const char* topic, const char* payload);
};