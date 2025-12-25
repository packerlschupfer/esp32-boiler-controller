// src/modules/tasks/MQTTTaskEventDriven.cpp
// MQTT task - handles communication with MQTT broker

#include "MQTTTask.h"

#include "config/SystemConstants.h"
#include "config/SystemSettingsStruct.h"
#include "config/SystemSettings.h"
#include "events/SystemEventsGenerated.h"
#include "shared/SharedSensorReadings.h"
#include "shared/SharedRelayReadings.h"
#include "shared/Temperature.h"
#include <EthernetManager.h>
#include <ETH.h>
#include <SemaphoreGuard.h>
#include <ArduinoJson.h>
#include "utils/ErrorHandler.h"
#include "utils/ErrorLogFRAM.h"
#include <esp_log.h>
#include "core/SystemResourceProvider.h"
#include "MQTTTopics.h"
#include <TaskManager.h>
#include "utils/PooledString.h"
#include "utils/MemoryPool.h"
#include "diagnostics/MQTTDiagnostics.h"
#include "modules/tasks/TimerSchedulerTask.h"
#include "modules/control/TemperatureSensorFallback.h"
#include "modules/control/BurnerStateMachine.h"
#include <cstring>
#include <ctype.h>
#include "config/ProjectConfig.h"
#include <RuntimeStorage.h>
#include "modules/mqtt/MQTTCommandHandlers.h"

static const char* TAG = "MQTT";

// Static member definitions (must match MQTTTask.h)
TaskHandle_t MQTTTask::taskHandle_ = nullptr;
bool MQTTTask::isRunning_ = false;
MQTTManager* MQTTTask::mqttManager_ = nullptr;
SemaphoreHandle_t MQTTTask::mqttMutex_ = nullptr;
std::shared_ptr<QueueManager::ManagedQueue> MQTTTask::highPriorityQueue_ = nullptr;
std::shared_ptr<QueueManager::ManagedQueue> MQTTTask::normalPriorityQueue_ = nullptr;
uint32_t MQTTTask::lastReconnectAttempt_ = 0;
uint32_t MQTTTask::currentReconnectDelay_ = MQTTTask::MIN_RECONNECT_INTERVAL_MS;
uint8_t MQTTTask::reconnectAttempts_ = 0;
// Round 20 Issue #5: Made atomic for thread-safe circuit breaker handling
std::atomic<uint8_t> MQTTTask::consecutiveDisconnects_{0};
uint32_t MQTTTask::circuitBreakerCooldownEnd_ = 0;
std::atomic<bool> MQTTTask::circuitBreakerOpen_{false};

// Event bits for MQTT task operations
static constexpr uint32_t MQTT_TASK_CONNECTED = (1 << 0);
static constexpr uint32_t MQTT_TASK_DISCONNECTED = (1 << 1);
static constexpr uint32_t MQTT_TASK_MESSAGE = (1 << 2);
static constexpr uint32_t MQTT_TASK_PUBLISH_SENSORS = (1 << 3);
static constexpr uint32_t MQTT_TASK_PUBLISH_HEALTH = (1 << 4);
static constexpr uint32_t MQTT_TASK_PROCESS_QUEUE = (1 << 5);
static constexpr uint32_t MQTT_TASK_RETRY_SUBSCRIPTIONS = (1 << 6);

// Event group for MQTT task (declared early for use in timer callback)
static EventGroupHandle_t mqttTaskEventGroup = nullptr;

// ============================================================================
// THREAD-SAFETY NOTE (Round 14 Issue #6):
// Variables in this anonymous namespace are SAFE because:
// - failedSubscriptions, subscriptionRetryTimer, subscriptionRetryAttempts:
//   Only accessed from MQTTTask::taskFunction() and timer callback
//   (timer runs in daemon task but only sets event bits, actual access is in task)
// - Timer callback only calls xEventGroupSetBits() which is thread-safe
// DO NOT access these variables from other tasks.
// ============================================================================
// Subscription retry tracking
namespace {
    // Track which subscription groups failed (bitmask)
    static uint8_t failedSubscriptions = 0;
    static constexpr uint8_t SUB_TEST_ECHO = (1 << 0);
    static constexpr uint8_t SUB_CMD = (1 << 1);
    static constexpr uint8_t SUB_CONFIG = (1 << 2);
    static constexpr uint8_t SUB_ERRORS = (1 << 3);
    static constexpr uint8_t SUB_SCHEDULER = (1 << 4);
    static constexpr uint32_t SUBSCRIPTION_RETRY_DELAY_MS = 5000;  // 5 seconds
    static TimerHandle_t subscriptionRetryTimer = nullptr;

    // Round 14 Issue #14: Limit subscription retry attempts
    static uint8_t subscriptionRetryAttempts = 0;
    static constexpr uint8_t MAX_SUBSCRIPTION_RETRIES = 10;

    static void subscriptionRetryTimerCallback(TimerHandle_t xTimer) {
        (void)xTimer;

        // Round 14 Issue #14: Stop retrying after max attempts
        if (subscriptionRetryAttempts >= MAX_SUBSCRIPTION_RETRIES) {
            LOG_ERROR(TAG, "Max subscription retries (%d) exceeded - giving up",
                     MAX_SUBSCRIPTION_RETRIES);
            // Stop the timer
            if (subscriptionRetryTimer) {
                xTimerStop(subscriptionRetryTimer, 0);
            }
            return;
        }

        subscriptionRetryAttempts++;
        LOG_DEBUG(TAG, "Subscription retry attempt %d/%d",
                 subscriptionRetryAttempts, MAX_SUBSCRIPTION_RETRIES);

        if (mqttTaskEventGroup) {
            xEventGroupSetBits(mqttTaskEventGroup, MQTT_TASK_RETRY_SUBSCRIPTIONS);
        }
    }
}

// Timer handles for periodic events
static TimerHandle_t sensorPublishTimer = nullptr;
static TimerHandle_t healthPublishTimer = nullptr;

// Timer periods - use values from SystemConstants
using namespace SystemConstants::Timing;
namespace MQTTConstants = SystemConstants::Tasks::MQTT;

// Track queue health for logging (avoid spam)
// THREAD-SAFE (Round 14 Issue #6): Only accessed from MQTTTask::taskFunction()
static struct QueueHealthTracking {
    uint32_t lastDropLogTime = 0;
    uint32_t droppedHighPriority = 0;
    uint32_t droppedNormalPriority = 0;
    uint32_t lastHealthLogTime = 0;
} queueHealthTracking;

/**
 * @brief MQTT event callback handler
 */
static void mqttEventCallback(MQTTManager::MQTTEvent event, void* data) {
    if (!mqttTaskEventGroup) return;
    
    switch (event) {
        case MQTTManager::MQTTEvent::CONNECTED:
            LOG_INFO(TAG, "MQTT connected event received");
            xEventGroupSetBits(mqttTaskEventGroup, MQTT_TASK_CONNECTED);
            // Reconnect state is now managed internally by MQTTManager
            break;
            
        case MQTTManager::MQTTEvent::DISCONNECTED:
            LOG_WARN(TAG, "MQTT disconnected event received");
            xEventGroupSetBits(mqttTaskEventGroup, MQTT_TASK_DISCONNECTED);
            break;
            
        case MQTTManager::MQTTEvent::MESSAGE_RECEIVED:
            xEventGroupSetBits(mqttTaskEventGroup, MQTT_TASK_MESSAGE);
            break;
            
        case MQTTManager::MQTTEvent::ERROR:
            if (data) {
                auto* errorData = static_cast<MQTTManager::ErrorEventData*>(data);
                LOG_ERROR(TAG, "MQTT error: %s", errorData->message);
            }
            break;
            
        default:
            break;
    }
}

