// include/modules/control/SensorErrorBitPolicy.h
#ifndef SENSOR_ERROR_BIT_POLICY_H
#define SENSOR_ERROR_BIT_POLICY_H

#include <cstdint>

/**
 * @brief Who owns Error::SENSOR_FAILURE, and when BurnerControlTask may clear it
 *        (header-only, native-testable).
 *
 * The bit has TWO writers and only one of them used to have a clear path:
 *
 * - TemperatureSensorFallback raises it for a genuinely missing sensor and clears it in
 *   exactly two places (TemperatureSensorFallback.cpp:292, :318). One sits inside
 *   `if (currentMode == SHUTDOWN)`, the other inside a mode-CHANGE guard.
 * - BurnerControlTask raises it when it cannot read the sensors at all - a mutex timeout
 *   or stale data - which says nothing about whether the sensors work.
 *
 * ErrorRecovery.cpp:397 is the only other clear, and it sits in clearErrors(), which has
 * no callers. So a bit raised by BurnerControlTask while the sensors were healthy was
 * cleared by nobody, and the bit is load-bearing twice over: it is in CRITICAL_ERRORS
 * (SafetyInterlocks.cpp:362), so performSafetyCheck() fails forever and emergency-stops
 * the burner, and clearEmergencyStop() requires it clear (CentralizedFailsafe.cpp:508-511),
 * so boiler/cmd/emergency_reset refuses permanently. Measured 2026-10-18: one 100 ms
 * sensor-mutex timeout, with healthy sensors, latched an emergency stop that no MQTT
 * command could release - only a reboot recovered.
 *
 * This policy keeps the two writers from fighting without merging them: BurnerControlTask
 * clears ONLY a bit it raised itself, so TSF's SHUTDOWN signal is never stomped.
 */
namespace SensorErrorBitPolicy {

    enum class Action {
        NONE,   // leave the bit alone - it is either already right, or not ours
        RAISE,  // set Error::SENSOR_FAILURE and take ownership
        CLEAR   // clear it and release ownership
    };

    /**
     * @param confirmedFailure  reads have failed for the whole confirm window
     *                          (SensorFailureConfirm::shouldStop)
     * @param readOk            this cycle's sensor read succeeded and is fresh
     * @param ownedHere         this task raised the bit and has not released it
     */
    inline Action decide(bool confirmedFailure, bool readOk, bool ownedHere) {
        if (confirmedFailure) {
            // Re-raising an already-owned bit is harmless and keeps the decision
            // idempotent, which matters because the caller runs this every cycle.
            return Action::RAISE;
        }
        if (readOk && ownedHere) {
            return Action::CLEAR;
        }
        // Not confirmed yet, or a read failed but the window has not elapsed, or the bit
        // belongs to TemperatureSensorFallback. In every one of those cases the burner is
        // still blocked for this cycle by the caller's heatDemand = false.
        return Action::NONE;
    }

} // namespace SensorErrorBitPolicy

#endif // SENSOR_ERROR_BIT_POLICY_H
