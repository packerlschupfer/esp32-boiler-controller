// src/modules/control/PumpCoordinator.cpp

#include "modules/control/PumpCoordinator.h"
#include "modules/tasks/RelayControlTask.h"
#include "shared/RelayFunctionDefs.h"
#include "shared/SharedRelayReadings.h"
#include "core/SystemResourceProvider.h"
#include "utils/Utils.h"
#include "LoggingMacros.h"
#include <MutexGuard.h>

static const char* TAG = "PumpCoordinator";

PumpCoordinator::PumpCoordinator()
    : minStateDurationMs_(30000)  // 30 seconds default
{
    mutex_ = xSemaphoreCreateMutex();
    if (!mutex_) {
        LOG_ERROR(TAG, "Failed to create mutex");
    }

    LOG_INFO(TAG, "PumpCoordinator initialized (protection: %lu ms)", minStateDurationMs_);
}

PumpCoordinator::~PumpCoordinator() {
    if (mutex_) {
        vSemaphoreDelete(mutex_);
    }
}

Result<void> PumpCoordinator::checkPumpChangeAllowed(PumpType pump, bool desiredState) {
    if (!mutex_) {
        return Result<void>(SystemError::MUTEX_TIMEOUT, "Mutex not initialized");
    }

    MutexGuard lock(mutex_);

    auto& state = getPumpState(pump);
    const char* pumpName = (pump == PumpType::HEATING) ? "Heating" : "Water";

    // Check if already in desired state
    if (state.isOn == desiredState) {
        LOG_DEBUG(TAG, "%s pump already %s", pumpName, desiredState ? "ON" : "OFF");
        return Result<void>();  // No change needed
    }

    // Check 30s motor protection
    if (state.lastChangeTime != 0) {  // Not first change
        uint32_t elapsed = state.getElapsedMs();
        if (elapsed < minStateDurationMs_) {
            LOG_WARN(TAG, "%s pump change blocked by protection (%lu/%lu ms)",
                     pumpName, elapsed, minStateDurationMs_);
            return Result<void>(SystemError::PUMP_PROTECTION_ACTIVE,
                               "30s minimum state duration not elapsed");
        }
    }

    // Change is allowed
    LOG_DEBUG(TAG, "%s pump change allowed (elapsed: %lu ms)",
              pumpName, state.lastChangeTime ? state.getElapsedMs() : 0);
    return Result<void>();
}

void PumpCoordinator::recordPumpStateChange(PumpType pump, bool newState) {
    if (!mutex_) {
        return;
    }

    MutexGuard lock(mutex_);

    auto& state = getPumpState(pump);
    const char* pumpName = (pump == PumpType::HEATING) ? "Heating" : "Water";

    // Update state tracking
    state.isOn = newState;
    state.lastChangeTime = millis();

    LOG_INFO(TAG, "%s pump state recorded: %s (for 30s protection)",
             pumpName, newState ? "ON" : "OFF");
}

bool PumpCoordinator::isPumpOn(PumpType pump) const {
    // Read actual relay feedback (not commanded state)
    auto& readings = SRP::getRelayReadings();

    if (pump == PumpType::HEATING) {
        return readings.relayHeatingPump;
    } else {
        return readings.relayWaterPump;
    }
}

uint32_t PumpCoordinator::getTimeSinceLastChange(PumpType pump) const {
    if (!mutex_) {
        return 0;
    }

    MutexGuard lock(mutex_);
    const auto& state = getPumpState(pump);
    return state.getElapsedMs();
}

bool PumpCoordinator::canChangePumpState(PumpType pump) const {
    if (!mutex_) {
        return false;
    }

    MutexGuard lock(mutex_);
    const auto& state = getPumpState(pump);

    // First change always allowed
    if (state.lastChangeTime == 0) {
        return true;
    }

    // Check if 30s protection elapsed
    uint32_t elapsed = state.getElapsedMs();
    return elapsed >= minStateDurationMs_;
}

void PumpCoordinator::forceAllPumpsOn() {
    LOG_WARN(TAG, "Emergency: forcing all pumps ON (bypassing protection)");

    // Bypass protection - command directly without updating state tracking
    RelayControlTask::setRelayState(RelayFunction::HEATING_PUMP, true);
    RelayControlTask::setRelayState(RelayFunction::WATER_PUMP, true);

    // Note: lastChangeTime NOT updated - allows emergency cycling
}

void PumpCoordinator::forceAllPumpsOff() {
    LOG_WARN(TAG, "Emergency: forcing all pumps OFF (bypassing protection)");

    // Bypass protection - command directly
    RelayControlTask::setRelayState(RelayFunction::HEATING_PUMP, false);
    RelayControlTask::setRelayState(RelayFunction::WATER_PUMP, false);

    // Note: lastChangeTime NOT updated
}

void PumpCoordinator::setMinStateDuration(uint32_t durationMs) {
    if (!mutex_) {
        return;
    }

    MutexGuard lock(mutex_);
    minStateDurationMs_ = durationMs;
    LOG_INFO(TAG, "Pump protection duration set to %lu ms", durationMs);
}

// ============================================================================
// Private Methods
// ============================================================================

PumpCoordinator::PumpState& PumpCoordinator::getPumpState(PumpType pump) {
    return (pump == PumpType::HEATING) ? heatingPump_ : waterPump_;
}

const PumpCoordinator::PumpState& PumpCoordinator::getPumpState(PumpType pump) const {
    return (pump == PumpType::HEATING) ? heatingPump_ : waterPump_;
}