/**
 * @brief Timer callback for sensor publishing
 */
static void sensorPublishTimerCallback(TimerHandle_t xTimer) {
    if (mqttTaskEventGroup) {
        xEventGroupSetBits(mqttTaskEventGroup, MQTT_TASK_PUBLISH_SENSORS);
    }
}

/**
 * @brief Timer callback for health publishing
 */
static void healthPublishTimerCallback(TimerHandle_t xTimer) {
    if (mqttTaskEventGroup) {
        xEventGroupSetBits(mqttTaskEventGroup, MQTT_TASK_PUBLISH_HEALTH);
    }
}

// Initialize MQTT (implementation from MQTTTask.cpp)
void MQTTTask::initializeMQTT() {
    LOG_INFO(TAG, "=== MQTT INITIALIZATION STARTING ===");
    LOG_INFO(TAG, "Initializing MQTT with event-driven API...");
    
    SemaphoreGuard guard(mqttMutex_);

    if (mqttManager_ == nullptr) {
        mqttManager_ = &MQTTManager::getInstance();
    }
    
    // Configure event-driven features
    mqttManager_->registerEventCallback(mqttEventCallback);
    
    // Configure auto-reconnect with exponential backoff
    MQTTManager::ReconnectConfig reconnectConfig;
    reconnectConfig.minInterval = MIN_RECONNECT_INTERVAL_MS;
    reconnectConfig.maxInterval = MAX_RECONNECT_INTERVAL_MS;
    reconnectConfig.maxAttempts = MAX_RECONNECT_ATTEMPTS;
    reconnectConfig.exponentialBackoff = true;
    mqttManager_->setAutoReconnect(true, reconnectConfig);
    
    // Configure MQTT connection
    // THREAD-SAFETY: Static buffers REQUIRED by MQTTManager library design
    // NOTE: MQTTConfig stores pointers (not copies), so buffers must persist
    // Initialized once during MQTTTask startup (single-threaded)
    // See: docs/MEMORY_OPTIMIZATION.md - Library requirement pattern
    static char mqtt_uri[128];
    snprintf(mqtt_uri, sizeof(mqtt_uri), "mqtt://%s:%d", MQTT_SERVER, MQTT_PORT);

    // Build client ID with "esplan-" prefix to match original format
    static char client_id[64];
    snprintf(client_id, sizeof(client_id), "esplan-%s", DEVICE_HOSTNAME);
    
    LOG_INFO(TAG, "MQTT URI: %s", mqtt_uri);
    LOG_INFO(TAG, "Client ID: %s", client_id);

    // Initialize connection using builder pattern
    auto config = MQTTConfig(mqtt_uri)
        .withClientId(client_id)
        .withCredentials(MQTT_USERNAME, MQTT_PASSWORD)
        .withLastWill(MQTT_STATUS_ONLINE, "{\"online\":false}", 0, true)
        .withAutoReconnect(true);

    auto result = mqttManager_->begin(config);
    if (!result.isOk()) {
        LOG_ERROR(TAG, "MQTT initialization failed with error");
        // Don't delete - MQTTManager is now a singleton
        mqttManager_ = nullptr;
        return;
    }

    LOG_INFO(TAG, "MQTT initialized with event-driven API");
}

// Process MQTT messages using new non-blocking API
void MQTTTask::processMQTT() {
    if (mqttManager_ == nullptr) return;
    
    // Process up to 5 messages without blocking
    mqttManager_->processMessages(5, 0);
}

