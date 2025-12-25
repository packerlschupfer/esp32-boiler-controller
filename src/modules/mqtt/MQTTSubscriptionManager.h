// src/modules/mqtt/MQTTSubscriptionManager.h
#ifndef MQTT_SUBSCRIPTION_MANAGER_H
#define MQTT_SUBSCRIPTION_MANAGER_H

#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"

/**
 * @brief MQTT subscription setup and retry management
 *
 * Extracted from MQTTTask.cpp following the MQTTCommandHandlers pattern.
 * Handles subscription setup on connect and automatic retry of failed subscriptions.
 *
 * Thread Safety:
 * - All functions should be called from MQTTTask only
 * - Timer callback is thread-safe (only sets event bits)
 * - Subscription state is protected by task serialization
 */
namespace MQTTSubscriptionManager {

/**
 * @brief Initialize subscription management
 * @param eventGroup Event group for triggering subscription retry
 * @param retryEventBit Event bit to set when retry is needed
 */
void initialize(EventGroupHandle_t eventGroup, uint32_t retryEventBit);

/**
 * @brief Setup all MQTT subscriptions on connect
 *
 * Subscribes to:
 * - test/echo (echo test)
 * - boiler/cmd/+ (control commands)
 * - boiler/cmd/config/+ (configuration commands)
 * - boiler/params/+ (parameter updates)
 * - errors/+ (error management)
 * - boiler/cmd/scheduler/+ (scheduler commands)
 *
 * Failed subscriptions are automatically retried.
 */
void setupSubscriptions();

/**
 * @brief Retry previously failed subscriptions
 *
 * Called periodically by timer or on-demand from task.
 * Retries only the subscriptions that failed in setupSubscriptions().
 */
void retryFailedSubscriptions();

/**
 * @brief Check if all subscriptions succeeded
 * @return true if no failed subscriptions, false otherwise
 */
bool allSubscriptionsSuccessful();

/**
 * @brief Get subscription statistics
 */
struct Stats {
    uint8_t failedSubscriptions;     // Bitmask of failed subscription groups
    uint8_t retryAttempts;           // Current retry attempt count
    uint8_t maxRetries;              // Maximum retry attempts allowed
};
Stats getStats();

/**
 * @brief Cleanup resources (call on task shutdown)
 */
void cleanup();

} // namespace MQTTSubscriptionManager

#endif // MQTT_SUBSCRIPTION_MANAGER_H
