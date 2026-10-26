// src/modules/tasks/MonitoringTask.h
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "config/ProjectConfig.h"
#include <EthernetManager.h>

/**
 * @brief System monitoring task for Boiler Controller
 * 
 * Periodically logs system health, network status, sensor readings, and relay states
 */
class MonitoringTask {
public:
    /**
     * Initialize the monitoring task
     * @return true if initialization successful, false otherwise
     */
    static bool init();

    /**
     * Start the monitoring task
     * @return true if task started successfully, false otherwise
     */
    static bool start();

    /**
     * Stop the monitoring task
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

private:
    /**
     * Main task function
     * @param pvParameters Task parameters (unused)
     */
    static void taskFunction(void* pvParameters);

    /**
     * Log system health information
     */
    static void logSystemHealth();
    
    /**
     * Log all FreeRTOS tasks with detailed information
     */
    static void logAllTasks();

    /**
     * Log network status
     */
    static void logNetworkStatus();

    /**
     * Log MB8ART sensor status and information
     */
    static void logSensorStatus();

    /**
     * Log RYN4 relay status and information
     */
    static void logRelayStatus();

    /**
     * Log control module states
     */
    static void logControlStates();

    // Task state
    static TaskHandle_t taskHandle;
};

/**
 * @brief Minimum stack free across the tasks WE control, from the last monitoring pass.
 *
 * Framework-owned stacks (IDLE, ipc) are excluded - their sizes are not ours to set and IDLE's
 * ~456 B free is normal, so including them would permanently mask the tightest task of ours.
 * Returns 0 before the first pass has run.
 *
 * Published as `stack_min` on boiler/status/health, replacing `stack_hwm`, which was
 * uxTaskGetStackHighWaterMark(NULL) - the MQTT task's OWN headroom under a name that reads like
 * a system minimum (found 2026-10-26, only because the published 1604 happened to match
 * MQTTTask exactly in a per-task dump).
 */
uint16_t MonitoringTask_getMinTaskStackFree();

/// Name of the task reported by MonitoringTask_getMinTaskStackFree(), truncated to 12 chars.
/// Always NUL-terminated; may be one pass behind the number. See the definition for why.
const char* MonitoringTask_getMinTaskStackName();
