// src/modules/control/BurnerStateMachine.cpp
#include "BurnerStateMachine.h"
#include "modules/control/SafetyInterlocks.h"  // Add safety interlocks
#include "modules/control/BurnerAntiFlapping.h"  // Add anti-flapping
#include "modules/control/BurnerSystemController.h"  // Unified burner control with batch relays
#include "modules/control/CentralizedFailsafe.h"  // Failsafe system
#include "modules/control/BurnerSafetyChecks.h"  // Round 21: Extracted safety checks
#include "modules/control/BurnerPowerController.h"  // Round 21: Extracted power control
#include "modules/control/BurnerRuntimeTracker.h"  // Round 21: Extracted runtime tracking
#include "modules/tasks/RelayControlTask.h"
#include "modules/tasks/MQTTTask.h"  // H15: For error recovery notification
#include "MQTTTopics.h"  // C1: Centralized topic macros (prevents hardcoded string drift)
#include "shared/SharedResources.h"
#include "config/RelayIndices.h"
#include "shared/Temperature.h"
#include "monitoring/HealthMonitor.h"
#include "utils/ErrorHandler.h"
#include "utils/MutexRetryHelper.h"
#include "utils/CriticalDataStorage.h"
#include "config/SafetyConfig.h"
#include "config/SystemSettingsStruct.h"  // For seamless mode switch - check actual temp vs target
#include <MutexGuard.h>  // For runtime counter persistence
#include "LoggingMacros.h"
#include "events/SystemEventsGenerated.h"
#include "core/SharedResourceManager.h"
#include "core/SystemResourceProvider.h"
#include "shared/SharedSensorReadings.h"
#include "shared/SharedRelayReadings.h"
#include "utils/Utils.h"  // Round 15 Issue #1, #3: Safe elapsed time helpers
#include <cmath>
#include <atomic>  // Round 14 Issue #2
#include <RuntimeStorage.h>

// No external declarations needed - using SRP methods

// Static member definitions
StateMachine<BurnerSMState> BurnerStateMachine::stateMachine("BurnerSM", BurnerSMState::IDLE);
const char* BurnerStateMachine::TAG = "BurnerStateMachine";
// Round 15 Issue #2 note: ignitionRetries intentionally NOT persisted to FRAM
// Rationale: Power cycle should reset retry count because:
// 1. User may have fixed the underlying issue (gas supply, sensor, etc.)
// 2. Starting fresh after power cycle is safer than inheriting old failure state
// 3. Repeated power cycles during ignition failures indicate electrical issues
uint8_t BurnerStateMachine::ignitionRetries = 0;
bool BurnerStateMachine::heatDemand = false;
Temperature_t BurnerStateMachine::targetTemperature = 0;
bool BurnerStateMachine::requestedHighPower = false;
SemaphoreHandle_t BurnerStateMachine::demandMutex = nullptr;

// ============================================================================
// THREAD-SAFETY NOTE (Round 14 Issue #1, Round 20 Issue #8):
// These variables are only accessed by BurnerStateMachine state callbacks which
// are all called from BurnerControlTask (single task context via update()).
// Made std::atomic as a defensive measure and to be explicit about thread-safety.
// DO NOT access these variables from other tasks.
// ============================================================================
// Round 21: burnerStartTime moved to BurnerRuntimeTracker
static std::atomic<bool> runningModeIsWater{false};    // Round 19 Issue #1: Track mode for switch detection
static std::atomic<uint32_t> errorStateEntryTime{0};   // Round 19 Issue #5: Track when ERROR state was entered
static std::atomic<uint32_t> postPurgeEntryTime{0};    // M6: Track when POST_PURGE state was entered for runtime-configurable duration

