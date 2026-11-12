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
 * @brief Handle runtime diagnostics requests (boiler/cmd/diagnostics)
 *
 * Read-only dumps only. The payload selects one of a fixed whitelist of
 * RuntimeDiagnostics dumps; it is never passed into the class as text, because
 * DiagnosticCommand also contains FORCE_REBOOT (implemented, calls ESP.restart())
 * and this is a gas boiler controller. Unknown payloads are rejected.
 *
 * @param payload One of: memory, tasks, events, sensors, relays, network, modbus
 */
void handleDiagnosticsCommand(const char* payload);

/**
 * @brief boiler/cmd/relay/<name> -> drive one relay by hand inside a test-mode window.
 *
 * Accepted only while TestMode is active; outside a window the command is refused and said
 * so on boiler/status/testmode rather than silently dropped. Relay names resolve through
 * TestModePolicy::relayFromName(). See TestModePolicy.h for what is and is not checked.
 *
 * @param topic   Full topic path; the relay name is its last segment
 * @param payload on|1|close or off|0|open
 */
void handleRelayCommand(const char* topic, const char* payload);

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
