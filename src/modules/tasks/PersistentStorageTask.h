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
 * @brief Mark the live room target as a TEMPORARY override that must not be persisted.
 *
 * The HA climate card writes boiler/cmd/room_target, which by owner decision is a temporary
 * override: it changes the running target and must NOT survive a reboot. The durable path is
 * params/set/heating/targetTemp.
 *
 * Both paths end up in the same field, settings.targetTemperatureInside, so the save path could
 * not tell them apart - and syncShadowsFromSettings() runs immediately before every saveAll(),
 * copying the live value into the registered shadow. So ANY unrelated save silently promoted
 * the override to NVS: every enable/priority/override switch, autotune, fram save_pid,
 * boiler/params/save, and every debounced params/set of any other parameter. Whether a card
 * adjustment survived a reboot therefore depended on whether something else happened to be
 * saved while it was active, and the persisted value was unreadable at runtime because
 * params/get reports the RAM-synced shadow. After the next reboot the house ran at a stale
 * override setpoint with the curve and backstop following it. Found by the 2026-10-28 review.
 *
 * Calling this makes the next save keep the last PERSISTED target instead. Cleared
 * automatically when a durable write arrives. Note the sync is still needed for durable
 * writes - removing it outright would reintroduce 57d6456, where a save wrote a stale shadow
 * back and reverted the room target.
 */
void PersistentStorageTask_MarkRoomTargetTemporary();

/**
 * @brief Republish one retained boiler/params/status/<group> mirror.
 *
 * Those mirrors are published on the params/set path only, so they go stale after any
 * boiler/cmd write - the HA System/Heating/Water/Water-Priority switches and the
 * heating/water override commands - and nothing refreshes them until HA restarts, the device
 * comes online, the add-on reconnects, a params/set happens to hit the same group, or the
 * operator presses that row's R button.
 *
 * For most keys that is display-only. For wheater/priorityEnabled it is a WRITE-BACK HAZARD:
 * the add-on's Water tab bulk "Set" always publishes the checkbox populated from the stale
 * group, so an HA priority change made via boiler/cmd was silently reverted the next time
 * anyone saved anything on that tab. Found by the 2026-10-28 review.
 *
 * Call after a cmd write that changes a mirrored parameter. Safe from any task: the publish
 * goes through the normal MQTT queue.
 */
void PersistentStorageTask_RepublishGroup(const char* group);

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