void BurnerStateMachine::initialize() {
    LOG_INFO(TAG, "Initializing burner state machine");

    // Create mutex for thread-safe demand access
    if (demandMutex == nullptr) {
        demandMutex = xSemaphoreCreateMutex();
        if (demandMutex == nullptr) {
            LOG_ERROR(TAG, "Failed to create demand mutex!");
        }
    }

    // Initialize anti-flapping system
    BurnerAntiFlapping::initialize();
    
    // Register all states with their configurations
    stateMachine.registerState(BurnerSMState::IDLE, {
        .handler = handleIdleState,
        .onEntry = nullptr,
        .onExit = nullptr,
        .timeoutMs = 0,
        .timeoutNextState = BurnerSMState::IDLE
    });
    
    stateMachine.registerState(BurnerSMState::PRE_PURGE, {
        .handler = handlePrePurgeState,
        .onEntry = onEnterPrePurge,
        .onExit = nullptr,
        .timeoutMs = PRE_PURGE_TIME_MS,
        .timeoutNextState = BurnerSMState::IGNITION
    });
    
    stateMachine.registerState(BurnerSMState::IGNITION, {
        .handler = handleIgnitionState,
        .onEntry = onEnterIgnition,
        .onExit = nullptr,
        .timeoutMs = IGNITION_TIME_MS,
        .timeoutNextState = BurnerSMState::LOCKOUT
    });
    
    stateMachine.registerState(BurnerSMState::RUNNING_LOW, {
        .handler = handleRunningLowState,
        .onEntry = onEnterRunningLow,
        .onExit = onExitRunning,
        .timeoutMs = 0,
        .timeoutNextState = BurnerSMState::RUNNING_LOW
    });
    
    stateMachine.registerState(BurnerSMState::RUNNING_HIGH, {
        .handler = handleRunningHighState,
        .onEntry = onEnterRunningHigh,
        .onExit = onExitRunning,
        .timeoutMs = 0,
        .timeoutNextState = BurnerSMState::RUNNING_HIGH
    });

    stateMachine.registerState(BurnerSMState::MODE_SWITCHING, {
        .handler = handleModeSwitchingState,
        .onEntry = onEnterModeSwitching,
        .onExit = nullptr,
        .timeoutMs = 0,  // Poll handler immediately (like RUNNING states)
        .timeoutNextState = BurnerSMState::MODE_SWITCHING  // No timeout
    });

    stateMachine.registerState(BurnerSMState::POST_PURGE, {
        .handler = handlePostPurgeState,
        .onEntry = onEnterPostPurge,
        .onExit = nullptr,
        .timeoutMs = 0,  // M6: Disabled - using manual timeout check for runtime-configurable duration
        .timeoutNextState = BurnerSMState::IDLE
    });
    
    stateMachine.registerState(BurnerSMState::LOCKOUT, {
        .handler = handleLockoutState,
        .onEntry = onEnterLockout,
        .onExit = onExitLockout,
        .timeoutMs = LOCKOUT_TIME_MS,
        .timeoutNextState = BurnerSMState::IDLE
    });
    
    stateMachine.registerState(BurnerSMState::ERROR, {
        .handler = handleErrorState,
        .onEntry = onEnterError,
        .onExit = nullptr,
        .timeoutMs = 0,
        .timeoutNextState = BurnerSMState::ERROR
    });
    
    // Set transition callback
    stateMachine.setTransitionCallback(logStateTransition);
    
    // Initialize the state machine
    stateMachine.initialize();
}

void BurnerStateMachine::update() {
    // Perform continuous safety monitoring
    // Only check safety during actual burner operation states
    BurnerSMState currentState = stateMachine.getCurrentState();

    // C8: Validate state is within valid enum range
    // If state becomes corrupted (memory error, cosmic ray), trigger emergency stop
    if (currentState < BurnerSMState::IDLE || currentState > BurnerSMState::ERROR) {
        LOG_ERROR(TAG, "INVALID STATE DETECTED: %d - triggering emergency stop!",
                 static_cast<int>(currentState));
        emergencyStop();
        xEventGroupSetBits(SRP::getSystemStateEventGroup(),
                          SystemEvents::SystemState::BURNER_ERROR);
        return;
    }

    if (currentState == BurnerSMState::IGNITION ||
        currentState == BurnerSMState::RUNNING_LOW ||
        currentState == BurnerSMState::RUNNING_HIGH) {

        // Check safety interlocks during operation
        if (!SafetyInterlocks::continuousSafetyMonitor()) {
            LOG_ERROR(TAG, "Safety interlock failed during operation!");
            emergencyStop();
            return;
        }
    }

    stateMachine.update();
}

void BurnerStateMachine::setHeatDemand(bool demand, Temperature_t target, bool highPower) {
    if (demandMutex == nullptr) {
        LOG_ERROR(TAG, "setHeatDemand: demandMutex is null");
        return;
    }

    auto guard = MutexRetryHelper::acquireGuard(demandMutex, "BurnerSM-SetDemand");
    if (!guard) {
        LOG_ERROR(TAG, "setHeatDemand: Failed to acquire demand mutex");
        return;
    }

    // Only update and log if something actually changed
    bool demandChanged = (heatDemand != demand);
    bool targetChanged = (target > 0 && tempAbs(tempSub(targetTemperature, target)) > 1);  // > 0.1°C difference
    bool powerChanged = (requestedHighPower != highPower);

    if (demandChanged || targetChanged || powerChanged) {
        heatDemand = demand;
        requestedHighPower = highPower;
        if (target > 0) {
            targetTemperature = target;
        }
        char tempBuf[16];
        formatTemp(tempBuf, sizeof(tempBuf), targetTemperature);
        LOG_INFO(TAG, "Heat demand: %s, target: %s°C, power: %s",
                 demand ? "ON" : "OFF", tempBuf, highPower ? "HIGH" : "LOW");
    }
}

