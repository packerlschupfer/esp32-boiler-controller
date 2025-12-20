// src/modules/mqtt/MQTTCommandHandlers.h
#pragma once

/**
 * @file MQTTCommandHandlers.h
 * @brief MQTT command handler declarations
 *
 * This module handles MQTT command routing and processing for the boiler controller.
 * Commands are received on topics like boiler/cmd/{command} and processed here.
 */

namespace MQTTCommandHandlers {

/**
 * @brief Handle system control commands (enable/disable/reboot)
 * @param payload Command payload ("on", "off", "enable", "disable", "reboot")
 */
void handleSystemCommand(const char* payload);

/**
 * @brief Handle heating control commands
 * @param payload Command payload ("on", "off", "override_on", "override_off", or temperature)
 */
void handleHeatingCommand(const char* payload);

/**
 * @brief Handle room target temperature command
 * @param payload Temperature value as string (15.0 - 30.0)
 */
void handleRoomTargetCommand(const char* payload);

/**
 * @brief Handle water heater control commands
 * @param payload Command payload ("on", "off", "override_on", "override_off", "priority_on", etc.)
 */
void handleWaterCommand(const char* payload);

/**
 * @brief Handle PID auto-tuning commands
 * @param payload Command payload ("start", "stop", "status", "params")
 */
void handlePIDAutotuneCommand(const char* payload);

/**
 * @brief Handle status request command
 */
void handleStatusCommand();

/**
 * @brief Handle FRAM error log commands
 * @param payload Command payload ("stats", "clear")
 */
void handleFRAMErrorsCommand(const char* payload);

/**
 * @brief Handle FRAM storage commands
 * @param payload Command payload ("status", "counters", "runtime", "reset_counters", "format", "save_pid")
 */
void handleFRAMCommand(const char* payload);

/**
 * @brief Handle error management commands
 * @param topic Full topic path
 * @param payload Command payload
 */
void handleErrorCommand(const char* topic, const char* payload);

/**
 * @brief Handle scheduler commands
 * @param topic Full topic path
 * @param payload Command payload (JSON)
 */
void handleSchedulerCommand(const char* topic, const char* payload);

/**
 * @brief Main command router - dispatches to specific handlers
 * @param topic Full topic path (e.g., "boiler/cmd/heating")
 * @param payload Command payload
 */
void routeControlCommand(const char* topic, const char* payload);

/**
 * @brief Publish current safety configuration to MQTT
 */
void publishSafetyConfig();

} // namespace MQTTCommandHandlers
