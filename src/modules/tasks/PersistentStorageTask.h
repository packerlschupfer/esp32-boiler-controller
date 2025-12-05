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
 * - Periodic auto-save every 5 minutes
 * - MQTT integration for remote parameter access
 * - Change callbacks to notify other tasks
 * 
 * @param pvParameters Task parameters (unused)
 */
void PersistentStorageTask(void* pvParameters);

#endif // PERSISTENT_STORAGE_TASK_H