void BurnerStateMachine::emergencyStop() {
    // H2: Re-entry protection - prevent multiple concurrent emergency stops
    static std::atomic<bool> emergencyStopInProgress{false};
    if (emergencyStopInProgress.exchange(true)) {
        LOG_WARN(TAG, "Emergency stop already in progress - skipping");
        return;
    }

    LOG_ERROR(TAG, "Emergency stop requested");
    BurnerSystemController* controller = SRP::getBurnerSystemController();
    if (controller) {
        controller->emergencyShutdown("BurnerStateMachine emergency stop");
    }

    // Round 19 Issue #4: Explicitly clear BURNER_ON bit regardless of current state
    // Emergency stop can be called from any state, not just RUNNING states.
    // If called from non-RUNNING state, onExitRunning() won't be triggered,
    // so we must clear the bit here to ensure clean state.
    xEventGroupClearBits(SRP::getSystemStateEventGroup(),
                        SystemEvents::SystemState::BURNER_ON);

    stateMachine.transitionTo(BurnerSMState::ERROR);

    // Allow future emergency stops after transition completes
    emergencyStopInProgress = false;
}

BurnerSMState BurnerStateMachine::getCurrentState() {
    return stateMachine.getCurrentState();
}

bool BurnerStateMachine::getHeatDemandState(bool& outDemand, Temperature_t& outTarget) {
    if (demandMutex == nullptr) {
        LOG_WARN(TAG, "getHeatDemandState: demandMutex is null");
        return false;
    }

    auto guard = MutexRetryHelper::acquireGuard(demandMutex, "BurnerSM-GetDemand");
    if (!guard) {
        LOG_WARN(TAG, "getHeatDemandState: Failed to acquire demand mutex");
        return false;
    }

    outDemand = heatDemand;
    outTarget = targetTemperature;
    return true;
}

void BurnerStateMachine::resetLockout() {
    if (stateMachine.isInState(BurnerSMState::LOCKOUT)) {
        LOG_INFO(TAG, "Resetting lockout state");
        ignitionRetries = 0;
        // Clear error bit when resetting lockout
        xEventGroupClearBits(SRP::getSystemStateEventGroup(), SystemEvents::SystemState::BURNER_ERROR);
        ErrorHandler::clearErrorRateLimit(SystemError::SYSTEM_FAILSAFE_TRIGGERED);
        stateMachine.transitionTo(BurnerSMState::IDLE);
    }
}

// State Handlers Implementation

BurnerSMState BurnerStateMachine::handleIdleState() {
    // Check for heat demand and safety conditions
    if (heatDemand && BurnerSafetyChecks::checkSafetyConditions()) {
        // Check anti-flapping before turning on
        if (BurnerAntiFlapping::canTurnOn()) {
            return BurnerSMState::PRE_PURGE;
        } else {
            LOG_DEBUG(TAG, "Delaying burner start for %lu ms due to anti-flapping", 
                     BurnerAntiFlapping::getTimeUntilCanTurnOn());
        }
    }
    return BurnerSMState::IDLE;
}

BurnerSMState BurnerStateMachine::handlePrePurgeState() {
    // Pre-purge is handled by timeout - don't force state
    // Just check safety conditions
    if (!BurnerSafetyChecks::checkSafetyConditions()) {
        return BurnerSMState::ERROR;
    }
    // Return current state to let timeout mechanism handle transition
    return stateMachine.getCurrentState();
}

BurnerSMState BurnerStateMachine::handleIgnitionState() {
    // Wait minimum ignition time before checking flame
    // Real burner ignition takes 3-5 seconds; simulated flame detection returns immediately
    uint32_t timeInState = stateMachine.getTimeInState();
    if (timeInState < SystemConstants::Timing::BURNER_MIN_IGNITION_TIME_MS) {
        return BurnerSMState::IGNITION;
    }

    // Check if flame is detected (after minimum time elapsed)
    if (BurnerSafetyChecks::isFlameDetected()) {
        ignitionRetries = 0;

        // Determine which power level to use based on demand
        if (BurnerPowerController::shouldIncreasePower(requestedHighPower)) {
            LOG_INFO(TAG, "Ignition successful after %lu ms - transitioning to high power", timeInState);
            return BurnerSMState::RUNNING_HIGH;
        } else {
            LOG_INFO(TAG, "Ignition successful after %lu ms - transitioning to low power", timeInState);
            return BurnerSMState::RUNNING_LOW;
        }
    }

    // If timeout occurs, retry or lockout
    if (timeInState >= IGNITION_TIME_MS) {
        ignitionRetries++;
        if (ignitionRetries >= MAX_IGNITION_RETRIES) {
            LOG_ERROR(TAG, "Max ignition retries exceeded");
            return BurnerSMState::LOCKOUT;
        } else {
            LOG_WARN(TAG, "Ignition retry %d/%d", ignitionRetries, MAX_IGNITION_RETRIES);
            return BurnerSMState::PRE_PURGE;
        }
    }

    return BurnerSMState::IGNITION;
}


