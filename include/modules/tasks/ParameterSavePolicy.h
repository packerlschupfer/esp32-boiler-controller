// include/modules/tasks/ParameterSavePolicy.h
#ifndef PARAMETER_SAVE_POLICY_H
#define PARAMETER_SAVE_POLICY_H

#include <cstdint>

/**
 * @brief When PersistentStorageTask writes SystemSettings to NVS (header-only, native-testable).
 *
 * Two kinds of save:
 * - EXPLICIT: someone asked (boiler/params/save, StateManager enable/priority/override, the
 *   syslog and autotune writers). Never delayed.
 * - DEBOUNCED: a parameter onChange callback fired. Coalesced, because saveAll() is 60
 *   parameters x (nvs_set + nvs_commit) - Preferences commits per key - and the one key that
 *   changed was already written by PersistentStorage::setJson(). A thermostat card clicking
 *   the room target up five times used to run five full passes.
 *
 * Found 2026-10-18: parametersChanged/lastChangeTime were assigned by all 14 onChange
 * callbacks and read by nothing, so the debounce was designed and never wired up.
 */
namespace ParameterSavePolicy {

    /**
     * @param explicitRequest  STORAGE_SAVE_REQUEST_BIT was set
     * @param parametersChanged  an onChange callback fired since the last save
     * @param nowMs  millis()
     * @param lastChangeMs  millis() at the last onChange
     * @param debounceMs  coalescing window
     */
    inline bool shouldSave(bool explicitRequest, bool parametersChanged,
                           uint32_t nowMs, uint32_t lastChangeMs, uint32_t debounceMs) {
        if (explicitRequest) {
            return true;
        }
        if (!parametersChanged) {
            return false;
        }
        // Unsigned subtraction, so this stays correct across the ~49 day millis() wrap.
        return (nowMs - lastChangeMs) >= debounceMs;
    }

} // namespace ParameterSavePolicy

#endif // PARAMETER_SAVE_POLICY_H
