// include/modules/control/AutotuneGainTarget.h
#ifndef AUTOTUNE_GAIN_TARGET_H
#define AUTOTUNE_GAIN_TARGET_H

/**
 * @brief Which gain set an autotune result belongs to (header-only, native-testable).
 *
 * The relay test drives whichever loop the active burner request selects: with a WATER
 * request the boiler charges the tank, otherwise it feeds the radiators. Ku and Tu
 * therefore describe that loop only, and the result must go to that loop's gain set
 * (wHeaterK* or spaceHeatingK*).
 *
 * BoilerTempController::updateMode() - which normally tracks the mode - does not run
 * while tuning, so isWaterMode_ still holds the mode of the last normal control cycle
 * (review 2026-09-14 pid-4: results were persisted to the gain set chosen by that stale
 * flag). The mode is instead captured at startAutoTuning() and compared with the active
 * request when the result is saved:
 *
 * - unchanged: write to the captured mode's gain set
 * - changed mid-run: reject. Part of the oscillation came from the radiator loop and
 *   part from the tank, so the result describes neither; the water gains were still
 *   hand-corrected values (2026-09-13) and silently overwriting either set with a
 *   mixed-loop measurement is worse than losing the run.
 */
namespace AutotuneGainTarget {

    enum class Target {
        SPACE,                // Persist to spaceHeatingKp/Ki/Kd
        WATER,                // Persist to wHeaterKp/Ki/Kd
        REJECT_MODE_CHANGED   // Discard: water/space changed during the run
    };

    inline Target select(bool waterModeAtStart, bool waterModeNow) {
        if (waterModeAtStart != waterModeNow) {
            return Target::REJECT_MODE_CHANGED;
        }
        return waterModeAtStart ? Target::WATER : Target::SPACE;
    }

    // "water"/"space" for the MQTT result payload and logs
    inline const char* toString(Target target) {
        switch (target) {
            case Target::WATER:  return "water";
            case Target::SPACE:  return "space";
            default:             return "rejected";
        }
    }

} // namespace AutotuneGainTarget

#endif // AUTOTUNE_GAIN_TARGET_H