BurnerSMState BurnerStateMachine::handleRunningLowState() {
    // 1. Check mode switch (water ↔ heating)
    bool currentMode = runningModeIsWater.load();
    BurnerSMState modeTransition = BurnerSafetyChecks::checkModeSwitchTransition(
        BurnerSMState::RUNNING_LOW, "RUNNING_LOW", currentMode);
    if (modeTransition != BurnerSMState::IDLE) {
        return modeTransition;
    }

    // 2. Check safety shutdown conditions (extracted)
    BurnerSMState shutdownCheck = BurnerSafetyChecks::checkSafetyShutdown(BurnerSMState::RUNNING_LOW, heatDemand);
    if (shutdownCheck != BurnerSMState::RUNNING_LOW) {
        return shutdownCheck;
    }

    // 3. Check flame loss (extracted)
    BurnerSMState flameCheck = BurnerSafetyChecks::checkFlameLoss(BurnerSMState::RUNNING_LOW, heatDemand);
    if (flameCheck != BurnerSMState::RUNNING_LOW) {
        return flameCheck;
    }

    // 4. Check if we need more power (only difference from RUNNING_HIGH)
    if (BurnerPowerController::shouldIncreasePower(requestedHighPower)) {
        // Check anti-flapping for power level change
        if (BurnerAntiFlapping::canChangePowerLevel(BurnerAntiFlapping::PowerLevel::POWER_HIGH)) {
            return BurnerSMState::RUNNING_HIGH;
        } else {
            LOG_DEBUG(TAG, "Delaying power increase for %lu ms due to anti-flapping",
                     BurnerAntiFlapping::getTimeUntilCanChangePower());
        }
    }

    return BurnerSMState::RUNNING_LOW;
}

BurnerSMState BurnerStateMachine::handleRunningHighState() {
    // 1. Check mode switch (water ↔ heating)
    bool currentMode = runningModeIsWater.load();
    BurnerSMState modeTransition = BurnerSafetyChecks::checkModeSwitchTransition(
        BurnerSMState::RUNNING_HIGH, "RUNNING_HIGH", currentMode);
    if (modeTransition != BurnerSMState::IDLE) {
        return modeTransition;
    }

    // 2. Check safety shutdown conditions (extracted)
    BurnerSMState shutdownCheck = BurnerSafetyChecks::checkSafetyShutdown(BurnerSMState::RUNNING_HIGH, heatDemand);
    if (shutdownCheck != BurnerSMState::RUNNING_HIGH) {
        return shutdownCheck;
    }

    // 3. Check flame loss (extracted)
    BurnerSMState flameCheck = BurnerSafetyChecks::checkFlameLoss(BurnerSMState::RUNNING_HIGH, heatDemand);
    if (flameCheck != BurnerSMState::RUNNING_HIGH) {
        return flameCheck;
    }

    // 4. Check if we can reduce power (only difference from RUNNING_LOW)
    if (BurnerPowerController::shouldDecreasePower(requestedHighPower)) {
        // Check anti-flapping for power level change
        if (BurnerAntiFlapping::canChangePowerLevel(BurnerAntiFlapping::PowerLevel::POWER_LOW)) {
            return BurnerSMState::RUNNING_LOW;
        } else {
            LOG_DEBUG(TAG, "Delaying power decrease for %lu ms due to anti-flapping",
                     BurnerAntiFlapping::getTimeUntilCanChangePower());
        }
    }

    return BurnerSMState::RUNNING_HIGH;
}

BurnerSMState BurnerStateMachine::handlePostPurgeState() {
    // SM-CRIT-1: Defensive init if entry action was bypassed (state machine corruption)
    // Without this, Utils::elapsedMs(0) returns millis() (~4.3B), bypassing post-purge
    if (postPurgeEntryTime == 0) {
        postPurgeEntryTime = millis();
        LOG_WARN(TAG, "POST_PURGE entered without onEntry - initializing timer");
    }

    // M6: Use runtime-configurable post-purge duration (default 90s, range 30s-3min)
    // Manual timeout check allows changing postPurgeMs via MQTT without reboot
    uint32_t postPurgeDurationMs = SafetyConfig::postPurgeMs;

    uint32_t timeInPostPurge = Utils::elapsedMs(postPurgeEntryTime);
    if (timeInPostPurge >= postPurgeDurationMs) {
        LOG_INFO(TAG, "Post-purge complete after %lu ms", timeInPostPurge);
        postPurgeEntryTime = 0;  // Reset for next post-purge
        return BurnerSMState::IDLE;
    }

    // Note: Round 17 Issue X fix is in StateManager - burner should NOT enter POST_PURGE
    // during mode transitions because WATER_PRIORITY_RELEASED triggers immediate heating handoff
    return BurnerSMState::POST_PURGE;
}

BurnerSMState BurnerStateMachine::handleLockoutState() {
    // Lockout can only be reset manually or by timeout
    return BurnerSMState::LOCKOUT;
}

