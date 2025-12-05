// include/modules/control/PumpCoordinator.h
#pragma once

#include "utils/ErrorHandler.h"
#include "utils/Utils.h"  // Round 20 Issue #4: For Utils::elapsedMs()
#include "shared/Temperature.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

/**
 * @brief Pump type enumeration
 */
enum class PumpType : uint8_t {
    HEATING = 0,  ///< Heating circulation pump (relay 1)
    WATER = 1     ///< Water heating circulation pump (relay 2)
};

/**
 * @brief Pump lifecycle coordinator with motor protection
 *
 * Manages pump startup/shutdown with 30-second minimum state duration
 * to protect pump motors from excessive cycling.
 *
 * Features:
 * - 30s motor protection (configurable)
 * - Synchronous pump verification (waits for relay feedback)
 * - Thread-safe operation
 * - Emergency bypass for safety-critical shutdowns
 *
 * Usage:
 *   PumpCoordinator coordinator;
 *   auto result = coordinator.startPumpAndVerify(PumpType::WATER, 2000);
 *   if (result.isOk()) {
 *       // Pump confirmed running
 *   }
 */
class PumpCoordinator {
public:
    /**
     * @brief Constructor
     */
    PumpCoordinator();

    /**
     * @brief Destructor
     */
    ~PumpCoordinator();

    /**
     * @brief Check if pump state change is allowed (30s protection)
     *
     * Does NOT command the pump - just checks if change is allowed.
     * Used by BurnerSystemController before sending batch relay command.
     *
     * @param pump Which pump to check
     * @param desiredState Desired ON/OFF state
     * @return Success if change allowed, error if blocked by 30s protection
     *
     * Errors:
     * - PUMP_PROTECTION_ACTIVE: 30s minimum not elapsed since last change
     */
    Result<void> checkPumpChangeAllowed(PumpType pump, bool desiredState);

    /**
     * @brief Record that pump state changed
     *
     * Called by BurnerSystemController AFTER batch relay command succeeds.
     * Updates state tracking for 30s protection enforcement.
     *
     * @param pump Which pump changed
     * @param newState New ON/OFF state
     */
    void recordPumpStateChange(PumpType pump, bool newState);

    /**
     * @brief Check if pump is currently running
     *
     * Reads actual relay feedback (not commanded state).
     *
     * @param pump Which pump to check
     * @return true if pump relay is ON
     */
    bool isPumpOn(PumpType pump) const;

    /**
     * @brief Get time since last pump state change
     *
     * @param pump Which pump to check
     * @return Milliseconds since last state change
     */
    uint32_t getTimeSinceLastChange(PumpType pump) const;

    /**
     * @brief Check if pump can change state (30s protection)
     *
     * @param pump Which pump to check
     * @return true if 30s minimum has elapsed
     */
    bool canChangePumpState(PumpType pump) const;

    /**
     * @brief Emergency override - turn ON all pumps (bypasses protection)
     *
     * For emergency shutdown or failsafe modes.
     * Does NOT update lastChangeTime (allows rapid cycling in emergency).
     */
    void forceAllPumpsOn();

    /**
     * @brief Emergency override - turn OFF all pumps (bypasses protection)
     *
     * Use with caution - only for complete system shutdown.
     */
    void forceAllPumpsOff();

    /**
     * @brief Set minimum pump state duration (motor protection)
     *
     * Default: 30000ms (30 seconds)
     *
     * @param durationMs Minimum duration in milliseconds
     */
    void setMinStateDuration(uint32_t durationMs);

    /**
     * @brief Get minimum state duration
     */
    uint32_t getMinStateDuration() const { return minStateDurationMs_; }

private:
    /**
     * @brief Internal pump state tracking
     */
    struct PumpState {
        bool isOn = false;              ///< Current commanded state
        uint32_t lastChangeTime = 0;    ///< millis() when last changed (0 = never)

        // Round 20 Issue #4: Use Utils::elapsedMs for consistency with rest of codebase
        uint32_t getElapsedMs() const {
            if (lastChangeTime == 0) return UINT32_MAX;  // Never changed
            return Utils::elapsedMs(lastChangeTime);
        }
    };

    PumpState heatingPump_;
    PumpState waterPump_;

    uint32_t minStateDurationMs_;  ///< Minimum state duration (default: 30000ms)

    mutable SemaphoreHandle_t mutex_;  ///< Thread safety

    /**
     * @brief Get PumpState reference (internal helper)
     */
    PumpState& getPumpState(PumpType pump);
    const PumpState& getPumpState(PumpType pump) const;
};
