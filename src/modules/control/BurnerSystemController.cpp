// src/modules/control/BurnerSystemController.cpp

#include "modules/control/BurnerSystemController.h"
#include "modules/control/SafetyInterlocks.h"
#include "modules/control/CentralizedFailsafe.h"
#include "modules/tasks/RelayControlTask.h"
#include "config/SystemConstants.h"
#include "config/RelayIndices.h"
#include "events/SystemEventsGenerated.h"
#include "shared/SharedRelayReadings.h"
#include "shared/SharedSensorReadings.h"
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
    , inHeatRecoveryMode_(false)
    , heatRecoveryStartTime_(0)
    , inCooldownMode_(false)
    , cooldownStartTime_(0)
    , cooldownHeatingPumpWasOn_(false)
    , cooldownWaterPumpWasOn_(false)
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

    // Cancel any pending cooldown (new mode takes over heat dissipation)
    if (inCooldownMode_) {
        LOG_INFO(TAG, "Cooldown cancelled - heating mode activation");
        inCooldownMode_ = false;
        cooldownHeatingPumpWasOn_ = false;
        cooldownWaterPumpWasOn_ = false;
    }

    // Check pump protection for heating pump
    auto pumpCheck = pumpCoordinator_.checkPumpChangeAllowed(PumpType::HEATING, true);
    if (pumpCheck.isError()) {
        LOG_WARN(TAG, "Heating pump change blocked: %s", pumpCheck.message());
        return pumpCheck;
    }

    // Determine power level
    PowerLevel actualPower = (power == PowerLevel::AUTO) ? PowerLevel::HALF : power;

    // Build relay states for heating mode
    // Heating pump ON, water pump OFF, burner in heating mode
    auto relayStates = buildRelayStates(BurnerMode::HEATING, actualPower, true, false);

    // Execute atomic batch command
    auto batchResult = executeRelayBatch(relayStates);
    if (batchResult.isError()) {
        return batchResult;
    }

    // Record pump state change (for 30s protection)
    pumpCoordinator_.recordPumpStateChange(PumpType::HEATING, true);
    if (currentMode_ == BurnerMode::WATER) {
        // Switched from water - record water pump going OFF
        pumpCoordinator_.recordPumpStateChange(PumpType::WATER, false);
    }

    // Update internal state
    currentMode_ = BurnerMode::HEATING;
    currentPower_ = actualPower;
    currentTarget_ = targetTemp;
    isActive_ = true;

    // Clear heat recovery mode (if was active)
    if (inHeatRecoveryMode_) {
        uint32_t recoveryDuration = millis() - heatRecoveryStartTime_;
        LOG_INFO(TAG, "Heat recovery mode ended (duration: %lu s) - burner activated",
                 recoveryDuration / 1000);
        inHeatRecoveryMode_ = false;
    }

    LOG_INFO(TAG, "Heating mode activated successfully");
    return Result<void>();
}