BurnerSMState BurnerStateMachine::handleErrorState() {
    // SM-HIGH-3: Defensive init if entry action was bypassed (state machine corruption)
    // Without this, Utils::elapsedMs(0) returns millis() (~4.3B), bypassing recovery delay
    if (errorStateEntryTime == 0) {
        errorStateEntryTime = millis();
        LOG_ERROR(TAG, "ERROR state entered without onEntry - initializing recovery timer");
    }

    // M4: Use runtime-configurable error recovery delay (default 5 min, range 1-30 min)
    // This prevents rapid ERROR ↔ IDLE ↔ RUNNING cycling with intermittent faults
    uint32_t recoveryDelayMs = SafetyConfig::errorRecoveryMs;

    uint32_t timeInError = Utils::elapsedMs(errorStateEntryTime);

    // H15: Publish recovery status periodically so users know how long to wait
    static uint32_t lastStatusPublish = 0;
    uint32_t now = millis();
    if (now - lastStatusPublish > SystemConstants::Burner::STATUS_PUBLISH_INTERVAL_MS) {
        lastStatusPublish = now;
        uint32_t remainingMs = (timeInError < recoveryDelayMs)
                             ? (recoveryDelayMs - timeInError) : 0;
        uint32_t remainingSec = remainingMs / 1000;

        char buffer[64];
        snprintf(buffer, sizeof(buffer),
                "{\"state\":\"error\",\"recovery_in\":%lu}", remainingSec);
        MQTTTask::publish(MQTT_STATUS_BURNER, buffer, 0, false, MQTTPriority::PRIORITY_MEDIUM);
    }

    if (timeInError < recoveryDelayMs) {
        // Still in mandatory hold period - no auto-recovery yet
        // User can still use resetLockout() for manual recovery
        return BurnerSMState::ERROR;
    }

    // After delay, check if safety conditions are restored
    if (BurnerSafetyChecks::checkSafetyConditions()) {
        // Clear error bit before transitioning to IDLE
        xEventGroupClearBits(SRP::getSystemStateEventGroup(), SystemEvents::SystemState::BURNER_ERROR);
        // Clear error rate limiting since error is resolved
        ErrorHandler::clearErrorRateLimit(SystemError::SYSTEM_FAILSAFE_TRIGGERED);
        LOG_INFO(TAG, "Safety conditions restored after %lu s - returning to IDLE",
                 timeInError / 1000);
        errorStateEntryTime = 0;  // Reset for next error
        return BurnerSMState::IDLE;
    }
    return BurnerSMState::ERROR;
}

// Entry/Exit Actions Implementation

void BurnerStateMachine::onEnterPrePurge() {
    LOG_INFO(TAG, "Starting pre-purge sequence");
    // Ensure burner is off
    BurnerSystemController* controller = SRP::getBurnerSystemController();
    if (controller) {
        // L10: Check deactivate result - failure here is a safety concern
        auto result = controller->deactivate();
        if (result.isError()) {
            LOG_ERROR(TAG, "PRE-PURGE: Failed to deactivate burner: %s", result.message().c_str());
            // SM-HIGH-2: Entry action failure must abort - don't proceed to ignition
            // with burner potentially still active
            emergencyStop();
            return;
        }
    }
    // Start exhaust fan if available
    // Could add fan control here
}

