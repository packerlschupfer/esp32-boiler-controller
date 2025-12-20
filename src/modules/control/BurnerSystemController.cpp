// src/modules/control/BurnerSystemController.cpp
// Controls BURNER relays only (0,1,2). Pump relays (4,5) controlled by PumpControlModule.

#include "modules/control/BurnerSystemController.h"
#include "modules/control/SafetyInterlocks.h"
#include "modules/control/CentralizedFailsafe.h"
#include "modules/tasks/RelayControlTask.h"
#include "config/SystemConstants.h"
#include "config/RelayIndices.h"
#include "events/SystemEventsGenerated.h"
#include "shared/Temperature.h"
#include "core/SystemResourceProvider.h"
#include "LoggingMacros.h"
#include <MutexGuard.h>

static const char* TAG = "BurnerSystemController";

BurnerSystemController::BurnerSystemController()
    : currentMode_(BurnerMode::OFF)
    , currentPower_(PowerLevel::HALF)
    , currentTarget_(0)
    , isActive_(false)
{
    mutex_ = xSemaphoreCreateMutex();
    if (!mutex_) {
        LOG_ERROR(TAG, "Failed to create mutex");
    }
}

BurnerSystemController::~BurnerSystemController() {
    if (mutex_) {
        vSemaphoreDelete(mutex_);
    }
}

Result<void> BurnerSystemController::initialize() {
    LOG_INFO(TAG, "Initializing BurnerSystemController");
    return Result<void>();
}

void BurnerSystemController::shutdown() {
    LOG_INFO(TAG, "Shutting down BurnerSystemController");
    deactivate();
}

Result<void> BurnerSystemController::activateHeatingMode(Temperature_t targetTemp, PowerLevel power) {
    if (!mutex_) {
        return Result<void>(SystemError::MUTEX_TIMEOUT, "Mutex not initialized");
    }

    MutexGuard lock(mutex_);

    LOG_INFO(TAG, "Activating heating mode (target: %d, power: %d)", targetTemp, static_cast<int>(power));

    // Determine power level
    PowerLevel actualPower = (power == PowerLevel::AUTO) ? PowerLevel::HALF : power;

    // Build relay states for heating mode (burner relays only)
    auto relayStates = buildRelayStates(BurnerMode::HEATING, actualPower);

    // Execute atomic batch command
    auto batchResult = executeRelayBatch(relayStates);
    if (batchResult.isError()) {
        return batchResult;
    }

    // Update internal state
    currentMode_ = BurnerMode::HEATING;
    currentPower_ = actualPower;
    currentTarget_ = targetTemp;
    isActive_ = true;

    LOG_INFO(TAG, "Heating mode activated (burner relays set)");
    return Result<void>();
}

Result<void> BurnerSystemController::activateWaterMode(Temperature_t targetTemp, PowerLevel power) {
    if (!mutex_) {
        return Result<void>(SystemError::MUTEX_TIMEOUT, "Mutex not initialized");
    }

    MutexGuard lock(mutex_);

    LOG_INFO(TAG, "Activating water mode (target: %d, power: %d)", targetTemp, static_cast<int>(power));

    // Determine power level
    PowerLevel actualPower = (power == PowerLevel::AUTO) ? PowerLevel::FULL : power;

    // Build relay states for water mode (burner relays only)
    auto relayStates = buildRelayStates(BurnerMode::WATER, actualPower);

    // Execute atomic batch command
    auto batchResult = executeRelayBatch(relayStates);
    if (batchResult.isError()) {
        return batchResult;
    }

    // Update internal state
    currentMode_ = BurnerMode::WATER;
    currentPower_ = actualPower;
    currentTarget_ = targetTemp;
    isActive_ = true;

    LOG_INFO(TAG, "Water mode activated (burner relays set)");
    return Result<void>();
}

