#ifndef PERSISTENT_STORAGE_TASK_H
#define PERSISTENT_STORAGE_TASK_H

#include <Arduino.h>

/**
 * @brief Persistent Storage Task
 *
 * Manages persistent parameter storage with NVS backend and MQTT integration.
 *
 * Features:
 * - Registers all system parameters for persistent storage
 * - Loads saved values on startup
 * - MQTT integration for remote parameter access
 * - Change callbacks to notify other tasks
 *
 * @param pvParameters Task parameters (unused)
 */
void PersistentStorageTask(void* pvParameters);

/**
 * @brief Request a save of all parameters to NVS
 *
 * This triggers an asynchronous save operation. Use after updating
 * SystemSettings values that need to persist across reboots.
 */
void PersistentStorageTask_RequestSave();

/**
 * @brief Request a reload of all parameters from NVS
 */
void PersistentStorageTask_RequestLoad();

#endif // PERSISTENT_STORAGE_TASK_H