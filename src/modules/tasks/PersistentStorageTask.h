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

/**
 * @brief Set a registered parameter as if it came from boiler/params/set/<name>
 *
 * Queued to the storage task, which range-checks it, updates the registered value and
 * its SystemSettings field through the change callback, and saves. Use this instead of
 * writing SystemSettings directly for registered parameters: the next parameter save
 * stored the stale registered value again (review 2026-09-14).
 *
 * @return false if the storage is not initialized or the command could not be queued
 */
bool PersistentStorageTask_SetParameter(const char* name, const char* payload);

/**
 * @brief True once the saved parameters are loaded and applied to SystemSettings
 *
 * For settings read only once at task start (boiler PID mode): the storage task loads
 * them ~150 ms after the control tasks start.
 */
bool PersistentStorageTask_ParametersLoaded();

#endif // PERSISTENT_STORAGE_TASK_H