void BurnerStateMachine::onEnterIgnition() {
    LOG_INFO(TAG, "Starting ignition sequence");

    // Increment burner start counter in FRAM
    rtstorage::RuntimeStorage* storage = SRP::getRuntimeStorage();
    if (storage) {
        if (storage->incrementCounter(rtstorage::COUNTER_BURNER_STARTS)) {
            uint32_t count = storage->getCounter(rtstorage::COUNTER_BURNER_STARTS);
            LOG_INFO(TAG, "Burner start count: %lu", count);
        }
    }

    // Get BurnerSystemController for batch relay commands
    BurnerSystemController* controller = SRP::getBurnerSystemController();

    // Determine mode - water or heating
    // When both WATER_ON and HEATING_ON are set, use WATER_PRIORITY to decide
    EventBits_t systemBits = xEventGroupGetBits(SRP::getSystemStateEventGroup());
    bool waterOn = (systemBits & SystemEvents::SystemState::WATER_ON) != 0;
    bool heatingOn = (systemBits & SystemEvents::SystemState::HEATING_ON) != 0;
    bool waterPriority = (systemBits & SystemEvents::SystemState::WATER_PRIORITY) != 0;

    // Water mode if: water is on AND (heating is off OR water has priority)
    bool isWaterMode = waterOn && (!heatingOn || waterPriority);

    // Round 19 Issue #1: Track the mode we're starting in for switch detection
    runningModeIsWater = isWaterMode;

    // Use the power level from setHeatDemand() - this is set by BoilerTempController
    // based on actual temperature error, not from event bits
    bool highPowerRequested = requestedHighPower;
    PowerLevel startPower = highPowerRequested ? PowerLevel::FULL : PowerLevel::HALF;
    LOG_INFO(TAG, "Starting with %s power (from BoilerTempController)", highPowerRequested ? "FULL" : "HALF");

    // Record actual power level for anti-flapping (skipped in transition callback)
    BurnerAntiFlapping::PowerLevel afLevel = highPowerRequested ?
        BurnerAntiFlapping::PowerLevel::POWER_HIGH : BurnerAntiFlapping::PowerLevel::POWER_LOW;
    BurnerAntiFlapping::recordPowerLevelChange(afLevel);

    // Activate burner via BurnerSystemController (burner relays only - pumps are independent)
    Result<void> activationResult;
    if (controller) {
        if (isWaterMode) {
            LOG_INFO(TAG, "Activating water mode via BurnerSystemController");
            activationResult = controller->activateWaterMode(targetTemperature, startPower);
        } else {
            LOG_INFO(TAG, "Activating heating mode via BurnerSystemController");
            activationResult = controller->activateHeatingMode(targetTemperature, startPower);
        }

        if (activationResult.isError()) {
            LOG_ERROR(TAG, "ABORT IGNITION: %s", activationResult.message().c_str());
            xEventGroupSetBits(SRP::getSystemStateEventGroup(), SystemEvents::SystemState::BURNER_ERROR);
            return;
        }
    }

    // Ignition delay - allow flow to establish
    // Pump is controlled independently by PumpControlModule (watches HEATING_ON/WATER_ON bits)
    // If pump physically fails, system detects via temperature/flow sensors
    vTaskDelay(pdMS_TO_TICKS(SystemConstants::Timing::BURNER_IGNITION_DELAY_MS));

    // Burner already activated via BurnerSystemController batch command above
}

void BurnerStateMachine::onEnterRunningLow() {
    LOG_INFO(TAG, "Entering low power operation");
    BurnerSystemController* controller = SRP::getBurnerSystemController();
    if (controller) {
        // L10: Check power level change result
        auto result = controller->setPowerLevel(PowerLevel::HALF);
        if (result.isError()) {
            LOG_ERROR(TAG, "RUNNING_LOW: Failed to set power level - entering failsafe");

            // Trigger centralized failsafe with DEGRADED level
            CentralizedFailsafe::triggerFailsafe(
                CentralizedFailsafe::FailsafeLevel::DEGRADED,
                SystemError::RELAY_OPERATION_FAILED,
                "Failed to set power level to LOW"
            );

            // Emergency shutdown to prevent operation at wrong power level
            emergencyStop();
            return;  // onEntry action aborted - state machine will transition to ERROR
        }
    }
    // Clear any error bits
    xEventGroupClearBits(SRP::getSystemStateEventGroup(), SystemEvents::SystemState::BURNER_ERROR);
    // Set system burner on bit
    xEventGroupSetBits(SRP::getSystemStateEventGroup(), SystemEvents::SystemState::BURNER_ON);
    // Record start time for runtime tracking
    BurnerRuntimeTracker::recordStartTime();
}

void BurnerStateMachine::onEnterRunningHigh() {
    LOG_INFO(TAG, "Entering high power operation");
    BurnerSystemController* controller = SRP::getBurnerSystemController();
    if (controller) {
        // L10: Check power level change result
        auto result = controller->setPowerLevel(PowerLevel::FULL);
        if (result.isError()) {
            LOG_ERROR(TAG, "RUNNING_HIGH: Failed to set power level - entering failsafe");

            // Trigger centralized failsafe with DEGRADED level
            CentralizedFailsafe::triggerFailsafe(
                CentralizedFailsafe::FailsafeLevel::DEGRADED,
                SystemError::RELAY_OPERATION_FAILED,
                "Failed to set power level to HIGH"
            );

            // Emergency shutdown to prevent operation at wrong power level
            emergencyStop();
            return;  // onEntry action aborted - state machine will transition to ERROR
        }
    }
    // Set system burner on bit
    xEventGroupSetBits(SRP::getSystemStateEventGroup(), SystemEvents::SystemState::BURNER_ON);
    // Record start time for runtime tracking (if transitioning from non-running state)
    BurnerRuntimeTracker::recordStartTime();
}

void BurnerStateMachine::onEnterPostPurge() {
    LOG_INFO(TAG, "Starting post-purge sequence (duration: %lu ms)", SafetyConfig::postPurgeMs);
    // M6: Record entry time for runtime-configurable duration
    postPurgeEntryTime = millis();
    BurnerSystemController* controller = SRP::getBurnerSystemController();
    if (controller) {
        // BurnerSystemController::deactivate() turns off burner relays only
        // Pumps continue running via PumpControlModule while HEATING_ON/WATER_ON bits are set
        // L10: Check deactivate result - this is the normal shutdown path
        auto result = controller->deactivate();
        if (result.isError()) {
            LOG_ERROR(TAG, "POST_PURGE: Failed to deactivate burner: %s", result.message().c_str());
            // Critical: trigger emergency stop if normal deactivation fails
            emergencyStop();
        }
    }
}

