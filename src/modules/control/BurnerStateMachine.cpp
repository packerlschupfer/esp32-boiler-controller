// src/modules/control/BurnerStateMachine.cpp
#include "BurnerStateMachine.h"
#include "modules/control/SafetyInterlocks.h"  // Add safety interlocks
#include "modules/control/BurnerAntiFlapping.h"  // Add anti-flapping
#include "modules/control/BurnerSystemController.h"  // Unified burner control with batch relays
#include "modules/tasks/RelayControlTask.h"
#include "shared/SharedResources.h"
#include "shared/RelayFunctionDefs.h"
#include "shared/Temperature.h"
#include "monitoring/HealthMonitor.h"
#include "utils/ErrorHandler.h"
#include "utils/MutexRetryHelper.h"
#include "utils/CriticalDataStorage.h"
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
static std::atomic<uint32_t> burnerStartTime{0};       // Track when burner started running
static std::atomic<bool> runningModeIsWater{false};    // Round 19 Issue #1: Track mode for switch detection
static std::atomic<uint32_t> errorStateEntryTime{0};   // Round 19 Issue #5: Track when ERROR state was entered

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
    
    stateMachine.registerState(BurnerSMState::POST_PURGE, {
        .handler = handlePostPurgeState,
        .onEntry = onEnterPostPurge,
        .onExit = nullptr,
        .timeoutMs = POST_PURGE_TIME_MS,
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
    if (heatDemand && checkSafetyConditions()) {
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
    if (!checkSafetyConditions()) {
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
    if (isFlameDetected()) {
        ignitionRetries = 0;

        // Determine which power level to use based on demand
        if (shouldIncreasePower()) {
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
    // Round 19 Issue #1: Check for mode switch (water ↔ heating)
    // Mode switch during RUNNING must go through POST_PURGE for clean pump handoff
    EventBits_t systemBits = xEventGroupGetBits(SRP::getSystemStateEventGroup());
    bool currentModeIsWater = (systemBits & SystemEvents::SystemState::WATER_ON) != 0;
    if (currentModeIsWater != runningModeIsWater) {
        LOG_INFO(TAG, "Mode switch detected during RUNNING_LOW (%s -> %s) - transitioning to POST_PURGE",
                 runningModeIsWater ? "WATER" : "HEATING",
                 currentModeIsWater ? "WATER" : "HEATING");
        return BurnerSMState::POST_PURGE;
    }

    // Check if we should stop
    if (!heatDemand || !checkSafetyConditions()) {
        // Check anti-flapping before turning off
        if (BurnerAntiFlapping::canTurnOff()) {
            return BurnerSMState::POST_PURGE;
        } else {
            LOG_DEBUG(TAG, "Delaying burner stop for %lu ms due to anti-flapping", 
                     BurnerAntiFlapping::getTimeUntilCanTurnOff());
        }
    }
    
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

    // Check if we need more power
    if (shouldIncreasePower()) {
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
    // Round 19 Issue #1: Check for mode switch (water ↔ heating)
    // Mode switch during RUNNING must go through POST_PURGE for clean pump handoff
    EventBits_t systemBits = xEventGroupGetBits(SRP::getSystemStateEventGroup());
    bool currentModeIsWater = (systemBits & SystemEvents::SystemState::WATER_ON) != 0;
    if (currentModeIsWater != runningModeIsWater) {
        LOG_INFO(TAG, "Mode switch detected during RUNNING_HIGH (%s -> %s) - transitioning to POST_PURGE",
                 runningModeIsWater ? "WATER" : "HEATING",
                 currentModeIsWater ? "WATER" : "HEATING");
        return BurnerSMState::POST_PURGE;
    }

    // Check if we should stop
    if (!heatDemand || !checkSafetyConditions()) {
        // Check anti-flapping before turning off
        if (BurnerAntiFlapping::canTurnOff()) {
            return BurnerSMState::POST_PURGE;
        } else {
            LOG_DEBUG(TAG, "Delaying burner stop for %lu ms due to anti-flapping", 
                     BurnerAntiFlapping::getTimeUntilCanTurnOff());
        }
    }
    
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

    // Check if we can reduce power
    if (shouldDecreasePower()) {
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
    // Post-purge is handled by timeout
    // Note: Round 17 Issue X fix is in StateManager - burner should NOT enter POST_PURGE
    // during mode transitions because WATER_PRIORITY_RELEASED triggers immediate heating handoff
    return BurnerSMState::POST_PURGE;
}

BurnerSMState BurnerStateMachine::handleLockoutState() {
    // Lockout can only be reset manually or by timeout
    return BurnerSMState::LOCKOUT;
}

BurnerSMState BurnerStateMachine::handleErrorState() {
    // Round 19 Issue #5: Require 5 minutes in ERROR before auto-recovery
    // This prevents rapid ERROR ↔ IDLE ↔ RUNNING cycling with intermittent faults
    constexpr uint32_t ERROR_RECOVERY_DELAY_MS = 5 * 60 * 1000;  // 5 minutes

    uint32_t timeInError = Utils::elapsedMs(errorStateEntryTime);
    if (timeInError < ERROR_RECOVERY_DELAY_MS) {
        // Still in mandatory hold period - no auto-recovery yet
        // User can still use resetLockout() for manual recovery
        return BurnerSMState::ERROR;
    }

    // After delay, check if safety conditions are restored
    if (checkSafetyConditions()) {
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
        controller->deactivate();
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
    EventBits_t systemBits = xEventGroupGetBits(SRP::getSystemStateEventGroup());
    bool isWaterMode = (systemBits & SystemEvents::SystemState::WATER_ON) != 0;

    // Round 19 Issue #1: Track the mode we're starting in for switch detection
    runningModeIsWater = isWaterMode;

    // Activate burner via BurnerSystemController (atomic batch: pump + burner relays)
    Result<void> activationResult;
    if (controller) {
        if (isWaterMode) {
            LOG_INFO(TAG, "Activating water mode via BurnerSystemController");
            activationResult = controller->activateWaterMode(targetTemperature, PowerLevel::FULL);
        } else {
            LOG_INFO(TAG, "Activating heating mode via BurnerSystemController");
            activationResult = controller->activateHeatingMode(targetTemperature, PowerLevel::FULL);
        }

        if (activationResult.isError()) {
            LOG_ERROR(TAG, "ABORT IGNITION: %s", activationResult.message().c_str());
            xEventGroupSetBits(SRP::getSystemStateEventGroup(), SystemEvents::SystemState::BURNER_ERROR);
            return;
        }
    }

    // Ignition delay - allow flow to establish after pump commanded ON
    // Pump precondition removed: relay verification confirms command succeeded,
    // if pump physically fails, system detects via temperature/flow sensors
    vTaskDelay(pdMS_TO_TICKS(SystemConstants::Timing::BURNER_IGNITION_DELAY_MS));

    // Burner already activated via BurnerSystemController batch command above
}

void BurnerStateMachine::onEnterRunningLow() {
    LOG_INFO(TAG, "Entering low power operation");
    BurnerSystemController* controller = SRP::getBurnerSystemController();
    if (controller) {
        controller->setPowerLevel(PowerLevel::HALF);
    }
    // Clear any error bits
    xEventGroupClearBits(SRP::getSystemStateEventGroup(), SystemEvents::SystemState::BURNER_ERROR);
    // Set system burner on bit
    xEventGroupSetBits(SRP::getSystemStateEventGroup(), SystemEvents::SystemState::BURNER_ON);
    // Record start time for runtime tracking
    burnerStartTime = millis();
}

void BurnerStateMachine::onEnterRunningHigh() {
    LOG_INFO(TAG, "Entering high power operation");
    BurnerSystemController* controller = SRP::getBurnerSystemController();
    if (controller) {
        controller->setPowerLevel(PowerLevel::FULL);
    }
    // Set system burner on bit
    xEventGroupSetBits(SRP::getSystemStateEventGroup(), SystemEvents::SystemState::BURNER_ON);
    // Record start time for runtime tracking (if transitioning from non-running state)
    if (burnerStartTime == 0) {
        burnerStartTime = millis();
    }
}

void BurnerStateMachine::onEnterPostPurge() {
    LOG_INFO(TAG, "Starting post-purge sequence");
    BurnerSystemController* controller = SRP::getBurnerSystemController();
    if (controller) {
        // BurnerSystemController::deactivate() keeps pumps running for heat dissipation
        controller->deactivate();
    }
}

void BurnerStateMachine::onEnterLockout() {
    LOG_ERROR(TAG, "Entering lockout state");
    BurnerSystemController* controller = SRP::getBurnerSystemController();
    if (controller) {
        controller->deactivate();
    }
    // Set alarm
    RelayControlTask::setRelayState(RelayFunction::ALARM, true);

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
    RelayControlTask::setRelayState(RelayFunction::ALARM, false);
}

void BurnerStateMachine::onExitRunning() {
    LOG_INFO(TAG, "Exiting running state");
    // Clear system burner on bit
    xEventGroupClearBits(SRP::getSystemStateEventGroup(), SystemEvents::SystemState::BURNER_ON);

    // Round 15 Issue #1 fix: Calculate and update runtime hours atomically
    // Use Utils::elapsedMs() for safe elapsed time (handles millis() wraparound)
    if (burnerStartTime > 0) {
        uint32_t runTimeMs = Utils::elapsedMs(burnerStartTime);
        float runTimeHours = runTimeMs / 3600000.0f;  // Convert ms to hours

        rtstorage::RuntimeStorage* storage = SRP::getRuntimeStorage();
        if (storage) {
            // Update total runtime
            if (storage->updateRuntimeHours(rtstorage::RUNTIME_TOTAL, runTimeHours)) {
                float totalHours = storage->getRuntimeHours(rtstorage::RUNTIME_TOTAL);
                // Use integer arithmetic for precise HH:MM:SS display
                uint32_t totalSeconds = runTimeMs / 1000;
                int runHours = totalSeconds / 3600;
                int runMinutes = (totalSeconds % 3600) / 60;
                int runSeconds = totalSeconds % 60;
                LOG_INFO(TAG, "Runtime: %d:%02d:%02d (Total: %d.%d hours)",
                        runHours, runMinutes, runSeconds,
                        (int)totalHours, (int)(totalHours * 10) % 10);
            }
            
            // Update burner runtime
            (void)storage->updateRuntimeHours(rtstorage::RUNTIME_BURNER, runTimeHours);
            
            // Also update critical data storage counters
            uint32_t runSecs = (uint32_t)(runTimeHours * 3600);
            CriticalDataStorage::incrementRuntimeCounter(runSecs, true);
            
            // Update heating or water runtime based on current mode
            EventBits_t systemBits = xEventGroupGetBits(SRP::getSystemStateEventGroup());
            if (systemBits & SystemEvents::SystemState::WATER_ON) {
                (void)storage->updateRuntimeHours(rtstorage::RUNTIME_WATER, runTimeHours);
                CriticalDataStorage::incrementCycleCounter(false);  // Water cycle
            } else {
                (void)storage->updateRuntimeHours(rtstorage::RUNTIME_HEATING, runTimeHours);
            }
        }
        
        burnerStartTime = 0;  // Reset for next run
    }
}

// Helper Functions Implementation

bool BurnerStateMachine::isFlameDetected() {
    // WARNING: No flame detection hardware installed
    // System assumes flame is present when burner relay is active

    // Round 14 Issue #2: Use atomic for thread-safe one-time log
    static std::atomic<bool> warningLogged{false};
    if (!warningLogged.exchange(true, std::memory_order_relaxed)) {
        LOG_WARN(TAG, "No flame detection sensor installed - assuming flame when burner active");
    }

    // Without a flame sensor, we assume flame is present when burner is active
    // In a real system, this would check an actual flame sensor
    // TODO: Integrate actual flame sensor when hardware is available

    BurnerSystemController* controller = SRP::getBurnerSystemController();
    return controller ? controller->isActive() : false;
}

bool BurnerStateMachine::checkSafetyConditions() {
    BurnerSystemController* controller = SRP::getBurnerSystemController();
    if (controller) {
        auto result = controller->performSafetyCheck();
        return result.isSuccess();
    }
    return false;  // Fail-safe: no controller = not safe
}

bool BurnerStateMachine::shouldIncreasePower() {
    // SAFETY CHECK: Block high power if temperature already near limit
    // This prevents overshoot when PID requests high power but boiler is already hot
    constexpr Temperature_t HIGH_POWER_LIMIT_TEMP = 800;  // 80.0°C - above this, LOW power only

    auto guard = MutexRetryHelper::acquireGuard(
        SRP::getSensorReadingsMutex(),
        "SensorReadings-PowerCheck"
    );
    if (guard) {
        const SharedSensorReadings& readings = SRP::getSensorReadings();
        if (readings.isBoilerTempOutputValid &&
            readings.boilerTempOutput >= HIGH_POWER_LIMIT_TEMP) {
            // Temperature too high for full power - force LOW regardless of PID request
            if (requestedHighPower) {
                LOG_INFO(TAG, "Blocking high power: boiler temp %d.%d°C >= limit %d.%d°C",
                        readings.boilerTempOutput / 10, abs(readings.boilerTempOutput % 10),
                        HIGH_POWER_LIMIT_TEMP / 10, HIGH_POWER_LIMIT_TEMP % 10);
            }
            return false;
        }
    }

    // Use PID-driven power level decision from HeatingControl/WheaterControl
    // The solenoid gas valve can switch frequently, so we trust PID's calculation
    return requestedHighPower;
}

bool BurnerStateMachine::shouldDecreasePower() {
    // Use PID-driven power level decision - decrease if PID requests LOW power
    // The solenoid gas valve can switch frequently, so we trust PID's calculation
    return !requestedHighPower;
}

void BurnerStateMachine::logStateTransition(BurnerSMState from, BurnerSMState to) {
    // Convert states to strings
    const char* stateNames[] = {
        "IDLE", "PRE_PURGE", "IGNITION",
        "RUNNING_LOW", "RUNNING_HIGH", "POST_PURGE", "LOCKOUT", "ERROR"
    };
    
    const char* fromStr = ((int)from < 8) ? stateNames[(int)from] : "UNKNOWN";
    const char* toStr = ((int)to < 8) ? stateNames[(int)to] : "UNKNOWN";
    
    LOG_INFO(TAG, "State transition: %s -> %s", fromStr, toStr);
    (void)fromStr;  // Suppress unused warning when logging is disabled
    (void)toStr;    // Suppress unused warning when logging is disabled
    
    // Record power level changes for anti-flapping
    BurnerAntiFlapping::PowerLevel newLevel = BurnerAntiFlapping::stateToPowerLevel(to);
    BurnerAntiFlapping::recordPowerLevelChange(newLevel);
    
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