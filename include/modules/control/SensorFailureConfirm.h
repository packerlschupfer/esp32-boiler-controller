// include/modules/control/SensorFailureConfirm.h
#ifndef SENSOR_FAILURE_CONFIRM_H
#define SENSOR_FAILURE_CONFIRM_H

#include <cstdint>

/**
 * @brief When BurnerControlTask stops the burner for missing temperature sensors
 *        (header-only, native-testable)
 *
 * TemperatureSensorFallback reports SHUTDOWN after one invalid reading, and the heating and
 * water tasks then drop their requests (normal stop with post-purge). The burner-level
 * emergency stop (ERROR for errorRecoveryMs) follows only when the required sensors stay
 * missing for CONFIRM_MS while heat demand persists, so a single glitch (e.g. the room
 * sensor) does not lock the burner out.
 */
namespace SensorFailureConfirm {

    constexpr uint32_t CONFIRM_MS = 10000;  // 4 MB8ART reads

    struct State {
        bool failing = false;
        uint32_t sinceMs = 0;
    };

    /**
     * @return true when the burner should be emergency-stopped now
     */
    inline bool shouldStop(State& state, bool sensorsOk, bool heatDemand, uint32_t nowMs) {
        if (sensorsOk || !heatDemand) {
            state.failing = false;
            return false;
        }
        if (!state.failing) {
            state.failing = true;
            state.sinceMs = nowMs;
            return false;
        }
        return (nowMs - state.sinceMs) >= CONFIRM_MS;
    }

}  // namespace SensorFailureConfirm

#endif  // SENSOR_FAILURE_CONFIRM_H