// Main task function
void MQTTTask::taskFunction(void* parameter) {
    (void)parameter;
    
    LOG_INFO(TAG, "MQTTTask started on core %d", xPortGetCoreID());
    
    // Create event group for task synchronization
    mqttTaskEventGroup = xEventGroupCreate();
    if (!mqttTaskEventGroup) {
        LOG_ERROR(TAG, "Failed to create event group");
        vTaskDelete(NULL);
        return;
    }
    
    // Watchdog will be registered after initialization
    
    // Associate queues with this task
    QueueManager::getInstance().associateQueueWithTask("mqtt_high_priority", xTaskGetCurrentTaskHandle());
    QueueManager::getInstance().associateQueueWithTask("mqtt_normal_priority", xTaskGetCurrentTaskHandle());
    
    // Create timers for periodic events
    sensorPublishTimer = xTimerCreate(
        "MQTTSensorPub",
        pdMS_TO_TICKS(MQTT_SENSOR_PUBLISH_INTERVAL_MS),
        pdTRUE,  // Auto-reload
        nullptr,
        sensorPublishTimerCallback
    );
    
    healthPublishTimer = xTimerCreate(
        "MQTTHealthPub",
        pdMS_TO_TICKS(MQTT_HEALTH_PUBLISH_INTERVAL_MS),
        pdTRUE,  // Auto-reload
        nullptr,
        healthPublishTimerCallback
    );
    
    if (!sensorPublishTimer || !healthPublishTimer) {
        LOG_ERROR(TAG, "Failed to create timers");
        // Cleanup any timer that was created
        if (sensorPublishTimer) {
            xTimerDelete(sensorPublishTimer, 0);
            sensorPublishTimer = nullptr;
        }
        if (healthPublishTimer) {
            xTimerDelete(healthPublishTimer, 0);
            healthPublishTimer = nullptr;
        }
        vTaskDelete(NULL);
        return;
    }
    
    // Wait for network connection
    LOG_INFO(TAG, "Waiting for network connection...");
    while (!EthernetManager::isConnected()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    // Network is up, proceed immediately
    LOG_INFO(TAG, "Network up, initializing MQTT...");
    
    // Initialize MQTT
    initializeMQTT();

    if (mqttManager_ == nullptr) {
        LOG_ERROR(TAG, "MQTT initialization failed");
        // Round 14 Issue #10: Cleanup timers before deleting task
        if (sensorPublishTimer) {
            xTimerDelete(sensorPublishTimer, 0);
            sensorPublishTimer = nullptr;
        }
        if (healthPublishTimer) {
            xTimerDelete(healthPublishTimer, 0);
            healthPublishTimer = nullptr;
        }
        if (mqttTaskEventGroup) {
            vEventGroupDelete(mqttTaskEventGroup);
            mqttTaskEventGroup = nullptr;
        }
        vTaskDelete(NULL);
        return;
    }
    
    // Connect to MQTT broker
    auto connectResult = mqttManager_->connect();
    if (!connectResult.isOk()) {
        LOG_ERROR(TAG, "Initial MQTT connection failed");
    }
    
    // Start timers
    if (xTimerStart(sensorPublishTimer, pdMS_TO_TICKS(100)) != pdPASS ||
        xTimerStart(healthPublishTimer, pdMS_TO_TICKS(100)) != pdPASS) {
        LOG_ERROR(TAG, "Failed to start timers");
        // Cleanup timers
        xTimerDelete(sensorPublishTimer, 0);
        xTimerDelete(healthPublishTimer, 0);
        sensorPublishTimer = nullptr;
        healthPublishTimer = nullptr;
        vTaskDelete(NULL);
        return;
    }
    
    // Register with watchdog after initialization
    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::enabled(
        false,  // not critical (won't reset system)
        SystemConstants::System::WDT_MQTT_TASK_MS
    );

    if (!SRP::getTaskManager().registerCurrentTaskWithWatchdog("MQTTTask", wdtConfig)) {
        LOG_ERROR(TAG, "WDT reg failed");
    } else {
        LOG_INFO(TAG, "WDT OK %lums", SystemConstants::System::WDT_MQTT_TASK_MS);
        (void)SRP::getTaskManager().feedWatchdog();  // Feed immediately
    }
    
    // Main event loop
    const EventBits_t ALL_EVENTS = MQTT_TASK_CONNECTED | MQTT_TASK_DISCONNECTED |
                                   MQTT_TASK_MESSAGE | MQTT_TASK_PUBLISH_SENSORS |
                                   MQTT_TASK_PUBLISH_HEALTH | MQTT_TASK_PROCESS_QUEUE |
                                   MQTT_TASK_RETRY_SUBSCRIPTIONS;

    // Create subscription retry timer (one-shot, started on subscription failure)
    subscriptionRetryTimer = xTimerCreate(
        "SubRetry",
        pdMS_TO_TICKS(SUBSCRIPTION_RETRY_DELAY_MS),
        pdFALSE,  // One-shot
        nullptr,
        subscriptionRetryTimerCallback
    );
    
    bool subscriptionsSetup = false;
    
    while (true) {
        // Wait for events with timeout to check queue periodically
        EventBits_t events = xEventGroupWaitBits(
            mqttTaskEventGroup,
            ALL_EVENTS,
            pdTRUE,   // Clear bits on exit
            pdFALSE,  // Wait for any bit
            pdMS_TO_TICKS(100)  // 100ms timeout for queue processing
        );
        
        // Handle connection events
        if (events & MQTT_TASK_CONNECTED) {
            LOG_INFO(TAG, "MQTT connected");

            // Reset circuit breaker state on successful connection
            consecutiveDisconnects_.store(0);
            circuitBreakerOpen_.store(false);

            // Signal MQTT operational to other tasks (e.g., PersistentStorageTask)
            SRP::setSystemStateEventBits(SystemEvents::SystemState::MQTT_OPERATIONAL);
            // Debug: confirm operational bit was set
            LOG_INFO(TAG, "MQTT_OPERATIONAL bit set");

            // Always re-subscribe on connect (subscriptions may be lost after reconnect)
            bool wasSetup = subscriptionsSetup;
            setupSubscriptions();
            subscriptionsSetup = true;
            LOG_INFO(TAG, "Subscriptions %s", wasSetup ? "refreshed on reconnect" : "setup completed");

            // Publish online status and device info
            auto ipStr = ETH.localIP().toString();
            MQTTTask::publish(MQTT_STATUS_ONLINE, "{\"online\":true}", 0, true, MQTTPriority::PRIORITY_HIGH);
            MQTTTask::publish(MQTT_STATUS_DEVICE_IP, ipStr.c_str(), 0, true, MQTTPriority::PRIORITY_MEDIUM);
            MQTTTask::publish(MQTT_STATUS_DEVICE_HOSTNAME, DEVICE_HOSTNAME, 0, true, MQTTPriority::PRIORITY_MEDIUM);
            MQTTTask::publish(MQTT_STATUS_DEVICE_FIRMWARE, FIRMWARE_VERSION, 0, true, MQTTPriority::PRIORITY_MEDIUM);

            // Publish initial system state so Home Assistant knows current state
            vTaskDelay(pdMS_TO_TICKS(100));  // Small delay to ensure online status is sent first
            publishSystemState();

            // Publish initial room target temperature for Home Assistant climate entity
            {
                SystemSettings& settings = SRP::getSystemSettings();
                float targetTemp = tempToFloat(settings.targetTemperatureInside);
                char response[32];
                snprintf(response, sizeof(response), "%.1f", targetTemp);
                MQTTTask::publish(MQTT_STATUS_HEATING "/target", response, 0, true, MQTTPriority::PRIORITY_HIGH);
                LOG_INFO(TAG, "Published initial room target: %.1f°C", targetTemp);
            }
        }

        if (events & MQTT_TASK_DISCONNECTED) {
            LOG_WARN(TAG, "MQTT disconnected");
            subscriptionsSetup = false;
            failedSubscriptions = 0;  // Reset failed subscriptions on disconnect
            subscriptionRetryAttempts = 0;  // Round 14 Issue #14: Reset retry counter
            // Stop retry timer if running
            if (subscriptionRetryTimer) {
                xTimerStop(subscriptionRetryTimer, 0);
            }
            // Clear MQTT operational bit
            SRP::clearSystemStateEventBits(SystemEvents::SystemState::MQTT_OPERATIONAL);

            // Round 20 Issue #5: Circuit breaker with atomic operations
            uint8_t disconnects = consecutiveDisconnects_.fetch_add(1) + 1;  // Atomic increment
            if (disconnects >= MAX_RECONNECT_ATTEMPTS && !circuitBreakerOpen_.exchange(true)) {
                // Only first thread to set circuitBreakerOpen enters here
                circuitBreakerCooldownEnd_ = millis() + CIRCUIT_BREAKER_COOLDOWN_MS;
                LOG_ERROR(TAG, "Circuit breaker OPEN: %d consecutive failures, "
                         "cooldown for %lu minutes",
                         disconnects, CIRCUIT_BREAKER_COOLDOWN_MS / 60000);

                // Disable auto-reconnect during cooldown
                if (mqttManager_) {
                    mqttManager_->setAutoReconnect(false, {});
                }
            }
        }

        // Circuit breaker: check if cooldown has expired
        if (circuitBreakerOpen_.load() && millis() >= circuitBreakerCooldownEnd_) {
            LOG_INFO(TAG, "Circuit breaker cooldown expired - resuming reconnection");
            circuitBreakerOpen_.store(false);
            consecutiveDisconnects_.store(0);

            // Re-enable auto-reconnect
            if (mqttManager_) {
                MQTTManager::ReconnectConfig reconnectConfig;
                reconnectConfig.minInterval = MIN_RECONNECT_INTERVAL_MS;
                reconnectConfig.maxInterval = MAX_RECONNECT_INTERVAL_MS;
                reconnectConfig.maxAttempts = MAX_RECONNECT_ATTEMPTS;
                reconnectConfig.exponentialBackoff = true;
                mqttManager_->setAutoReconnect(true, reconnectConfig);

                // Attempt immediate reconnection
                auto result = mqttManager_->connect();
                if (!result.isOk()) {
                    LOG_WARN(TAG, "Reconnection attempt after cooldown failed");
                }
            }
        }

        // Handle subscription retry
        if ((events & MQTT_TASK_RETRY_SUBSCRIPTIONS) && mqttManager_ && mqttManager_->isConnected()) {
            if (failedSubscriptions != 0) {
                LOG_INFO(TAG, "Retrying failed subscriptions (mask: 0x%02X)", failedSubscriptions);
                retryFailedSubscriptions();
            }
        }

        // Process messages when connected
        if (mqttManager_ != nullptr && mqttManager_->isConnected()) {
            // Process incoming messages
            if (events & MQTT_TASK_MESSAGE) {
                processMQTT();
            }
            
            // Process publish queue
            processPublishQueue();
            
            // Check if more queue processing needed
            if (highPriorityQueue_ && highPriorityQueue_->getMessagesWaiting() > 0) {
                xEventGroupSetBits(mqttTaskEventGroup, MQTT_TASK_PROCESS_QUEUE);
            }
            
            // Handle sensor publishing
            if (events & MQTT_TASK_PUBLISH_SENSORS) {
                publishSensorData();
                LOG_DEBUG(TAG, "Published sensor data (timer)");
            }
            
            // Handle health publishing
            if (events & MQTT_TASK_PUBLISH_HEALTH) {
                publishSystemStatus();
                // Note: publishSystemState() is NOT called periodically
                // It's only published on connect and when state actually changes
                // (state changes are published by handleControlCommand when processing commands)
                LOG_DEBUG(TAG, "Published system health (timer)");
            }
            
            // Event-driven sensor publishing removed - rely on 10-second timer interval
            // This prevents irregular publishing intervals caused by overlapping sensor update timings
        }

        // Periodic queue health check, backpressure evaluation, and logging
        {
            // Update backpressure state (this sets/clears event bits)
            bool underPressure = isUnderPressure();

            uint32_t now = millis();
            if (now - queueHealthTracking.lastHealthLogTime > MQTTConstants::QUEUE_HEALTH_LOG_INTERVAL_MS) {
                queueHealthTracking.lastHealthLogTime = now;

                // Get queue health from QueueManager
                if (highPriorityQueue_ && normalPriorityQueue_) {
                    const auto& highMetrics = highPriorityQueue_->getMetrics();
                    const auto& normalMetrics = normalPriorityQueue_->getMetrics();

                    // Log queue health status (always log utilization for monitoring)
                    uint32_t totalDropped = highMetrics.getTotalDropped() + normalMetrics.getTotalDropped();
                    uint8_t util = getQueueUtilization();

                    if (totalDropped > 0 || underPressure || !highMetrics.isHealthy() || !normalMetrics.isHealthy()) {
                        LOG_INFO(TAG, "Queue health: util=%u%% pressure=%s H[%u/%u drop:%lu] N[%u/%u drop:%lu]",
                                 util, underPressure ? "YES" : "no",
                                 highPriorityQueue_->getMessagesWaiting(), HIGH_PRIORITY_QUEUE_SIZE,
                                 highMetrics.getTotalDropped(),
                                 normalPriorityQueue_->getMessagesWaiting(), NORMAL_PRIORITY_QUEUE_SIZE,
                                 normalMetrics.getTotalDropped());
                    }
                }
            }
        }

        // Feed watchdog
        (void)SRP::getTaskManager().feedWatchdog();
    }
}

// Public interface implementations
bool MQTTTask::init() {
    if (mqttMutex_ != nullptr) {
        return true; // Already initialized
    }

    // Round 16 Issue D: Document expected heap allocation (~6KB total)
    // - Mutex: ~96 bytes
    // - Queue structures: ~1.5KB (high + normal priority queues + FreeRTOS overhead)
    // - Memory pools: ~2.3KB (3×512 + 3×256 byte buffers, allocated lazily)
    // - Subscription callbacks: ~1KB (std::function objects)
    // - ArduinoJson temp: ~1KB (during subscription setup)
    // This is a one-time allocation at boot - no leaks expected

    mqttMutex_ = xSemaphoreCreateMutex();
    if (mqttMutex_ == nullptr) {
        LOG_ERROR(TAG, "Failed to create mutex");
        return false;
    }
    
    // Create publish queues
    QueueManager::QueueConfig highPriorityConfig;
    highPriorityConfig.length = HIGH_PRIORITY_QUEUE_SIZE;
    highPriorityConfig.itemSize = sizeof(MQTTPublishRequest);
    highPriorityConfig.overflowStrategy = QueueManager::OverflowStrategy::DROP_OLDEST;
    highPriorityQueue_ = QueueManager::getInstance().createQueue("mqtt_high_priority", highPriorityConfig);
    
    QueueManager::QueueConfig normalPriorityConfig;
    normalPriorityConfig.length = NORMAL_PRIORITY_QUEUE_SIZE;
    normalPriorityConfig.itemSize = sizeof(MQTTPublishRequest);
    normalPriorityConfig.overflowStrategy = QueueManager::OverflowStrategy::DROP_OLDEST;
    normalPriorityQueue_ = QueueManager::getInstance().createQueue("mqtt_normal_priority", normalPriorityConfig);
    
    if (!highPriorityQueue_ || !normalPriorityQueue_) {
        LOG_ERROR(TAG, "Failed to create queues");
        return false;
    }
    
    return true;
}

bool MQTTTask::start() {
    if (!init()) {
        return false;
    }
    
    if (isRunning_) {
        return true;
    }
    
    // Use TaskManager for proper watchdog integration
    // Task will manually register watchdog from its own context
    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::disabled();
    
    bool result = SRP::getTaskManager().startTaskPinned(
        taskFunction,
        "MQTTTask",
        STACK_SIZE_MQTT_TASK,
        nullptr,
        PRIORITY_CONTROL_TASK,  // Use standard control task priority
        1,  // Pin to core 1
        wdtConfig
    );
    
    if (result) {
        // Get the task handle after creation
        taskHandle_ = SRP::getTaskManager().getTaskHandleByName("MQTTTask");
    }
    
    if (!result) {
        LOG_ERROR(TAG, "Failed to create task");
        return false;
    }
    
    isRunning_ = true;
    return true;
}

void MQTTTask::stop() {
    if (!isRunning_ || taskHandle_ == nullptr) {
        return;
    }
    
    isRunning_ = false;
    
    if (mqttManager_ != nullptr) {
        mqttManager_->disconnect();
    }
    
    // Delete timers
    if (sensorPublishTimer) {
        xTimerDelete(sensorPublishTimer, pdMS_TO_TICKS(100));
        sensorPublishTimer = nullptr;
    }
    if (healthPublishTimer) {
        xTimerDelete(healthPublishTimer, pdMS_TO_TICKS(100));
        healthPublishTimer = nullptr;
    }
    
    // Delete event group
    if (mqttTaskEventGroup) {
        vEventGroupDelete(mqttTaskEventGroup);
        mqttTaskEventGroup = nullptr;
    }
    
    vTaskDelete(taskHandle_);
    taskHandle_ = nullptr;
}

bool MQTTTask::isRunning() {
    return isRunning_;
}

TaskHandle_t MQTTTask::getTaskHandle() {
    return taskHandle_;
}

MQTTManager* MQTTTask::getMQTTManager() {
    return mqttManager_;
}

bool MQTTTask::isConnected() {
    return mqttManager_ != nullptr && mqttManager_->isConnected();
}

bool MQTTTask::publish(const char* topic, const char* payload, int qos, bool retain, MQTTPriority priority) {
    if (!isRunning_ || !topic || !payload) {
        return false;
    }

    // CRITICAL messages bypass the queue when under pressure to ensure safety commands are delivered
    if (priority == MQTTPriority::PRIORITY_CRITICAL && isUnderPressure()) {
        // Direct publish for safety-critical messages when queue is congested
        if (mqttManager_ && mqttManager_->isConnected()) {
            auto result = mqttManager_->publish(topic, payload, qos, retain);
            if (result.isOk()) {
                LOG_INFO(TAG, "CRITICAL message bypassed queue: %s", topic);
                return true;
            }
            LOG_WARN(TAG, "CRITICAL message bypass failed, falling back to queue: %s", topic);
            // Fall through to queue the message
        }
    }

    // Apply backpressure for non-critical messages
    if (shouldThrottle(priority)) {
        // Track throttled messages for logging
        static uint32_t throttledCount = 0;
        static uint32_t lastThrottleLogTime = 0;
        throttledCount++;

        uint32_t now = millis();
        if (now - lastThrottleLogTime > 10000) {  // Log every 10 seconds
            LOG_WARN(TAG, "Backpressure active: throttled %lu messages", throttledCount);
            lastThrottleLogTime = now;
            throttledCount = 0;
        }
        return false;  // Message skipped due to backpressure
    }

    MQTTPublishRequest request;
    strncpy(request.topic, topic, sizeof(request.topic) - 1);
    request.topic[sizeof(request.topic) - 1] = '\0';
    strncpy(request.payload, payload, sizeof(request.payload) - 1);
    request.payload[sizeof(request.payload) - 1] = '\0';
    request.qos = qos;
    request.retain = retain;
    request.priority = priority;
    request.timestamp = xTaskGetTickCount();

    // Queue based on priority - CRITICAL and HIGH go to high priority queue
    bool queued = false;
    if ((priority == MQTTPriority::PRIORITY_CRITICAL || priority == MQTTPriority::PRIORITY_HIGH) && highPriorityQueue_) {
        queued = highPriorityQueue_->send(&request, 0);
        if (!queued) {
            queueHealthTracking.droppedHighPriority++;
        }
    } else if (normalPriorityQueue_) {
        queued = normalPriorityQueue_->send(&request, 0);
        if (!queued) {
            queueHealthTracking.droppedNormalPriority++;
        }
    }

    if (queued) {
        xEventGroupSetBits(mqttTaskEventGroup, MQTT_TASK_PROCESS_QUEUE);
    } else {
        // Log dropped messages periodically to avoid log spam
        uint32_t now = millis();
        if (now - queueHealthTracking.lastDropLogTime > MQTTConstants::QUEUE_DROP_LOG_INTERVAL_MS) {
            queueHealthTracking.lastDropLogTime = now;
            if (queueHealthTracking.droppedHighPriority > 0 || queueHealthTracking.droppedNormalPriority > 0) {
                LOG_WARN(TAG, "MQTT queue overflow - dropped H:%lu N:%lu messages",
                         queueHealthTracking.droppedHighPriority, queueHealthTracking.droppedNormalPriority);
                queueHealthTracking.droppedHighPriority = 0;
                queueHealthTracking.droppedNormalPriority = 0;
            }
        }
    }

    return queued;
}

bool MQTTTask::subscribe(const char* topic, std::function<void(const char*)> callback, int qos) {
    if (!isRunning_ || !mqttManager_ || !topic) {
        return false;
    }
    
    SemaphoreGuard guard(mqttMutex_);
    
    auto result = mqttManager_->subscribe(topic, 
        [callback](const String& payload) {
            if (callback) {
                callback(payload.c_str());
            }
        }, qos);
    
    return result.isOk();
}

// ============== Backpressure System ==============
// Thresholds for backpressure signaling
static constexpr uint8_t PRESSURE_THRESHOLD_HIGH = 80;    // 80% - start throttling MEDIUM priority
static constexpr uint8_t PRESSURE_THRESHOLD_MEDIUM = 50;  // 50% - start throttling LOW priority
static constexpr uint8_t PRESSURE_HYSTERESIS = 20;        // Must drop 20% below threshold to release pressure

// Track pressure state to apply hysteresis
static bool pressureStateActive = false;

uint8_t MQTTTask::getQueueUtilization() {
    if (!highPriorityQueue_ || !normalPriorityQueue_) {
        return 0;
    }

    // Calculate combined utilization (weighted: high priority queue matters more)
    size_t highUsed = highPriorityQueue_->getMessagesWaiting();
    size_t normalUsed = normalPriorityQueue_->getMessagesWaiting();
    size_t highTotal = HIGH_PRIORITY_QUEUE_SIZE;
    size_t normalTotal = NORMAL_PRIORITY_QUEUE_SIZE;

    // Weighted average: 60% weight to high priority queue
    float highUtil = (static_cast<float>(highUsed) / highTotal) * 100.0f;
    float normalUtil = (static_cast<float>(normalUsed) / normalTotal) * 100.0f;
    float combined = (highUtil * 0.6f) + (normalUtil * 0.4f);

    return static_cast<uint8_t>(combined);
}

bool MQTTTask::isUnderPressure() {
    uint8_t util = getQueueUtilization();

    // Apply hysteresis to avoid oscillation
    if (pressureStateActive) {
        // Must drop significantly below threshold to release
        if (util < (PRESSURE_THRESHOLD_MEDIUM - PRESSURE_HYSTERESIS)) {
            pressureStateActive = false;
            // Clear the pressure event bit
            EventGroupHandle_t generalEventGroup = SRP::getGeneralSystemEventGroup();
            if (generalEventGroup) {
                xEventGroupClearBits(generalEventGroup, SystemEvents::GeneralSystem::MQTT_QUEUE_PRESSURE);
            }
            LOG_INFO(TAG, "Queue pressure released (util: %u%%)", util);
        }
    } else {
        // Enter pressure state when exceeding threshold
        if (util >= PRESSURE_THRESHOLD_HIGH) {
            pressureStateActive = true;
            // Set the pressure event bit
            EventGroupHandle_t generalEventGroup = SRP::getGeneralSystemEventGroup();
            if (generalEventGroup) {
                xEventGroupSetBits(generalEventGroup, SystemEvents::GeneralSystem::MQTT_QUEUE_PRESSURE);
            }
            LOG_WARN(TAG, "Queue pressure HIGH (util: %u%%) - throttling non-critical messages", util);
        }
    }

    return pressureStateActive;
}

bool MQTTTask::shouldThrottle(MQTTPriority priority) {
    // CRITICAL and HIGH priority messages are never throttled
    if (priority == MQTTPriority::PRIORITY_CRITICAL || priority == MQTTPriority::PRIORITY_HIGH) {
        return false;
    }

    uint8_t util = getQueueUtilization();

    switch (priority) {
        case MQTTPriority::PRIORITY_MEDIUM:
            // Throttle MEDIUM when > 80% full
            return util >= PRESSURE_THRESHOLD_HIGH;

        case MQTTPriority::PRIORITY_LOW:
            // Throttle LOW when > 50% full
            return util >= PRESSURE_THRESHOLD_MEDIUM;

        default:
            return false;
    }
}

// Complete implementations from MQTTTask.cpp
void MQTTTask::handleReconnection() {
    // Auto-reconnect is handled by MQTTManager in event-driven mode
    // This function is kept for compatibility but does nothing
}

void MQTTTask::processPublishQueue() {
    if (!mqttManager_ || !mqttManager_->isConnected() || !highPriorityQueue_ || !normalPriorityQueue_) {
        return;
    }
    
    static uint32_t lastLogTime = 0;
    uint32_t now = millis();
    
    // Check queue sizes
    UBaseType_t highQueueItems = highPriorityQueue_->getMessagesWaiting();
    UBaseType_t normalQueueItems = normalPriorityQueue_->getMessagesWaiting();
    UBaseType_t totalItems = highQueueItems + normalQueueItems;
    
    if (now - lastLogTime > SystemConstants::Tasks::MQTT::QUEUE_STATUS_LOG_INTERVAL_MS && totalItems > 0) {
        LOG_DEBUG(TAG, "Queue: h:%d, n:%d", highQueueItems, normalQueueItems);
        lastLogTime = now;
    }

    // Return early if both queues are empty
    if (totalItems == 0) {
        return;
    }

    MQTTPublishRequest request;
    int processed = 0;
    const int maxPerIteration = SystemConstants::Tasks::MQTT::MAX_ITEMS_PER_ITERATION;

    // Process high priority queue first - process ALL high priority messages
    while (highQueueItems > 0 && processed < maxPerIteration &&
           highPriorityQueue_->receive(&request, 0)) {

        // Don't hold mutex while publishing - MQTTManager has its own mutex
        auto result = mqttManager_->publish(request.topic, request.payload, request.qos, request.retain);

        if (!result.isOk()) {
            LOG_WARN(TAG, "Failed to publish high priority to %s", request.topic);

            // If connection lost, put message back and stop processing
            if (result.error() == MQTTError::CONNECTION_FAILED) {
                // Can't put back in front with ManagedQueue - message is lost
                // This is acceptable as the queue will handle overflow based on strategy
                LOG_ERROR(TAG, "Connection lost, dropping message to %s", request.topic);

                // Propagate connection loss to monitoring systems
                SRP::clearSystemStateEventBits(SystemEvents::SystemState::MQTT_OPERATIONAL);
                xEventGroupSetBits(SRP::getErrorNotificationEventGroup(),
                                  SystemEvents::Error::NETWORK);

                return;  // Stop all processing if connection lost
            }
        } else {
            LOG_DEBUG(TAG, "Published high priority to %s", request.topic);
        }

        processed++;
        highQueueItems--;

        // Brief yield every 4 messages to allow other tasks - reduced from 10ms per message
        if (processed % 4 == 0 && highQueueItems > 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    
    // Process normal priority queue only if we have capacity left
    int normalProcessed = 0;
    while (normalQueueItems > 0 && (processed + normalProcessed) < maxPerIteration && 
           normalPriorityQueue_->receive(&request, 0)) {
        
        auto result = mqttManager_->publish(request.topic, request.payload, request.qos, request.retain);
        
        if (!result.isOk()) {
            LOG_WARN(TAG, "Failed to publish normal priority to %s", request.topic);
            
            // If connection lost, put message back and stop processing
            if (result.error() == MQTTError::CONNECTION_FAILED) {
                // Can't put back in front with ManagedQueue - message is lost
                // This is acceptable as the queue will handle overflow based on strategy
                LOG_ERROR(TAG, "Connection lost, dropping message to %s", request.topic);

                // Propagate connection loss to monitoring systems
                SRP::clearSystemStateEventBits(SystemEvents::SystemState::MQTT_OPERATIONAL);
                xEventGroupSetBits(SRP::getErrorNotificationEventGroup(),
                                  SystemEvents::Error::NETWORK);

                break;
            }
        } else {
            LOG_DEBUG(TAG, "Published normal priority to %s", request.topic);
        }
        
        normalProcessed++;
        normalQueueItems--;

        // Brief yield every 4 messages to allow other tasks
        if (normalProcessed % 4 == 0 && normalQueueItems > 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    
    if ((processed + normalProcessed) > 0) {
        LOG_DEBUG(TAG, "Processed %d high, %d normal priority messages. Remaining: high:%d, normal:%d", 
                  processed, normalProcessed, 
                  highPriorityQueue_->getMessagesWaiting(), 
                  normalPriorityQueue_->getMessagesWaiting());
    }
}

void MQTTTask::publishSystemStatus() {
    if (!mqttManager_->isConnected()) {
        return;
    }
    
    SemaphoreGuard guard(mqttMutex_, pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "Failed to acquire mutex for status publish");
        return;
    }
    
    // Create JSON status message
    JsonDocument doc;  // ArduinoJson v7
    doc["timestamp"] = millis();

    uint32_t heapFree = ESP.getFreeHeap();
    uint32_t heapMaxBlock = ESP.getMaxAllocHeap();
    doc["heap_free"] = heapFree;
    doc["heap_min"] = ESP.getMinFreeHeap();
    doc["heap_max_blk"] = heapMaxBlock;  // Largest free block (for fragmentation analysis)

    // Round 16 Issue #5: Add heap fragmentation percentage
    // Fragmentation = 100 - (max_block * 100 / free_heap)
    // Lower is better: 0% = no fragmentation, 100% = completely fragmented
    // Use 64-bit arithmetic to prevent overflow on large heap values
    uint8_t fragPct = (heapFree > 0) ?
        (100 - static_cast<uint8_t>((static_cast<uint64_t>(heapMaxBlock) * 100ULL) / heapFree)) : 100;
    doc["heap_frag"] = fragPct;

    doc["uptime"] = millis() / 1000;
    
    // Add health monitor data
    // Add basic health data - HealthMonitor not available in this version
    doc["health"]["tasks"] = uxTaskGetNumberOfTasks();
    doc["health"]["stack_hwm"] = uxTaskGetStackHighWaterMark(NULL);
    
    auto buffer = MemoryPools::getLogBuffer();
    if (!buffer) {
        LOG_ERROR(TAG, "Failed to allocate buffer for health data");
        return;
    }

    size_t written = serializeJson(doc, buffer.data(), buffer.size());
    if (written == 0 || written >= buffer.size()) {
        LOG_ERROR(TAG, "JSON serialization failed or truncated for health data");
        return;
    }

    // Queue for publishing with MEDIUM priority
    MQTTTask::publish(MQTT_STATUS_HEALTH, buffer.c_str(), 0, false, MQTTPriority::PRIORITY_MEDIUM);
}

void MQTTTask::publishSystemState() {
    // System state is now included in sensor data as compact byte "s"
    // s = bit0:system_enabled, bit1:heating_enabled, bit2:heating_on,
    //     bit3:water_enabled, bit4:water_on, bit5:water_priority
    // Individual state topics are published by handleControlCommand() when state changes
    // This function is kept for compatibility but does nothing - state is in sensor data
    LOG_DEBUG(TAG, "System state included in sensor data 's' field");
}

void MQTTTask::publishSensorData() {
    if (!mqttManager_->isConnected()) {
        return;
    }
    
    // Get sensor data with timeout to avoid blocking
    SemaphoreGuard guard(SRP::getSensorReadingsMutex(), pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "Failed to acquire sensor mutex for MQTT publish");
        return;
    }
    
    // Create local copy of sensor data
    SharedSensorReadings sensors = SRP::getSensorReadings();
    
    // SemaphoreGuard releases automatically when it goes out of scope
    
    // Create JSON message
    JsonDocument doc;  // ArduinoJson v7

    // Temperature values are in tenths of degrees Celsius (int16_t)
    // Example: 273 = 27.3°C, -50 = -5.0°C

    // Compact format: Use shorter keys for smaller payload
    JsonObject temps = doc["t"].to<JsonObject>();  // temperatures
    temps["bo"] = sensors.boilerTempOutput;     // boiler output
    temps["br"] = sensors.boilerTempReturn;     // boiler return
    temps["wt"] = sensors.waterHeaterTempTank;  // water tank
    temps["o"] = sensors.outsideTemp;           // outside

    // Optional sensors (enable via ENABLE_SENSOR_* flags)
#ifdef ENABLE_SENSOR_WATER_TANK_TOP
    temps["wtt"] = sensors.waterTankTopTemp;    // water tank top
#endif
#ifdef ENABLE_SENSOR_WATER_RETURN
    temps["wr"] = sensors.waterHeaterTempReturn; // water return
#endif
#ifdef ENABLE_SENSOR_HEATING_RETURN
    temps["hr"] = sensors.heatingTempReturn;    // heating return
#endif
    
    // Only include inside temp if valid
    if (tempIsValid(sensors.insideTemp)) {
        temps["i"] = sensors.insideTemp;        // inside
    }

    // Add burner target temperature (from BurnerStateMachine)
    bool burnerDemand = false;
    Temperature_t burnerTarget = 0;
    if (BurnerStateMachine::getHeatDemandState(burnerDemand, burnerTarget)) {
        temps["bt"] = burnerTarget;             // burner target
    }

    // Include system pressure if valid (in hundredths of BAR for precision)
    if (sensors.isSystemPressureValid) {
        doc["p"] = sensors.systemPressure;      // pressure in hundredths of BAR
    }
    
    // Get relay status
    if (xSemaphoreTake(SRP::getRelayReadingsMutex(), pdMS_TO_TICKS(50)) == pdTRUE) {
        SharedRelayReadings relays = SRP::getRelayReadings();
        xSemaphoreGive(SRP::getRelayReadingsMutex());
        
        // Compact format: combine relay states into a single byte
        // Bit 0: burner, 1: heating_pump, 2: water_pump, 3: half_power, 4: water_mode
        uint8_t relayBits = 0;
        if (relays.relayBurnerEnable) relayBits |= 0x01;
        if (relays.relayHeatingPump) relayBits |= 0x02;
        if (relays.relayWaterPump) relayBits |= 0x04;
        if (relays.relayPowerBoost) relayBits |= 0x08;
        if (relays.relayWaterMode) relayBits |= 0x10;
        
        doc["r"] = relayBits;  // relays as single byte
    }

    // Add system state as compact byte (like relays)
    // s = system state: bit0=system_enabled, bit1=heating_enabled, bit2=heating_on,
    //                   bit3=water_enabled, bit4=water_on, bit5=water_priority
    EventBits_t systemState = SRP::getSystemStateEventBits();
    uint8_t stateBits = 0;
    if (systemState & SystemEvents::SystemState::BOILER_ENABLED) stateBits |= 0x01;
    if (systemState & SystemEvents::SystemState::HEATING_ENABLED) stateBits |= 0x02;
    if (systemState & SystemEvents::SystemState::HEATING_ON) stateBits |= 0x04;
    if (systemState & SystemEvents::SystemState::WATER_ENABLED) stateBits |= 0x08;
    if (systemState & SystemEvents::SystemState::WATER_ON) stateBits |= 0x10;
    if (systemState & SystemEvents::SystemState::WATER_PRIORITY) stateBits |= 0x20;
    doc["s"] = stateBits;

    // Add sensor fallback status for degraded mode notification
    // sf = sensor fallback: 0=STARTUP (waiting), 1=NORMAL (OK), 2=SHUTDOWN (degraded)
    auto fallbackMode = TemperatureSensorFallback::getCurrentMode();
    doc["sf"] = static_cast<uint8_t>(fallbackMode);

    // If in degraded mode, add which sensors are missing
    if (fallbackMode != TemperatureSensorFallback::FallbackMode::NORMAL) {
        const auto& status = TemperatureSensorFallback::getStatus();
        uint8_t missingSensors = 0;
        if (status.missingBoilerOutput) missingSensors |= 0x01;
        if (status.missingBoilerReturn) missingSensors |= 0x02;
        if (status.missingWaterTemp) missingSensors |= 0x04;
        if (status.missingRoomTemp) missingSensors |= 0x08;
        doc["sm"] = missingSensors;  // sensor missing bits
    }

    // Use the larger JSON buffer pool for sensor data
    auto buffer = MemoryPools::jsonBufferPool.allocate();
    if (!buffer) {
        LOG_ERROR(TAG, "Failed to allocate buffer for sensor data");
        return;
    }

    size_t written = serializeJson(doc, buffer->data, sizeof(buffer->data));
    if (written == 0 || written >= sizeof(buffer->data)) {
        LOG_ERROR(TAG, "JSON serialization failed or truncated (wrote %zu/%zu bytes)",
                 written, sizeof(buffer->data));
        MemoryPools::jsonBufferPool.deallocate(buffer);
        return;
    }

    // Queue for publishing with HIGH priority
    MQTTTask::publish(MQTT_STATUS_SENSORS, buffer->data, 0, false, MQTTPriority::PRIORITY_HIGH);
    
    // Return buffer to pool
    MemoryPools::jsonBufferPool.deallocate(buffer);
}

// Helper to schedule subscription retry
static void scheduleSubscriptionRetry() {
    if (failedSubscriptions != 0 && subscriptionRetryTimer) {
        xTimerStart(subscriptionRetryTimer, 0);
        LOG_INFO(TAG, "Scheduled subscription retry in %lu ms", SUBSCRIPTION_RETRY_DELAY_MS);
    }
}

// Retry only the failed subscriptions
void MQTTTask::retryFailedSubscriptions() {
    if (!mqttManager_ || !mqttManager_->isConnected()) {
        return;
    }

    uint8_t previousFailures = failedSubscriptions;

    // Retry test/echo if it failed
    if (failedSubscriptions & SUB_TEST_ECHO) {
        auto result = mqttManager_->subscribe("test/echo",
            [](const String& topic, const String& payload) {
                LOG_INFO(TAG, "Echo test: %s", payload.c_str());
                MQTTTask::publish("test/response", payload.c_str(), 0, false, MQTTPriority::PRIORITY_LOW);
            }, 0);
        if (result.isOk()) {
            failedSubscriptions &= ~SUB_TEST_ECHO;
            LOG_INFO(TAG, "Retry: test/echo succeeded");
        }
    }

    // Retry command topics if failed
    if (failedSubscriptions & SUB_CMD) {
        char cmdTopic[64];
        snprintf(cmdTopic, sizeof(cmdTopic), "%s/+", MQTT_CMD_PREFIX);
        auto result = mqttManager_->subscribe(cmdTopic,
            [](const String& topic, const String& payload) {
                handleControlCommand(topic.c_str(), payload.c_str());
            }, 0);
        if (result.isOk()) {
            failedSubscriptions &= ~SUB_CMD;
            LOG_INFO(TAG, "Retry: %s succeeded", cmdTopic);
        }
    }

    // Retry config topics if failed
    if (failedSubscriptions & SUB_CONFIG) {
        char configTopic[64];
        snprintf(configTopic, sizeof(configTopic), "%s/+", MQTT_CONFIG_PREFIX);
        auto result = mqttManager_->subscribe(configTopic,
            [](const String& topic, const String& payload) {
                LOG_INFO(TAG, "Configuration update on %s: %s", topic.c_str(), payload.c_str());
            }, 0);
        if (result.isOk()) {
            failedSubscriptions &= ~SUB_CONFIG;
            LOG_INFO(TAG, "Retry: %s succeeded", configTopic);
        }
    }

    // Retry errors topics if failed
    if (failedSubscriptions & SUB_ERRORS) {
        auto result = mqttManager_->subscribe("errors/+",
            [](const String& topic, const String& payload) {
                handleErrorCommand(topic.c_str(), payload.c_str());
            }, 0);
        if (result.isOk()) {
            failedSubscriptions &= ~SUB_ERRORS;
            LOG_INFO(TAG, "Retry: errors/+ succeeded");
        }
    }

    // Retry scheduler topics if failed
    if (failedSubscriptions & SUB_SCHEDULER) {
        char schedulerTopic[64];
        snprintf(schedulerTopic, sizeof(schedulerTopic), "%s/+", MQTT_CMD_SCHEDULER_PREFIX);
        auto result = mqttManager_->subscribe(schedulerTopic,
            [](const String& topic, const String& payload) {
                handleSchedulerCommand(topic.c_str(), payload.c_str());
            }, 0);
        if (result.isOk()) {
            failedSubscriptions &= ~SUB_SCHEDULER;
            LOG_INFO(TAG, "Retry: %s succeeded", schedulerTopic);
        }
    }

    // Schedule another retry if still have failures
    if (failedSubscriptions != 0) {
        LOG_WARN(TAG, "Still have failed subscriptions (0x%02X) - scheduling retry", failedSubscriptions);
        scheduleSubscriptionRetry();
    } else if (previousFailures != 0) {
        LOG_INFO(TAG, "All subscriptions recovered!");
        // Round 14 Issue #14: Reset retry counter on full recovery
        subscriptionRetryAttempts = 0;
    }
}

void MQTTTask::setupSubscriptions() {
    if (!mqttManager_ || !mqttManager_->isConnected()) {
        LOG_ERROR(TAG, "Cannot setup subscriptions - MQTT not connected");
        return;
    }

    // Reset failure tracking for fresh setup
    failedSubscriptions = 0;

    // SUBACK verification is now implemented in MQTTManager v1.6.0 / ESP32MQTTClient v1.1.0
    // - MQTTManager::subscribe() returns success when request is sent
    // - MQTT_EVENT_SUBSCRIBED callback fires when SUBACK received
    // - Use MQTTManager::isSubscriptionConfirmed(topic) to verify broker acknowledged
    // - Retry mechanism handles transient failures and broker rejections
    LOG_INFO(TAG, "Setting up MQTT subscriptions...");

    // Subscribe to test topic
    LOG_INFO(TAG, "Step 1/5: Subscribing to test/echo");

    auto result = mqttManager_->subscribe("test/echo",
        [](const String& topic, const String& payload) {
            LOG_INFO(TAG, "Echo test: %s", payload.c_str());
            // Echo back the message
            MQTTTask::publish("test/response", payload.c_str(), 0, false, MQTTPriority::PRIORITY_LOW);
        }, 0);  // QoS 0

    if (!result.isOk()) {
        LOG_ERROR(TAG, "Failed to subscribe to test/echo, error: %d", static_cast<int>(result.error()));
        failedSubscriptions |= SUB_TEST_ECHO;
    } else {
        LOG_INFO(TAG, "Successfully subscribed to test/echo");
    }

    // Subscribe to command topics
    LOG_INFO(TAG, "Step 2/5: Subscribing to %s/+", MQTT_CMD_PREFIX);

    char cmdTopic[64];
    snprintf(cmdTopic, sizeof(cmdTopic), "%s/+", MQTT_CMD_PREFIX);

    result = mqttManager_->subscribe(cmdTopic,
        [](const String& topic, const String& payload) {
            handleControlCommand(topic.c_str(), payload.c_str());
        }, 0);  // QoS 0

    if (!result.isOk()) {
        LOG_ERROR(TAG, "Failed to subscribe to %s, error: %d", cmdTopic, static_cast<int>(result.error()));
        failedSubscriptions |= SUB_CMD;
    } else {
        LOG_INFO(TAG, "Successfully subscribed to %s", cmdTopic);
    }

    // Subscribe to command config topics (e.g., boiler/cmd/config/syslog_enabled)
    char cmdConfigTopic[64];
    snprintf(cmdConfigTopic, sizeof(cmdConfigTopic), "%s/config/+", MQTT_CMD_PREFIX);

    result = mqttManager_->subscribe(cmdConfigTopic,
        [](const String& topic, const String& payload) {
            handleControlCommand(topic.c_str(), payload.c_str());
        }, 0);  // QoS 0

    if (!result.isOk()) {
        LOG_ERROR(TAG, "Failed to subscribe to %s, error: %d", cmdConfigTopic, static_cast<int>(result.error()));
    } else {
        LOG_INFO(TAG, "Successfully subscribed to %s", cmdConfigTopic);
    }

    // Subscribe to configuration updates
    LOG_INFO(TAG, "Step 3/5: Subscribing to %s/+", MQTT_CONFIG_PREFIX);

    char configTopic[64];
    snprintf(configTopic, sizeof(configTopic), "%s/+", MQTT_CONFIG_PREFIX);

    result = mqttManager_->subscribe(configTopic,
        [](const String& topic, const String& payload) {
            LOG_INFO(TAG, "Configuration update on %s: %s", topic.c_str(), payload.c_str());
        }, 0);  // QoS 0

    if (!result.isOk()) {
        LOG_ERROR(TAG, "Failed to subscribe to %s, error: %d", configTopic, static_cast<int>(result.error()));
        failedSubscriptions |= SUB_CONFIG;
    } else {
        LOG_INFO(TAG, "Successfully subscribed to %s", configTopic);
    }

    // Subscribe to error management commands
    LOG_INFO(TAG, "Step 4/5: Subscribing to errors/+");

    result = mqttManager_->subscribe("errors/+",
        [](const String& topic, const String& payload) {
            handleErrorCommand(topic.c_str(), payload.c_str());
        }, 0);  // QoS 0

    if (!result.isOk()) {
        LOG_ERROR(TAG, "Failed to subscribe to errors/+, error: %d", static_cast<int>(result.error()));
        failedSubscriptions |= SUB_ERRORS;
    } else {
        LOG_INFO(TAG, "Successfully subscribed to errors/+");
    }

    // Subscribe to scheduler commands
    LOG_INFO(TAG, "Step 5/5: Subscribing to %s/+", MQTT_CMD_SCHEDULER_PREFIX);

    char schedulerTopic[64];
    snprintf(schedulerTopic, sizeof(schedulerTopic), "%s/+", MQTT_CMD_SCHEDULER_PREFIX);

    result = mqttManager_->subscribe(schedulerTopic,
        [](const String& topic, const String& payload) {
            handleSchedulerCommand(topic.c_str(), payload.c_str());
        }, 0);  // QoS 0

    if (!result.isOk()) {
        LOG_ERROR(TAG, "Failed to subscribe to %s, error: %d", schedulerTopic, static_cast<int>(result.error()));
        failedSubscriptions |= SUB_SCHEDULER;
    } else {
        LOG_INFO(TAG, "Successfully subscribed to %s", schedulerTopic);
    }

    // Schedule retry if any subscriptions failed
    if (failedSubscriptions != 0) {
        LOG_WARN(TAG, "Some subscriptions failed (0x%02X) - scheduling retry", failedSubscriptions);
        scheduleSubscriptionRetry();
    } else {
        LOG_INFO(TAG, "MQTT subscriptions setup complete - all done!");
    }

    // Persistent Storage parameter topics are handled by PersistentStorageTask
}

void MQTTTask::handleControlCommand(const char* topic, const char* payload) {
    // Delegate to extracted command handlers
    MQTTCommandHandlers::routeControlCommand(topic, payload);
}

void MQTTTask::handleSchedulerCommand(const char* topic, const char* payload) {
    // Delegate to extracted command handlers
    MQTTCommandHandlers::handleSchedulerCommand(topic, payload);
}

void MQTTTask::handleErrorCommand(const char* topic, const char* payload) {
    // Delegate to extracted command handlers
    MQTTCommandHandlers::handleErrorCommand(topic, payload);
}