Result<void> BurnerSystemController::switchMode(BurnerMode newMode, Temperature_t targetTemp) {
    if (!mutex_) {
        return Result<void>(SystemError::MUTEX_TIMEOUT, "Mutex not initialized");
    }

    MutexGuard lock(mutex_);

    // Validate mode switch parameters
    if (newMode == currentMode_) {
        LOG_DEBUG(TAG, "Already in target mode - no switch needed");
        return Result<void>();  // No change needed
    }

    if (newMode == BurnerMode::OFF || currentMode_ == BurnerMode::OFF) {
        return Result<void>(SystemError::INVALID_PARAMETER,
                           "switchMode() only for HEATING ↔ WATER (use deactivate() for OFF transitions)");
    }

    if (newMode == BurnerMode::BOTH) {
        return Result<void>(SystemError::INVALID_PARAMETER,
                           "switchMode() does not support BOTH mode");
    }

    if (!isActive_) {
        return Result<void>(SystemError::INVALID_PARAMETER,
                           "Cannot switch mode - burner not active");
    }

    // Log mode switch
    char tempBuf[16];
    formatTemp(tempBuf, sizeof(tempBuf), targetTemp);
    LOG_INFO(TAG, "Mode switch: %s → %s (target: %s, power: %s)",
             currentMode_ == BurnerMode::HEATING ? "HEATING" : "WATER",
             newMode == BurnerMode::HEATING ? "HEATING" : "WATER",
             tempBuf,
             currentPower_ == PowerLevel::FULL ? "FULL" : "HALF");

    // Build relay states for new mode (burner relays only)
    auto relayStates = buildRelayStates(newMode, currentPower_);

    // Execute atomic batch command
    auto batchResult = executeRelayBatch(relayStates);
    if (batchResult.isError()) {
        LOG_ERROR(TAG, "Mode switch relay batch failed: %s",
                 batchResult.message().c_str());
        return batchResult;
    }

    // Update internal state
    currentMode_ = newMode;
    currentTarget_ = targetTemp;

    LOG_INFO(TAG, "Mode switch complete (burner relays updated)");
    return Result<void>();
}

Result<void> BurnerSystemController::deactivate() {
    if (!mutex_) {
        return Result<void>(SystemError::MUTEX_TIMEOUT, "Mutex not initialized");
    }

    MutexGuard lock(mutex_);

    if (!isActive_) {
        LOG_DEBUG(TAG, "Already deactivated");
        return Result<void>();
    }

    LOG_INFO(TAG, "Deactivating burner (relays only - pumps handled by PumpControlModule)");

    // Build relay states: all burner relays OFF
    // Pump relays (4,5) are NOT touched - PumpControlModule controls them independently
    auto relayStates = buildRelayStates(BurnerMode::OFF, PowerLevel::FULL);

    // Execute batch command
    auto batchResult = executeRelayBatch(relayStates);
    if (batchResult.isError()) {
        return batchResult;
    }

    // Update state
    isActive_ = false;
    currentMode_ = BurnerMode::OFF;

    LOG_INFO(TAG, "Burner deactivated (burner relays OFF)");
    return Result<void>();
}


Result<void> BurnerSystemController::emergencyShutdown(const char* reason) {
    LOG_ERROR(TAG, "EMERGENCY SHUTDOWN: %s", reason);

    // Bypass mutex - emergency takes priority
    // Turn off ALL relays immediately
    bool relaySuccess = RelayControlTask::setAllRelays(false);

    // C2: Enhanced error handling for relay failure during emergency
    if (!relaySuccess) {
        LOG_ERROR(TAG, "CRITICAL: setAllRelays FAILED during emergency shutdown!");
        LOG_ERROR(TAG, "Physical safety devices (thermal fuse, pressure relief) are last line of defense");

        // Set error bit so monitoring can detect this critical failure
        xEventGroupSetBits(SRP::getErrorNotificationEventGroup(),
                          SystemEvents::Error::RELAY | SystemEvents::Error::SAFETY);

        // Note: Don't try to activate alarm relay - if setAllRelays failed,
        // Modbus communication is likely down and alarm would also fail.
        // The error bits above will trigger monitoring alerts via MQTT.
    }

    // Update state (best-effort)
    if (mutex_ && xSemaphoreTake(mutex_, 0) == pdTRUE) {
        isActive_ = false;
        currentMode_ = BurnerMode::OFF;
        xSemaphoreGive(mutex_);
    }

    // C2: Return error if relay operation failed so callers know shutdown may be incomplete
    if (!relaySuccess) {
        return Result<void>(SystemError::RELAY_OPERATION_FAILED,
                           "Emergency relay shutdown failed - physical safety devices are last defense");
    }

    return Result<void>();
}

