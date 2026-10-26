// src/modules/mqtt/MQTTPublisher.h
#ifndef MQTT_PUBLISHER_H
#define MQTT_PUBLISHER_H

/**
 * @brief MQTT status and sensor data publishing
 *
 * Extracted from MQTTTask.cpp following the MQTTCommandHandlers pattern.
 * Handles periodic publishing of system status, sensor data, and health information.
 *
 * Thread Safety:
 * - All functions use SemaphoreGuard for mutex protection
 * - Safe to call from MQTTTask or timer callbacks
 * - Uses MQTTTask::publish() for actual MQTT transmission
 */
namespace MQTTPublisher {

/**
 * @brief Publish system status and health data
 *
 * Published to: boiler/status/health
 * Content: Memory stats, uptime, task count, stack high water mark
 * Priority: MEDIUM
 *
 * Includes heap fragmentation percentage calculation.
 */
void publishSystemStatus();

/**
 * @brief Publish sensor readings
 *
 * Published to: boiler/status/sensors
 * Content: Temperatures, pressure, relay states, system state
 * Priority: HIGH
 *
 * Uses compact JSON format with short keys to minimize payload size.
 */
void publishSensorData();

/**
 * @brief Publish system state (compatibility function)
 *
 * System state is now included in sensor data as compact byte "s".
 * This function is kept for compatibility but does nothing - state is in sensor data.
 */
void publishSystemState();

} // namespace MQTTPublisher

#endif // MQTT_PUBLISHER_H