Result<void> BurnerSystemController::activateWaterMode(Temperature_t targetTemp, PowerLevel power) {
    if (!mutex_) {
        return Result<void>(SystemError::MUTEX_TIMEOUT, "Mutex not initialized");
    }

    MutexGuard lock(mutex_);

    LOG_INFO(TAG, "Activating water mode (target: %d, power: %d)", targetTemp, static_cast<int>(power));

    // Cancel any pending cooldown (new mode takes over heat dissipation)
    if (inCooldownMode_) {
        LOG_INFO(TAG, "Cooldown cancelled - water mode activation");
        inCooldownMode_ = false;
        cooldownHeatingPumpWasOn_ = false;
        cooldownWaterPumpWasOn_ = false;
    }

    // Check pump protection for water pump
    auto pumpCheck = pumpCoordinator_.checkPumpChangeAllowed(PumpType::WATER, true);
    if (pumpCheck.isError()) {
        LOG_WARN(TAG, "Water pump change blocked: %s", pumpCheck.message());
        return pumpCheck;
    }

    // Determine power level
    PowerLevel actualPower = (power == PowerLevel::AUTO) ? PowerLevel::FULL : power;

    // Build relay states for water mode
    // Water pump ON, heating pump OFF, burner in water mode
    auto relayStates = buildRelayStates(BurnerMode::WATER, actualPower, false, true);

    // Execute atomic batch command
    auto batchResult = executeRelayBatch(relayStates);
    if (batchResult.isError()) {
        return batchResult;
    }

    // Record pump state changes (for 30s protection)
    pumpCoordinator_.recordPumpStateChange(PumpType::WATER, true);
    if (currentMode_ == BurnerMode::HEATING) {
        // Switched from heating - record heating pump going OFF
        pumpCoordinator_.recordPumpStateChange(PumpType::HEATING, false);
    }

    // Update internal state
    currentMode_ = BurnerMode::WATER;
    currentPower_ = actualPower;
    currentTarget_ = targetTemp;
    isActive_ = true;

    // Clear heat recovery mode (water takes priority)
    if (inHeatRecoveryMode_) {
        LOG_INFO(TAG, "Heat recovery cancelled - water priority active");
        inHeatRecoveryMode_ = false;
    }

    LOG_INFO(TAG, "Water mode activated successfully");
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
    LOG_INFO(TAG, "Seamless mode switch: %s → %s (target: %s, power: %s)",
             currentMode_ == BurnerMode::HEATING ? "HEATING" : "WATER",
             newMode == BurnerMode::HEATING ? "HEATING" : "WATER",
             tempBuf,
             currentPower_ == PowerLevel::FULL ? "FULL" : "HALF");

    // Build relay states: keep burner ON, switch pumps atomically
    bool heatingPump = (newMode == BurnerMode::HEATING);
    bool waterPump = (newMode == BurnerMode::WATER);

    auto relayStates = buildRelayStates(newMode, currentPower_,
                                       heatingPump, waterPump);

    // Execute atomic batch command
    auto batchResult = executeRelayBatch(relayStates);
    if (batchResult.isError()) {
        LOG_ERROR(TAG, "Mode switch relay batch failed: %s",
                 batchResult.message().c_str());
        return batchResult;
    }

    // Record pump state changes
    // NOTE: Bypasses 30s protection - mode switch is safe exception because:
    // 1. Burner stays running (heat dissipation active)
    // 2. Only one pump changes at a time (make-before-break)
    // 3. Mode switches are rare (~few per day) vs normal cycling
    if (newMode == BurnerMode::HEATING) {
        pumpCoordinator_.recordPumpStateChange(PumpType::HEATING, true);
        pumpCoordinator_.recordPumpStateChange(PumpType::WATER, false);
    } else {
        pumpCoordinator_.recordPumpStateChange(PumpType::WATER, true);
        pumpCoordinator_.recordPumpStateChange(PumpType::HEATING, false);
    }

    // Update internal state
    currentMode_ = newMode;
    currentTarget_ = targetTemp;

    LOG_INFO(TAG, "Mode switch complete (seamless transition in <100ms)");
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

    LOG_INFO(TAG, "Deactivating burner system");

    // Build relay states: all burner relays OFF, pumps stay as they are (safety)
    // Keep pumps running to dissipate heat
    bool heatingPumpState = pumpCoordinator_.isPumpOn(PumpType::HEATING);
    bool waterPumpState = pumpCoordinator_.isPumpOn(PumpType::WATER);

    // When OFF, use FULL power (R4=OFF) - HALF would leave R4 energized unnecessarily
    auto relayStates = buildRelayStates(BurnerMode::OFF, PowerLevel::FULL,
                                       heatingPumpState, waterPumpState);

    // Execute batch command
    auto batchResult = executeRelayBatch(relayStates);
    if (batchResult.isError()) {
        return batchResult;
    }

    // Update state
    isActive_ = false;
    currentMode_ = BurnerMode::OFF;

    // Start cooldown timer if any pump was running
    if (heatingPumpState || waterPumpState) {
        inCooldownMode_ = true;
        cooldownStartTime_ = millis();
        cooldownHeatingPumpWasOn_ = heatingPumpState;
        cooldownWaterPumpWasOn_ = waterPumpState;
        LOG_INFO(TAG, "Burner deactivated - cooldown started (pumps: H=%d W=%d, duration: %lu ms)",
                 heatingPumpState, waterPumpState, SystemConstants::Timing::PUMP_COOLDOWN_MS);
    } else {
        LOG_INFO(TAG, "Burner deactivated (no pumps were running)");
    }

    return Result<void>();
}