bool BurnerSystemController::isActive() const {
    if (!mutex_) return false;
    MutexGuard lock(mutex_);
    return isActive_;
}

BurnerMode BurnerSystemController::getCurrentMode() const {
    if (!mutex_) return BurnerMode::OFF;
    MutexGuard lock(mutex_);
    return currentMode_;
}

PowerLevel BurnerSystemController::getCurrentPowerLevel() const {
    if (!mutex_) return PowerLevel::HALF;
    MutexGuard lock(mutex_);
    return currentPower_;
}

Temperature_t BurnerSystemController::getCurrentTargetTemp() const {
    if (!mutex_) return 0;
    MutexGuard lock(mutex_);
    return currentTarget_;
}

// ============================================================================
// Private Methods
// ============================================================================

std::array<bool, 8> BurnerSystemController::buildRelayStates(BurnerMode mode, PowerLevel power) {
    // RYN4 relay mapping - see RelayIndices.h for single source of truth
    // Physical relay = array index + 1
    //
    // This function ONLY sets burner relays (indices 0,1,2).
    // Pump relays (indices 4,5) are controlled by PumpControlModule.

    std::array<bool, 8> states = {};  // Initialize all to false

    // Burner control relays - actual hardware behavior:
    // - Relay 0 (BURNER_ENABLE): Heating mode at half power
    // - Relay 1 (POWER_BOOST): Boost to full power (works with heating OR water)
    // - Relay 2 (WATER_MODE): Water mode at half power
    //
    // Truth table:
    // HEATING HALF:  Relay0=ON,  Relay1=OFF, Relay2=OFF
    // HEATING FULL:  Relay0=ON,  Relay1=ON,  Relay2=OFF
    // WATER HALF:    Relay0=OFF, Relay1=OFF, Relay2=ON
    // WATER FULL:    Relay0=OFF, Relay1=ON,  Relay2=ON
    // OFF:           Relay0=OFF, Relay1=OFF, Relay2=OFF
    if (mode == BurnerMode::HEATING) {
        states[RelayIndex::BURNER_ENABLE] = true;   // Heating mode on
        states[RelayIndex::WATER_MODE]    = false;
    } else if (mode == BurnerMode::WATER) {
        states[RelayIndex::BURNER_ENABLE] = false;
        states[RelayIndex::WATER_MODE]    = true;   // Water mode on
    } else {
        // OFF mode
        states[RelayIndex::BURNER_ENABLE] = false;
        states[RelayIndex::WATER_MODE]    = false;
    }

    // Boost relay - ON for full power, but only when burner is active
    // In OFF mode, all burner relays should be off
    states[RelayIndex::POWER_BOOST] = (mode != BurnerMode::OFF) && (power == PowerLevel::FULL);

    // NOTE: Pump relays (HEATING_PUMP, WATER_PUMP) are NOT set here.
    // They remain false (0) in the array. The batch command will preserve
    // current pump state because only indices 0,1,2 are meaningful here.

    return states;
}

