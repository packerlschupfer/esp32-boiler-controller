// src/modules/mqtt/MQTTSubscriptionManager.cpp
#include "MQTTSubscriptionManager.h"
#include "MQTTTopics.h"
#include "modules/mqtt/MQTTCommandHandlers.h"
#include "modules/tasks/MQTTTask.h"
#include "core/SystemResourceProvider.h"
#include <esp_log.h>

static const char* TAG = "MQTTSubMgr";

namespace MQTTSubscriptionManager {

// ============================================================================
// THREAD-SAFETY NOTE:
// Variables in this anonymous namespace are SAFE because:
// - failedSubscriptions, subscriptionRetryTimer, subscriptionRetryAttempts:
//   Only accessed from MQTTTask::taskFunction() and timer callback
//   (timer runs in daemon task but only sets event bits, actual access is in task)
// - Timer callback only calls xEventGroupSetBits() which is thread-safe
// DO NOT access these variables from other tasks.
// ============================================================================
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

    // Event group for signaling retry needed
    static EventGroupHandle_t mqttTaskEventGroup = nullptr;
    static uint32_t retryEventBit = 0;

    // Limit subscription retry attempts
    static uint8_t subscriptionRetryAttempts = 0;
    static constexpr uint8_t MAX_SUBSCRIPTION_RETRIES = 10;

    // Timer callback - signals task to retry
    static void subscriptionRetryTimerCallback(TimerHandle_t xTimer) {
        (void)xTimer;

        // Stop retrying after max attempts
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
            xEventGroupSetBits(mqttTaskEventGroup, retryEventBit);
        }
    }

    // Helper to schedule subscription retry
    void scheduleSubscriptionRetry() {
        if (failedSubscriptions != 0 && subscriptionRetryTimer) {
            xTimerStart(subscriptionRetryTimer, 0);
            LOG_INFO(TAG, "Scheduled subscription retry in %lu ms", SUBSCRIPTION_RETRY_DELAY_MS);
        }
    }
}

void initialize(EventGroupHandle_t eventGroup, uint32_t retryBit) {
    mqttTaskEventGroup = eventGroup;
    retryEventBit = retryBit;

    // Create retry timer
    if (!subscriptionRetryTimer) {
        subscriptionRetryTimer = xTimerCreate(
            "SubRetry",
            pdMS_TO_TICKS(SUBSCRIPTION_RETRY_DELAY_MS),
            pdFALSE,  // One-shot timer
            nullptr,
            subscriptionRetryTimerCallback
        );

        if (!subscriptionRetryTimer) {
            LOG_ERROR(TAG, "Failed to create subscription retry timer");
        }
    }

    // Reset state
    failedSubscriptions = 0;
    subscriptionRetryAttempts = 0;

    LOG_INFO(TAG, "MQTTSubscriptionManager initialized");
}

void setupSubscriptions() {
    auto mqttManager = SRP::getMQTTManager();
    if (!mqttManager || !mqttManager->isConnected()) {
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

    auto result = mqttManager->subscribe("test/echo",
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

    result = mqttManager->subscribe(cmdTopic,
        [](const String& topic, const String& payload) {
            MQTTCommandHandlers::routeControlCommand(topic.c_str(), payload.c_str());
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

    result = mqttManager->subscribe(cmdConfigTopic,
        [](const String& topic, const String& payload) {
            MQTTCommandHandlers::routeControlCommand(topic.c_str(), payload.c_str());
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

    result = mqttManager->subscribe(configTopic,
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

    result = mqttManager->subscribe("errors/+",
        [](const String& topic, const String& payload) {
            MQTTCommandHandlers::handleErrorCommand(topic.c_str(), payload.c_str());
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

    result = mqttManager->subscribe(schedulerTopic,
        [](const String& topic, const String& payload) {
            MQTTCommandHandlers::handleSchedulerCommand(topic.c_str(), payload.c_str());
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

void retryFailedSubscriptions() {
    auto mqttManager = SRP::getMQTTManager();
    if (!mqttManager || !mqttManager->isConnected()) {
        return;
    }

    uint8_t previousFailures = failedSubscriptions;

    // Retry test/echo if it failed
    if (failedSubscriptions & SUB_TEST_ECHO) {
        auto result = mqttManager->subscribe("test/echo",
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
        auto result = mqttManager->subscribe(cmdTopic,
            [](const String& topic, const String& payload) {
                MQTTCommandHandlers::routeControlCommand(topic.c_str(), payload.c_str());
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
        auto result = mqttManager->subscribe(configTopic,
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
        auto result = mqttManager->subscribe("errors/+",
            [](const String& topic, const String& payload) {
                MQTTCommandHandlers::handleErrorCommand(topic.c_str(), payload.c_str());
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
        auto result = mqttManager->subscribe(schedulerTopic,
            [](const String& topic, const String& payload) {
                MQTTCommandHandlers::handleSchedulerCommand(topic.c_str(), payload.c_str());
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
        // Reset retry counter on full recovery
        subscriptionRetryAttempts = 0;
    }
}

bool allSubscriptionsSuccessful() {
    return failedSubscriptions == 0;
}

Stats getStats() {
    return Stats{
        .failedSubscriptions = failedSubscriptions,
        .retryAttempts = subscriptionRetryAttempts,
        .maxRetries = MAX_SUBSCRIPTION_RETRIES
    };
}

void cleanup() {
    if (subscriptionRetryTimer) {
        xTimerStop(subscriptionRetryTimer, 0);
        xTimerDelete(subscriptionRetryTimer, 0);
        subscriptionRetryTimer = nullptr;
    }
    failedSubscriptions = 0;
    subscriptionRetryAttempts = 0;
    LOG_INFO(TAG, "MQTTSubscriptionManager cleanup complete");
}

} // namespace MQTTSubscriptionManager