Result<void> BurnerSystemController::switchToHeatRecovery() {
    if (!mutex_) {
        return Result<void>(SystemError::MUTEX_TIMEOUT, "Mutex not initialized");
    }

    MutexGuard lock(mutex_);

    LOG_INFO(TAG, "Attempting heat recovery mode switch");

    // 1. Check temperature conditions (is it worth switching?)
    auto& sensors = SRP::getSensorReadings();
    Temperature_t boilerOutput = sensors.boilerTempOutput;
    Temperature_t boilerReturn = sensors.boilerTempReturn;
    Temperature_t differential = tempSub(boilerOutput, boilerReturn);

    // Minimum boiler output temperature (35°C)
    if (tempLess(boilerOutput, tempFromWhole(35))) {
        char tempBuf[16];
        formatTemp(tempBuf, sizeof(tempBuf), boilerOutput);
        LOG_INFO(TAG, "Heat recovery skipped - boiler too cold (%s < 35°C)", tempBuf);
        return Result<void>(SystemError::INVALID_PARAMETER,
                           "Boiler temperature too low for heat recovery");
    }

    // Minimum differential (10°C)
    if (tempLess(differential, tempFromWhole(10))) {
        char diffBuf[16];
        formatTemp(diffBuf, sizeof(diffBuf), differential);
        LOG_INFO(TAG, "Heat recovery skipped - differential too low (%s < 10°C)", diffBuf);
        return Result<void>(SystemError::INVALID_PARAMETER,
                           "Insufficient temperature differential");
    }

    // 2. Check pump protection (can we switch to heating pump?)
    auto pumpCheck = pumpCoordinator_.checkPumpChangeAllowed(PumpType::HEATING, true);
    if (pumpCheck.isError()) {
        LOG_WARN(TAG, "Heat recovery blocked by pump protection: %s", pumpCheck.message().c_str());
        return pumpCheck;
    }

    // 3. Build relay states: heating pump ON, water pump OFF, burner OFF
    // Use FULL power level so R4=OFF (not energized when burner is off)
    auto relayStates = buildRelayStates(BurnerMode::OFF, PowerLevel::FULL, true, false);

    // 4. Execute atomic batch command
    auto batchResult = executeRelayBatch(relayStates);
    if (batchResult.isError()) {
        return batchResult;
    }

    // 5. Record pump state changes
    pumpCoordinator_.recordPumpStateChange(PumpType::HEATING, true);
    pumpCoordinator_.recordPumpStateChange(PumpType::WATER, false);

    // 6. Enter heat recovery mode
    inHeatRecoveryMode_ = true;
    heatRecoveryStartTime_ = millis();
    isActive_ = false;  // Burner not active (using residual heat only)
    currentMode_ = BurnerMode::HEATING;  // Heating circulation but no burner

    char boilerBuf[16], diffBuf[16];
    formatTemp(boilerBuf, sizeof(boilerBuf), boilerOutput);
    formatTemp(diffBuf, sizeof(diffBuf), differential);
    LOG_INFO(TAG, "Heat recovery mode active - using residual heat (boiler: %s, diff: %s)",
             boilerBuf, diffBuf);

    return Result<void>();
}

