#ifndef PERSISTENT_STORAGE_TASK_H
#define PERSISTENT_STORAGE_TASK_H

#include <Arduino.h>
#include "shared/Temperature.h"  // Temperature_t, for the room-target API below

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
 * @brief Is a room-target write meant to survive a reboot?
 *
 * Temporary = the running target changes, NVS does not. Durable = both.
 */
enum class RoomTargetDurability : uint8_t { Temporary, Durable };

/**
 * @brief Write the live room target, recording whether it is a temporary override.
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
 * A Temporary write makes the next save keep the last PERSISTED target instead. A Durable
 * write clears that. Note the sync is still needed for durable writes - removing it outright
 * would reintroduce 57d6456, where a save wrote a stale shadow back and reverted the room
 * target.
 *
 * 2026-10-30: THIS IS NOW THE ONLY WAY TO WRITE THE ROOM TARGET from outside the
 * storage task. The mark-only function it replaces had to be called as a second step
 * after the write, and two of the five writers never called it at all (the
 * boiler/cmd/heating <n> numeric branch and the space-heating window start), while a
 * third called it AFTER releasing the settings mutex - a window in which the storage
 * loop's shadow sync could promote the override before the mark landed. Both halves
 * are gone: one call writes the value and records its durability together.
 *
 * ORDERING IS THE WHOLE TRICK. The shadow sync does not take the settings mutex, so
 * this cannot be made atomic against it. It does not need to be:
 *   - Temporary: set the flag BEFORE the value, so an interleaving sync sees either
 *     (flag set, old value) or (flag set, new value) and leaves the shadow alone.
 *   - Durable:   set the value BEFORE clearing the flag, so a sync sees either
 *     (flag set, new value) - shadow untouched, corrected next pass - or
 *     (flag clear, new value) - shadow correct immediately.
 * The sync runs every loop pass, so both orderings converge. Do not "simplify" this
 * by setting flag and value in the natural reading order.
 *
 * @return false if the settings mutex was busy; the caller must treat that as "the
 *         target did not change" and say so, not assume success.
 */
bool PersistentStorageTask_SetRoomTarget(Temperature_t value, RoomTargetDurability durability);

/**
 * @brief The room target that is currently PERSISTED, i.e. the registered shadow.
 *
 * Needed because "go back to the configured target" is a real operation - a schedule
 * window ending - and the previous implementation answered it by caching the
 * pre-window value in the action object. That cache was a single slot per schedule
 * type, so overlapping windows clobbered it, it did not survive a reboot, and
 * restoring it overwrote any durable params/set the owner made DURING the window.
 * Reading the shadow has none of those problems and needs no state.
 */
Temperature_t PersistentStorageTask_GetDurableRoomTarget();

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