Result<void> BurnerSystemController::executeRelayBatch(const std::array<bool, 8>& states) {
    // Set burner relays individually (indices 0,1,2 only)
    // This avoids affecting pump relays (4,5) which are controlled by PumpControlModule
    //
    // NOTE: We use individual setRelayState calls rather than setMultipleRelays
    // because setMultipleRelays would overwrite pump relay states.

    bool success = true;

    // Set BURNER_ENABLE (logical 0 → physical 1)
    if (!RelayControlTask::setRelayState(RelayIndex::toPhysical(RelayIndex::BURNER_ENABLE), states[RelayIndex::BURNER_ENABLE])) {
        LOG_ERROR(TAG, "Failed to set BURNER_ENABLE relay");
        success = false;
    }

    // Set POWER_BOOST (logical 1 → physical 2)
    if (!RelayControlTask::setRelayState(RelayIndex::toPhysical(RelayIndex::POWER_BOOST), states[RelayIndex::POWER_BOOST])) {
        LOG_ERROR(TAG, "Failed to set POWER_BOOST relay");
        success = false;
    }

    // Set WATER_MODE (logical 2 → physical 3)
    if (!RelayControlTask::setRelayState(RelayIndex::toPhysical(RelayIndex::WATER_MODE), states[RelayIndex::WATER_MODE])) {
        LOG_ERROR(TAG, "Failed to set WATER_MODE relay");
        success = false;
    }

    if (!success) {
        return Result<void>(SystemError::RELAY_OPERATION_FAILED,
                           "One or more burner relay commands failed");
    }

    LOG_DEBUG(TAG, "Burner relays set (Enable:%d Boost:%d Water:%d)",
              states[RelayIndex::BURNER_ENABLE],
              states[RelayIndex::POWER_BOOST],
              states[RelayIndex::WATER_MODE]);

    return Result<void>();
}

Result<void> BurnerSystemController::setPowerLevel(PowerLevel power) {
    if (!mutex_) {
        return Result<void>(SystemError::NOT_INITIALIZED, "Mutex not initialized");
    }

    MutexGuard lock(mutex_);

    if (!isActive_) {
        return Result<void>(SystemError::INVALID_PARAMETER, "Burner not active");
    }

    if (currentPower_ == power) {
        return Result<void>();  // Already at requested power level
    }

    LOG_INFO(TAG, "Changing power level from %s to %s",
             currentPower_ == PowerLevel::HALF ? "HALF" : "FULL",
             power == PowerLevel::HALF ? "HALF" : "FULL");

    // POWER_BOOST relay (logical 1 → physical 2): ON=full power, OFF=half power
    bool success = RelayControlTask::setRelayState(RelayIndex::toPhysical(RelayIndex::POWER_BOOST), power == PowerLevel::FULL);

    if (!success) {
        LOG_ERROR(TAG, "Failed to set POWER_BOOST relay");
        return Result<void>(SystemError::RELAY_OPERATION_FAILED, "Power level relay failed");
    }

    currentPower_ = power;
    return Result<void>();
}

Result<void> BurnerSystemController::performSafetyCheck() {
    // Check emergency stop
    if (!SafetyInterlocks::checkEmergencyStop()) {
        emergencyShutdown("Emergency stop triggered");
        return Result<void>(SystemError::EMERGENCY_STOP, "Emergency stop is active");
    }

    // Check temperature limits
    if (!SafetyInterlocks::checkTemperatureLimits(SystemConstants::Temperature::MAX_BOILER_TEMP_C)) {
        emergencyShutdown("Temperature limits exceeded");
        return Result<void>(SystemError::TEMPERATURE_CRITICAL, "Temperature limits exceeded");
    }

    // Check critical system errors
    if (!SafetyInterlocks::checkSystemErrors()) {
        return Result<void>(SystemError::SYSTEM_FAILSAFE_TRIGGERED, "Critical system errors detected");
    }

    // Check if boiler is enabled (not an error - just skip burner operations)
    EventBits_t systemStateBits = xEventGroupGetBits(SRP::getSystemStateEventGroup());
    if (!(systemStateBits & SystemEvents::SystemState::BOILER_ENABLED)) {
        return Result<void>();  // Not an error - boiler is disabled
    }

    return Result<void>();
}