bool BurnerSystemController::shouldActivateHeatingBurner() const {
    if (!mutex_) return true;  // Fail-safe: allow activation

    MutexGuard lock(mutex_);

    // Not in heat recovery - always allow burner activation
    if (!inHeatRecoveryMode_) {
        return true;
    }

    auto& sensors = SRP::getSensorReadings();
    Temperature_t boilerOutput = sensors.boilerTempOutput;
    uint32_t elapsed = millis() - heatRecoveryStartTime_;

    // Check if residual heat exhausted (two conditions - either triggers exit)

    // Condition 1: Time-based minimum (2 minutes)
    if (elapsed >= 120000) {  // 2 minutes
        LOG_INFO(TAG, "Heat recovery time expired (%lu s) - allowing burner activation",
                 elapsed / 1000);
        // Don't clear flag here - will be cleared when burner activates
        return true;
    }

    // Condition 2: Temperature-based (boiler below 40°C)
    if (tempLess(boilerOutput, tempFromWhole(40))) {
        char tempBuf[16];
        formatTemp(tempBuf, sizeof(tempBuf), boilerOutput);
        LOG_INFO(TAG, "Heat recovery heat exhausted (boiler: %s < 40°C) - allowing burner activation",
                 tempBuf);
        // Don't clear flag here - will be cleared when burner activates
        return true;
    }

    // Still have residual heat - block burner activation
    char tempBuf[16];
    formatTemp(tempBuf, sizeof(tempBuf), boilerOutput);
    LOG_DEBUG(TAG, "Heat recovery active - blocking burner (time: %lu/120 s, boiler: %s)",
              elapsed / 1000, tempBuf);

    return false;  // Don't activate burner yet (using residual heat)
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

std::array<bool, 8> BurnerSystemController::buildRelayStates(BurnerMode mode, PowerLevel power,
                                                             bool heatingPump, bool waterPump) {
    // RYN4 relay mapping - see RelayIndices.h for single source of truth
    // Physical relay = array index + 1

    std::array<bool, 8> states = {};  // Initialize all to false

    // Pumps
    states[RelayIndex::HEATING_PUMP] = heatingPump;
    states[RelayIndex::WATER_PUMP]   = waterPump;

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

    return states;
}

Result<void> BurnerSystemController::executeRelayBatch(const std::array<bool, 8>& states) {
    // Use RelayControlTask batch command (single Modbus transaction)
    bool success = RelayControlTask::setMultipleRelays(states);

    if (!success) {
        LOG_ERROR(TAG, "Batch relay command failed");
        return Result<void>(SystemError::RELAY_OPERATION_FAILED,
                           "Batch relay command failed");
    }

    LOG_DEBUG(TAG, "Batch relay command succeeded (HeatingPump:%d WaterPump:%d BurnerEnable:%d PowerBoost:%d WaterMode:%d)",
              states[RelayIndex::HEATING_PUMP], states[RelayIndex::WATER_PUMP],
              states[RelayIndex::BURNER_ENABLE], states[RelayIndex::POWER_BOOST], states[RelayIndex::WATER_MODE]);

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

    // Only POWER_SELECT relay changes - use single relay command (not batch)
    // Relay 4: 1=half power, 0=full power (setRelayState uses 1-based indexing)
    bool success = RelayControlTask::setRelayState(4, power == PowerLevel::HALF);

    if (!success) {
        LOG_ERROR(TAG, "Failed to set power level relay");
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

bool BurnerSystemController::checkAndHandleCooldown() {
    if (!mutex_) return false;

    MutexGuard lock(mutex_);

    if (!inCooldownMode_) {
        return false;
    }

    // Safety check: don't turn off pumps if burner has been reactivated
    // (This should not happen - activation should cancel cooldown, but belt-and-suspenders)
    if (isActive_) {
        LOG_WARN(TAG, "Cooldown aborted - burner is active (should have been cancelled)");
        inCooldownMode_ = false;
        cooldownHeatingPumpWasOn_ = false;
        cooldownWaterPumpWasOn_ = false;
        return false;
    }

    uint32_t elapsed = millis() - cooldownStartTime_;
    if (elapsed < SystemConstants::Timing::PUMP_COOLDOWN_MS) {
        // Still in cooldown
        return false;
    }

    // Cooldown complete - turn off pumps
    LOG_INFO(TAG, "Cooldown complete (%lu ms) - turning off pumps", elapsed);

    // Build relay states: all OFF (use FULL so R4=OFF, not energized)
    auto relayStates = buildRelayStates(BurnerMode::OFF, PowerLevel::FULL, false, false);

    // Round 19 Issue #2: Only record state changes if batch command succeeds
    // If command fails, don't record phantom state - retry on next call
    auto result = executeRelayBatch(relayStates);
    if (result.isError()) {
        LOG_ERROR(TAG, "Failed to turn off pumps after cooldown: %s - will retry", result.message());
        // DO NOT clear cooldown state or record pump changes
        // Will retry on next checkAndHandleCooldown() call
        return false;
    }

    // Batch command succeeded - record pump state changes for motor protection
    if (cooldownHeatingPumpWasOn_) {
        pumpCoordinator_.recordPumpStateChange(PumpType::HEATING, false);
    }
    if (cooldownWaterPumpWasOn_) {
        pumpCoordinator_.recordPumpStateChange(PumpType::WATER, false);
    }

    // Clear cooldown state
    inCooldownMode_ = false;
    cooldownHeatingPumpWasOn_ = false;
    cooldownWaterPumpWasOn_ = false;

    return true;
}

bool BurnerSystemController::isInCooldown() const {
    if (!mutex_) return false;
    MutexGuard lock(mutex_);
    return inCooldownMode_;
}