void BurnerStateMachine::onEnterLockout() {
    LOG_ERROR(TAG, "Entering lockout state");
    BurnerSystemController* controller = SRP::getBurnerSystemController();
    if (controller) {
        // L10: Check deactivate result - lockout is already an error state
        auto result = controller->deactivate();
        if (result.isError()) {
            LOG_ERROR(TAG, "LOCKOUT: Failed to deactivate burner: %s", result.message().c_str());
            // Try emergency shutdown as fallback
            controller->emergencyShutdown("Lockout deactivate failed");
        }
    }
    // Set alarm
    RelayControlTask::setRelayState(RelayIndex::toPhysical(RelayIndex::ALARM), true);

    // Log to health monitor
    HealthMonitor* healthMonitor = SRP::getHealthMonitor();
    if (healthMonitor) {
        healthMonitor->recordError(HealthMonitor::Subsystem::CONTROL,
                                   SystemError::IGNITION_FAILURE);
    }
}

void BurnerStateMachine::onEnterError() {
    LOG_ERROR(TAG, "Entering error state");

    // Round 19 Issue #5: Record when we entered ERROR for recovery delay
    errorStateEntryTime = millis();

    BurnerSystemController* controller = SRP::getBurnerSystemController();
    if (controller) {
        controller->emergencyShutdown("BurnerStateMachine error state");
    }
}

void BurnerStateMachine::onExitLockout() {
    LOG_INFO(TAG, "Exiting lockout state - clearing error bit");
    // Clear error bit when exiting lockout (either by timeout or manual reset)
    xEventGroupClearBits(SRP::getSystemStateEventGroup(), SystemEvents::SystemState::BURNER_ERROR);
    ErrorHandler::clearErrorRateLimit(SystemError::SYSTEM_FAILSAFE_TRIGGERED);
    // Clear alarm
    RelayControlTask::setRelayState(RelayIndex::toPhysical(RelayIndex::ALARM), false);
}

void BurnerStateMachine::onExitRunning() {
    LOG_INFO(TAG, "Exiting running state");
    // Clear system burner on bit
    xEventGroupClearBits(SRP::getSystemStateEventGroup(), SystemEvents::SystemState::BURNER_ON);

    // Update runtime counters and persist to storage
    BurnerRuntimeTracker::updateRuntimeCounters();
}

// Helper Functions Implementation
// Round 21: Functions moved to BurnerSafetyChecks, BurnerPowerController, BurnerRuntimeTracker

void BurnerStateMachine::onEnterModeSwitching() {
    LOG_INFO(TAG, "Entering mode switching state (seamless transition)");
}

