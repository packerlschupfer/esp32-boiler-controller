// src/common/SystemStrings.h
#pragma once

/**
 * Common string constants to reduce memory usage by eliminating duplicates
 * These strings are stored in Flash memory
 */

namespace SystemStrings {
    // Common error messages
    constexpr const char* ERR_MEMORY_ALLOC = "Memory allocation failed";
    constexpr const char* ERR_MUTEX_ACQUIRE = "Failed to acquire mutex";
    constexpr const char* ERR_MUTEX_TIMEOUT = "Mutex timeout";
    constexpr const char* ERR_TASK_CREATE = "Failed to create task";
    constexpr const char* ERR_INIT_FAILED = "Initialization failed";
    constexpr const char* ERR_NOT_INITIALIZED = "Not initialized";
    constexpr const char* ERR_INVALID_PARAM = "Invalid parameter";
    constexpr const char* ERR_DEVICE_NOT_FOUND = "Device not found";
    
    // MQTT specific errors
    constexpr const char* ERR_MQTT_CONNECT = "Failed to connect to MQTT";
    constexpr const char* ERR_MQTT_SUBSCRIBE = "Failed to subscribe";
    constexpr const char* ERR_MQTT_PUBLISH = "Failed to publish";
    constexpr const char* ERR_MQTT_NOT_CONNECTED = "MQTT not connected";
    
    // Modbus specific errors  
    constexpr const char* ERR_MODBUS_COMM = "Modbus communication error";
    constexpr const char* ERR_MODBUS_TIMEOUT = "Modbus timeout";
    constexpr const char* ERR_MODBUS_REQUEST = "Failed to send request";
    
    // Task status messages
    constexpr const char* MSG_TASK_STARTED = "Task started";
    constexpr const char* MSG_TASK_STOPPED = "Task stopped";
    constexpr const char* MSG_TASK_RUNNING = "Task running";
    
    // Success messages
    constexpr const char* MSG_INIT_SUCCESS = "Initialized successfully";
    constexpr const char* MSG_CONNECTED = "Connected";
    constexpr const char* MSG_DISCONNECTED = "Disconnected";
    
    // Format strings for common patterns
    constexpr const char* FMT_FAILED_TO_CREATE = "Failed to create %s";
    constexpr const char* FMT_FAILED_TO_INIT = "Failed to initialize %s";
    constexpr const char* FMT_TASK_STACK = "%s stack: %u words";
    constexpr const char* FMT_ERROR_CODE = "Error code: %d";
}