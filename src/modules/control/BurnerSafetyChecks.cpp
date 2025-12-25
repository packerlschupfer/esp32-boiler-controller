// src/modules/control/BurnerSafetyChecks.cpp
#include "BurnerSafetyChecks.h"
#include "BurnerStateMachine.h"
#include "modules/control/BurnerSystemController.h"
#include "modules/control/BurnerAntiFlapping.h"
#include "core/SystemResourceProvider.h"
#include "shared/SharedSensorReadings.h"
#include "events/SystemEventsGenerated.h"
#include "utils/MutexRetryHelper.h"
#include <esp_log.h>
#include <atomic>

static const char* TAG = "BurnerSafety";

bool BurnerSafetyChecks::isFlameDetected() {
    // WARNING: No flame detection hardware installed
    // System assumes flame is present when burner relay is active

    // Round 14 Issue #2: Use atomic for thread-safe one-time log
    static std::atomic<bool> warningLogged{false};
    if (!warningLogged.exchange(true, std::memory_order_relaxed)) {
        LOG_DEBUG(TAG, "No flame detection sensor installed - assuming flame when burner active");
    }

    // Without a flame sensor, we assume flame is present when burner is active
    // In a real system, this would check an actual flame sensor
    // TODO: Integrate actual flame sensor when hardware is available

    BurnerSystemController* controller = SRP::getBurnerSystemController();
    return controller ? controller->isActive() : false;
}

bool BurnerSafetyChecks::checkSafetyConditions() {
    BurnerSystemController* controller = SRP::getBurnerSystemController();
    if (controller) {
        auto result = controller->performSafetyCheck();
        return result.isSuccess();
    }
    return false;  // Fail-safe: no controller = not safe
}

bool BurnerSafetyChecks::canSeamlesslySwitch(BurnerSMState currentState) {
    // Only allow seamless mode switch when all conditions are safe:

    // 1. Currently in stable RUNNING state
    if (currentState != BurnerSMState::RUNNING_LOW && currentState != BurnerSMState::RUNNING_HIGH) {
        return false;
    }

    // 2. Safety conditions pass
    if (!checkSafetyConditions()) {
        return false;
    }

    // 3. Flame detected (burner actually running)
    if (!isFlameDetected()) {
        return false;
    }

    // Note: We don't check heatDemand here because during mode switch,
    // the old mode clears its demand before the new mode sets it.
    // The MODE_SWITCHING handler will validate new mode has demand.

    return true;
}

BurnerSMState BurnerSafetyChecks::checkModeSwitchTransition(
    BurnerSMState currentState,
    const char* currentStateName,
    bool& runningModeIsWater
) {
    // Check for mode switch (water ↔ heating)
    // When both WATER_ON and HEATING_ON are set, use WATER_PRIORITY to decide
    EventBits_t systemBits = xEventGroupGetBits(SRP::getSystemStateEventGroup());
    bool waterOn = (systemBits & SystemEvents::SystemState::WATER_ON) != 0;
    bool heatingOn = (systemBits & SystemEvents::SystemState::HEATING_ON) != 0;
    bool waterPriority = (systemBits & SystemEvents::SystemState::WATER_PRIORITY) != 0;

    // Sanity check: both modes should not be ON simultaneously (indicates race condition)
    if (waterOn && heatingOn) {
        LOG_WARN(TAG, "Both WATER_ON and HEATING_ON set - using priority=%d as tiebreaker", waterPriority);
    }

    // Water mode if: water is on AND (heating is off OR water has priority)
    bool currentModeIsWater = waterOn && (!heatingOn || waterPriority);
    if (currentModeIsWater != runningModeIsWater) {
        // Mode switch detected - attempt seamless or go to POST_PURGE
        if (canSeamlesslySwitch(currentState)) {
            LOG_INFO(TAG, "Seamless mode switch detected during %s (%s -> %s)",
                     currentStateName,
                     runningModeIsWater ? "WATER" : "HEATING",
                     currentModeIsWater ? "WATER" : "HEATING");
            return BurnerSMState::MODE_SWITCHING;
        } else {
            LOG_INFO(TAG, "Mode switch detected during %s (%s -> %s) - transitioning to POST_PURGE",
                     currentStateName,
                     runningModeIsWater ? "WATER" : "HEATING",
                     currentModeIsWater ? "WATER" : "HEATING");
            return BurnerSMState::POST_PURGE;
        }
    }

    // No mode switch - return IDLE as sentinel
    return BurnerSMState::IDLE;
}

BurnerSMState BurnerSafetyChecks::checkSafetyShutdown(BurnerSMState currentState, bool heatDemand) {
    // Check if we should stop burner
    if (!heatDemand || !checkSafetyConditions()) {
        // Check anti-flapping before turning off
        if (BurnerAntiFlapping::canTurnOff()) {
            return BurnerSMState::POST_PURGE;
        } else {
            LOG_DEBUG(TAG, "Delaying burner stop for %lu ms due to anti-flapping",
                     BurnerAntiFlapping::getTimeUntilCanTurnOff());
        }
    }

    // No shutdown condition - return current state
    return currentState;
}

BurnerSMState BurnerSafetyChecks::checkFlameLoss(BurnerSMState currentState, bool heatDemand) {
    // Round 16 Issue B: Differentiate intentional shutdown from unexpected flame loss
    // Check if flame is lost - but distinguish between intentional and unexpected
    if (!isFlameDetected()) {
        if (!heatDemand) {
            // Intentional shutdown - burner was commanded off, this is expected
            LOG_DEBUG(TAG, "Burner off (intentional - demand ended)");
        } else {
            // Unexpected flame loss - demand is active but flame is gone
            // This could indicate a real problem (even without a flame sensor)
            LOG_WARN(TAG, "UNEXPECTED: Flame/burner off while demand still active");
        }
        // Both cases transition to POST_PURGE (bypasses anti-flapping for safety)
        return BurnerSMState::POST_PURGE;
    }

    // Flame detected - no transition
    return currentState;
}