BurnerSMState BurnerStateMachine::handleModeSwitchingState() {
    LOG_INFO(TAG, ">>> handleModeSwitchingState() CALLED <<<");

    // Safety check first - abort if safety interlocks active
    if (!BurnerSafetyChecks::checkSafetyConditions()) {
        LOG_ERROR(TAG, "Safety interlock during mode switch - aborting to ERROR");
        return BurnerSMState::ERROR;
    }

    // Get new mode from event bits
    // Use BurnerRequest bits for demand (these are set by HeatingControl/WaterControl when they need burner)
    // SystemState bits may not be set yet during seamless transition
    EventBits_t systemBits = xEventGroupGetBits(SRP::getSystemStateEventGroup());
    EventBits_t burnerBits = xEventGroupGetBits(SRP::getBurnerRequestEventGroup());

    // Check burner requests (actual demand)
    bool waterRequested = (burnerBits & SystemEvents::BurnerRequest::WATER) != 0;
    bool heatingRequested = (burnerBits & SystemEvents::BurnerRequest::HEATING) != 0;
    bool waterPriority = (systemBits & SystemEvents::SystemState::WATER_PRIORITY) != 0;

    // Water mode if: water requested AND (heating not requested OR water has priority)
    bool newModeIsWater = waterRequested && (!heatingRequested || waterPriority);

    // Check if new mode has heat demand (using BurnerRequest bits, not SystemState)
    bool newModeHasDemand = newModeIsWater ? waterRequested : heatingRequested;

    LOG_INFO(TAG, "Mode switch handler: newMode=%s, burnerBits=0x%08X, WATER_REQ=%d, HEATING_REQ=%d, demand=%d",
             newModeIsWater ? "WATER" : "HEATING",
             (unsigned int)burnerBits,
             waterRequested,
             heatingRequested,
             newModeHasDemand);

    if (!newModeHasDemand) {
        // No request bit set yet - but check if heating is actually needed
        // HeatingControlTask runs on 5s intervals, may not have set request yet
        // during seamless water→heating transition
        if (!newModeIsWater) {
            // Check if room temperature is below target (heating needed)
            SystemSettings& settings = SRP::getSystemSettings();
            SharedSensorReadings readings = SRP::getSensorReadings();
            if (readings.isInsideTempValid &&
                readings.insideTemp < settings.targetTemperatureInside) {
                // Room is cold - heating IS needed, don't shut down
                char roomBuf[16], targetBuf[16];
                formatTemp(roomBuf, sizeof(roomBuf), readings.insideTemp);
                formatTemp(targetBuf, sizeof(targetBuf), settings.targetTemperatureInside);
                LOG_INFO(TAG, "Heating needed (room %s°C < target %s°C) - waiting for HeatingControlTask",
                        roomBuf, targetBuf);
                // Stay in MODE_SWITCHING, HeatingControlTask will set request soon
                return BurnerSMState::MODE_SWITCHING;
            }
        }

        // No demand in new mode - go to POST_PURGE
        LOG_DEBUG(TAG, "New mode has no heat demand - transitioning to POST_PURGE");
        return BurnerSMState::POST_PURGE;
    }

    // Check if mode changed back to original (race condition)
    if (newModeIsWater == runningModeIsWater) {
        // Mode reverted during switch - return to safe low power
        // Don't use shouldIncreasePower() as it may not be updated yet
        LOG_WARN(TAG, "Mode reverted during switch - resuming at low power");
        return BurnerSMState::RUNNING_LOW;
    }

    // Execute mode switch via BurnerSystemController
    BurnerSystemController* controller = SRP::getBurnerSystemController();
    if (!controller) {
        LOG_ERROR(TAG, "No BurnerSystemController - aborting mode switch");
        return BurnerSMState::POST_PURGE;
    }

    BurnerMode newMode = newModeIsWater ? BurnerMode::WATER : BurnerMode::HEATING;
    auto result = controller->switchMode(newMode, targetTemperature);

    if (result.isError()) {
        LOG_ERROR(TAG, "Mode switch failed: %s - falling back to shutdown",
                 result.message().c_str());
        return BurnerSMState::POST_PURGE;
    }

    // Update mode tracking
    runningModeIsWater = newModeIsWater;

    LOG_INFO(TAG, "Mode switch complete - resuming %s operation",
             newModeIsWater ? "WATER" : "HEATING");

    // Return to appropriate power level based on PID demand
    if (BurnerPowerController::shouldIncreasePower(requestedHighPower)) {
        return BurnerSMState::RUNNING_HIGH;
    } else {
        return BurnerSMState::RUNNING_LOW;
    }
}

void BurnerStateMachine::logStateTransition(BurnerSMState from, BurnerSMState to) {
    // Convert states to strings
    const char* stateNames[] = {
        "IDLE", "PRE_PURGE", "IGNITION",
        "RUNNING_LOW", "RUNNING_HIGH", "MODE_SWITCHING",
        "POST_PURGE", "LOCKOUT", "ERROR"
    };

    const char* fromStr = ((int)from < 9) ? stateNames[(int)from] : "UNKNOWN";
    const char* toStr = ((int)to < 9) ? stateNames[(int)to] : "UNKNOWN";
    
    LOG_INFO(TAG, "State transition: %s -> %s", fromStr, toStr);
    (void)fromStr;  // Suppress unused warning when logging is disabled
    (void)toStr;    // Suppress unused warning when logging is disabled

    // Record power level changes for anti-flapping
    // Skip MODE_SWITCHING - power level doesn't change during mode switch
    // Skip IGNITION - actual power level determined in onEnterIgnition() based on request
    if (to != BurnerSMState::MODE_SWITCHING && from != BurnerSMState::MODE_SWITCHING &&
        to != BurnerSMState::IGNITION) {
        BurnerAntiFlapping::PowerLevel newLevel = BurnerAntiFlapping::stateToPowerLevel(to);
        BurnerAntiFlapping::recordPowerLevelChange(newLevel);
    }
    
    // Log to health monitor
    HealthMonitor* healthMonitor = SRP::getHealthMonitor();
    if (healthMonitor) {
        healthMonitor->recordSuccess(HealthMonitor::Subsystem::CONTROL);
    }
    
    // Log state change event to FRAM
    rtstorage::RuntimeStorage* storage = SRP::getRuntimeStorage();
    if (storage) {
        // Encode from/to states in data field
        uint16_t data = (static_cast<uint8_t>(from) << 8) | static_cast<uint8_t>(to);
        
        // Log critical state changes
        if (to == BurnerSMState::ERROR || to == BurnerSMState::LOCKOUT ||
            to == BurnerSMState::IGNITION || from == BurnerSMState::ERROR) {
            (void)storage->logEvent(rtstorage::EVENT_STATE_CHANGE, data);
        }
    }
}