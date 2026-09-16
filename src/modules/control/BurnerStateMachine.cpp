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
#include "modules/control/BurnerRequestManager.h"
#include "modules/control/BurnerTransitions.h"  // Stage B: native-testable transition logic
#include <cmath>
#include <atomic>  // Round 14 Issue #2
#include <RuntimeStorage.h>

// No external declarations needed - using SRP methods

// Static member definitions
StateMachine<BurnerSMState> BurnerStateMachine::stateMachine("BurnerSM", BurnerSMState::IDLE);
const char* BurnerStateMachine::TAG = "BurnerStateMachine";
bool BurnerStateMachine::heatDemand = false;
Temperature_t BurnerStateMachine::targetTemperature = 0;
bool BurnerStateMachine::requestedHighPower = false;
SemaphoreHandle_t BurnerStateMachine::demandMutex = nullptr;

// BurnerDemandGate permission: written by BurnerControlTask, read lock-free by
// BoilerTempControlTask and checked by setHeatDemand(true) under demandMutex
static std::atomic<bool> demandPermitted{false};
static std::atomic<bool> demandHighPowerAllowed{true};
static std::atomic<int16_t> demandMaxTargetTemp{0};

// ============================================================================
// THREAD-SAFETY NOTE (Round 14 Issue #1, Round 20 Issue #8):
// These variables are only accessed by BurnerStateMachine state callbacks which
// are all called from BurnerControlTask (single task context via update()).
// Made std::atomic as a defensive measure and to be explicit about thread-safety.
// DO NOT access these variables from other tasks.
// ============================================================================
// Round 21: burnerStartTime moved to BurnerRuntimeTracker
static std::atomic<uint32_t> errorStateEntryTime{0};   // Round 19 Issue #5: Track when ERROR state was entered
static std::atomic<uint32_t> postPurgeEntryTime{0};    // M6: Track when POST_PURGE state was entered for runtime-configurable duration

// Memory of BurnerTransitions::step(): ignition retries, running mode (Round 19
// Issue #1: mode for switch detection) and the no-mode-request timer. Same
// single-task access as above.
// Round 15 Issue #2 note: ignitionRetries intentionally NOT persisted to FRAM
// Rationale: Power cycle should reset retry count because:
// 1. User may have fixed the underlying issue (gas supply, sensor, etc.)
// 2. Starting fresh after power cycle is safer than inheriting old failure state
// 3. Repeated power cycles during ignition failures indicate electrical issues
static BurnerTransitions::Memory transitionMemory = {0, false, 0, false};

static const BurnerTransitions::Timing TRANSITION_TIMING = {
    SystemConstants::Timing::BURNER_MIN_IGNITION_TIME_MS,
    SystemConstants::Burner::IGNITION_TIME_MS,
    SystemConstants::Burner::MAX_IGNITION_RETRIES,
    BurnerTransitions::MODE_DEMAND_LOSS_GRACE_MS,
    SystemConstants::Burner::PRE_PURGE_TIME_MS
};

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
        // Backstop only: the handler goes to IGNITION after PRE_PURGE_TIME_MS once demand
        // and mode request are re-checked (as the timeout it skipped those checks)
        .timeoutMs = PRE_PURGE_TIME_MS + BurnerTransitionPolicy::PRE_PURGE_BACKSTOP_MARGIN_MS,
        .timeoutNextState = BurnerSMState::IGNITION
    });
    
    stateMachine.registerState(BurnerSMState::IGNITION, {
        .handler = handleIgnitionState,
        .onEntry = onEnterIgnition,
        .onExit = nullptr,
        // Backstop only: the handler retries (PRE_PURGE) or locks out at IGNITION_TIME_MS
        .timeoutMs = IGNITION_TIME_MS + BurnerTransitionPolicy::IGNITION_BACKSTOP_MARGIN_MS,
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
        // Hard limit behind the bounded waits of BurnerTransitionPolicy; the handler
        // still runs every update, a normal switch completes within milliseconds
        .timeoutMs = BurnerTransitionPolicy::MODE_SWITCH_HARD_TIMEOUT_MS,
        .timeoutNextState = BurnerSMState::POST_PURGE
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

bool BurnerStateMachine::setHeatDemand(bool demand, Temperature_t target, bool highPower) {
    if (demandMutex == nullptr) {
        LOG_ERROR(TAG, "setHeatDemand: demandMutex is null");
        return false;
    }

    auto guard = MutexRetryHelper::acquireGuard(demandMutex, "BurnerSM-SetDemand");
    if (!guard) {
        LOG_ERROR(TAG, "setHeatDemand: Failed to acquire demand mutex");
        return false;
    }

    // BurnerDemandGate: the permission check and the arming are one step under
    // demandMutex. BoilerTempControlTask armed from a permission snapshot taken at
    // the start of its cycle, so a revoke in between was re-armed for one cycle
    // (review 2026-09-14 bsm-6). A revoke is stored before BurnerControlTask's
    // setHeatDemand(false): an arm is either refused here or overwritten by that OFF.
    // Check and write are BurnerDemandGate::applyDemandWrite(), which the native tests
    // interleave with revokes; the permission must be read here, under demandMutex.
    BurnerDemandGate::DemandSlot slot = {heatDemand, targetTemperature, requestedHighPower};
    const BurnerDemandGate::WriteResult result =
        BurnerDemandGate::applyDemandWrite(slot, demand, target, highPower, demandPermitted.load());
    if (result == BurnerDemandGate::WriteResult::REFUSED) {
        LOG_INFO(TAG, "Heat demand ON refused - burner demand not permitted");
        return false;
    }

    // Only update and log if something actually changed (target: > 0.1°C difference)
    if (result == BurnerDemandGate::WriteResult::UPDATED) {
        heatDemand = slot.demand;
        requestedHighPower = slot.highPower;
        targetTemperature = slot.target;
        char tempBuf[16];
        formatTemp(tempBuf, sizeof(tempBuf), targetTemperature);
        LOG_INFO(TAG, "Heat demand: %s, target: %s°C, power: %s",
                 demand ? "ON" : "OFF", tempBuf, highPower ? "HIGH" : "LOW");
    }
    return true;
}

void BurnerStateMachine::setDemandPermission(const BurnerDemandGate::Permission& permission) {
    // Revoke before and grant after the limits change, so a reader never sees a
    // grant with stale limits
    if (!permission.permitted) {
        demandPermitted.store(false);
    }
    demandMaxTargetTemp.store(permission.maxTargetTemp);
    demandHighPowerAllowed.store(permission.highPowerAllowed);
    if (permission.permitted) {
        demandPermitted.store(true);
    }
}

BurnerDemandGate::Permission BurnerStateMachine::getDemandPermission() {
    BurnerDemandGate::Permission permission;
    permission.permitted = demandPermitted.load();
    permission.highPowerAllowed = demandHighPowerAllowed.load();
    permission.maxTargetTemp = demandMaxTargetTemp.load();
    return permission;
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

bool BurnerStateMachine::getHeatDemandState(bool& outDemand, Temperature_t& outTarget, bool& outHighPower) {
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
    outHighPower = requestedHighPower;
    return true;
}

void BurnerStateMachine::resetLockout() {
    if (stateMachine.isInState(BurnerSMState::LOCKOUT)) {
        LOG_INFO(TAG, "Resetting lockout state");
        transitionMemory.ignitionRetries = 0;
        // Clear error bit when resetting lockout
        xEventGroupClearBits(SRP::getSystemStateEventGroup(), SystemEvents::SystemState::BURNER_ERROR);
        ErrorHandler::clearErrorRateLimit(SystemError::SYSTEM_FAILSAFE_TRIGGERED);
        stateMachine.transitionTo(BurnerSMState::IDLE);
    }
}

// State Handlers Implementation

namespace {

// Firmware inputs for BurnerTransitions::step(). Event bits are read once per
// tick; safety check, flame, power limit, anti-flapping and settings are probed
// only when the transition logic asks for them (performSafetyCheck() may
// emergency-shutdown, several probes take mutexes).
class FirmwareTransitionEnvironment : public BurnerTransitions::Environment {
public:
    FirmwareTransitionEnvironment(const char* tag, Temperature_t targetTemp)
        : tag_(tag),
          targetTemp_(targetTemp),
          systemBits_(xEventGroupGetBits(SRP::getSystemStateEventGroup())),
          requestBits_(BurnerRequestManager::getCurrentRequests()) {}

    EventBits_t systemBits() const { return systemBits_; }
    EventBits_t requestBits() const { return requestBits_; }

    bool safetyOk() override { return BurnerSafetyChecks::checkSafetyConditions(); }
    bool flameDetected() override { return BurnerSafetyChecks::isFlameDetected(); }

    bool boilerEnabled() override { return hasSystemBit(SystemEvents::SystemState::BOILER_ENABLED); }
    bool heatingEnabled() override { return hasSystemBit(SystemEvents::SystemState::HEATING_ENABLED); }
    bool waterEnabled() override { return hasSystemBit(SystemEvents::SystemState::WATER_ENABLED); }
    bool heatingOn() override { return hasSystemBit(SystemEvents::SystemState::HEATING_ON); }
    bool waterOn() override { return hasSystemBit(SystemEvents::SystemState::WATER_ON); }
    bool waterPriority() override { return hasSystemBit(SystemEvents::SystemState::WATER_PRIORITY); }
    bool heatingRequested() override { return (requestBits_ & SystemEvents::BurnerRequest::HEATING) != 0; }
    bool waterRequested() override { return (requestBits_ & SystemEvents::BurnerRequest::WATER) != 0; }

    bool relaysInWaterMode() override {
        BurnerSystemController* controller = SRP::getBurnerSystemController();
        return controller && controller->getCurrentMode() == BurnerMode::WATER;
    }

    bool highPowerAllowed(bool requestedHighPower) override {
        return BurnerPowerController::shouldIncreasePower(requestedHighPower);
    }

    bool heatingLikelyWanted() override {
        // F33: snapshot settings and sensor readings UNDER their mutexes
        bool useWeather = false;
        bool overrideOff = true;
        Temperature_t targetInside = 0;
        Temperature_t outsideThreshold = 0;
        Temperature_t overheatMargin = 0;
        Temperature_t hysteresis = BurnerTransitionPolicy::DEFAULT_HEATING_HYSTERESIS;
        if (SRP::takeSystemSettingsMutex(pdMS_TO_TICKS(50)) != pdTRUE) {
            return false;
        }
        {
            const SystemSettings& s = SRP::getSystemSettings();
            useWeather = s.useWeatherCompensatedControl;
            overrideOff = s.heatingOverrideOff;
            targetInside = s.targetTemperatureInside;
            outsideThreshold = s.outsideTempHeatingThreshold;
            overheatMargin = s.roomTempOverheatMargin;
            hysteresis = s.heating_hysteresis;
            SRP::giveSystemSettingsMutex();
        }
        SharedSensorReadings readings{};
        if (SRP::takeSensorReadingsMutex(pdMS_TO_TICKS(50)) != pdTRUE) {
            return false;
        }
        readings = SRP::getSensorReadings();
        SRP::giveSensorReadingsMutex();
        return BurnerTransitionPolicy::heatingLikelyWanted(
            heatingEnabled(), overrideOff, useWeather,
            readings.isOutsideTempValid, readings.outsideTemp, outsideThreshold,
            readings.isInsideTempValid, readings.insideTemp, targetInside, overheatMargin,
            hysteresis);
    }

    bool canTurnOn() override { return BurnerAntiFlapping::canTurnOn(); }
    bool canTurnOff() override { return BurnerAntiFlapping::canTurnOff(); }
    bool canChangePower(bool toHigh) override {
        return BurnerAntiFlapping::canChangePowerLevel(toHigh ? BurnerAntiFlapping::PowerLevel::POWER_HIGH
                                                              : BurnerAntiFlapping::PowerLevel::POWER_LOW);
    }

    bool switchMode(bool toWater) override {
        BurnerSystemController* controller = SRP::getBurnerSystemController();
        if (!controller) {
            LOG_ERROR(tag_, "No BurnerSystemController - aborting mode switch");
            return false;
        }
        auto result = controller->switchMode(toWater ? BurnerMode::WATER : BurnerMode::HEATING, targetTemp_);
        if (result.isError()) {
            LOG_ERROR(tag_, "Mode switch failed: %s - falling back to shutdown", result.message().c_str());
            return false;
        }
        return true;
    }

private:
    bool hasSystemBit(EventBits_t bit) const { return (systemBits_ & bit) != 0; }

    const char* tag_;
    Temperature_t targetTemp_;
    EventBits_t systemBits_;
    EventBits_t requestBits_;
};

} // namespace

// IDLE, PRE_PURGE, IGNITION, RUNNING_LOW/HIGH and MODE_SWITCHING decide their
// next state in BurnerTransitions::step() (Stage B); see BurnerTransitions.h.
BurnerSMState BurnerStateMachine::handleIdleState() {
    return runTransitionStep();
}

BurnerSMState BurnerStateMachine::handlePrePurgeState() {
    return runTransitionStep();
}

BurnerSMState BurnerStateMachine::handleIgnitionState() {
    return runTransitionStep();
}

BurnerSMState BurnerStateMachine::handleRunningLowState() {
    return runTransitionStep();
}

BurnerSMState BurnerStateMachine::handleRunningHighState() {
    return runTransitionStep();
}

BurnerSMState BurnerStateMachine::runTransitionStep() {
    const BurnerTransitions::Context ctx = {
        stateMachine.getCurrentState(),
        stateMachine.getTimeInState(),
        millis(),
        heatDemand,
        requestedHighPower
    };
    FirmwareTransitionEnvironment env(TAG, targetTemperature);
    const BurnerTransitions::Decision decision =
        BurnerTransitions::step(ctx, TRANSITION_TIMING, env, transitionMemory);
    logTransitionDecision(ctx, decision, env.systemBits(), env.requestBits());
    return decision.next;
}

void BurnerStateMachine::logTransitionDecision(const BurnerTransitions::Context& ctx,
                                               const BurnerTransitions::Decision& d,
                                               uint32_t systemBits, uint32_t requestBits) {
    using BurnerTransitions::Reason;
    const char* stateName = (ctx.state == BurnerSMState::RUNNING_HIGH) ? "RUNNING_HIGH" : "RUNNING_LOW";
    const char* fromMode = d.fromWater ? "WATER" : "HEATING";
    const char* toMode = d.toWater ? "WATER" : "HEATING";
    (void)stateName;
    (void)fromMode;
    (void)toMode;
    (void)systemBits;
    (void)requestBits;

    if (d.bothModesOn) {
        LOG_WARN(TAG, "Both WATER_ON and HEATING_ON set - using priority=%d as tiebreaker",
                 (systemBits & SystemEvents::SystemState::WATER_PRIORITY) != 0);
    }
    if (ctx.state == BurnerSMState::MODE_SWITCHING && d.reason != Reason::SAFETY_FAILED) {
        LOG_INFO(TAG, "Mode switch handler: newMode=%s, burnerBits=0x%08X, WATER_REQ=%d, HEATING_REQ=%d, demand=%d",
                 toMode, (unsigned int)requestBits,
                 (requestBits & SystemEvents::BurnerRequest::WATER) != 0,
                 (requestBits & SystemEvents::BurnerRequest::HEATING) != 0,
                 d.newModeHasDemand);
    }
    if (d.stopDelayed) {
        LOG_DEBUG(TAG, "Delaying burner stop for %lu ms due to anti-flapping",
                  BurnerAntiFlapping::getTimeUntilCanTurnOff());
    }

    switch (d.reason) {
        case Reason::STALE_DEMAND: {
            static uint32_t lastStaleLogMs = 0;
            if (lastStaleLogMs == 0 || ctx.nowMs - lastStaleLogMs > 60000) {
                LOG_WARN(TAG, "Ignoring stale heat demand - no active heating/water mode request");
                lastStaleLogMs = ctx.nowMs;
            }
            break;
        }
        case Reason::START_DELAYED:
            LOG_DEBUG(TAG, "Delaying burner start for %lu ms due to anti-flapping",
                      BurnerAntiFlapping::getTimeUntilCanTurnOn());
            break;
        case Reason::SAFETY_FAILED:
            if (ctx.state == BurnerSMState::MODE_SWITCHING) {
                LOG_ERROR(TAG, "Safety interlock during mode switch - aborting to ERROR");
            }
            break;
        case Reason::MODE_WITHDRAWN:
            LOG_INFO(TAG, "Heating/water request withdrawn during pre-purge - aborting start");
            break;
        case Reason::DEMAND_WITHDRAWN:
            LOG_INFO(TAG, "Heat demand withdrawn during pre-purge - aborting start");
            break;
        case Reason::IGNITION_OK:
            LOG_INFO(TAG, "Ignition successful after %lu ms - transitioning to %s power", ctx.timeInStateMs,
                     d.next == BurnerSMState::RUNNING_HIGH ? "high" : "low");
            break;
        case Reason::IGNITION_RETRY:
            LOG_WARN(TAG, "Ignition retry %d/%d", transitionMemory.ignitionRetries, MAX_IGNITION_RETRIES);
            break;
        case Reason::IGNITION_LOCKOUT:
            LOG_ERROR(TAG, "Max ignition retries exceeded");
            break;
        case Reason::SWITCH_SEAMLESS:
            LOG_INFO(TAG, "Seamless mode switch detected during %s (%s -> %s)", stateName, fromMode, toMode);
            break;
        case Reason::SWITCH_NEEDS_STOP:
            LOG_INFO(TAG, "Mode switch detected during %s (%s -> %s) - transitioning to POST_PURGE",
                     stateName, fromMode, toMode);
            break;
        case Reason::EXPLICIT_DISABLE:
            LOG_INFO(TAG, "%s disabled - stopping burner now (minimum on-time bypassed)",
                     d.boilerDisabled ? "Boiler" : (d.fromWater ? "Water heating" : "Space heating"));
            break;
        case Reason::NO_MODE_DEMAND:
            LOG_WARN(TAG, "Burner running without active heating/water mode request for %lu ms - stopping",
                     d.elapsedMs);
            break;
        case Reason::FLAME_LOST:
            if (ctx.heatDemand) {
                LOG_WARN(TAG, "UNEXPECTED: Flame/burner off while demand still active");
            } else {
                LOG_DEBUG(TAG, "Burner off (intentional - demand ended)");
            }
            break;
        case Reason::POWER_UP_DELAYED:
            LOG_DEBUG(TAG, "Delaying power increase for %lu ms due to anti-flapping",
                      BurnerAntiFlapping::getTimeUntilCanChangePower());
            break;
        case Reason::POWER_DOWN_DELAYED:
            LOG_DEBUG(TAG, "Delaying power decrease for %lu ms due to anti-flapping",
                      BurnerAntiFlapping::getTimeUntilCanChangePower());
            break;
        case Reason::SWITCH_WAIT_FOR_HEATING:
            LOG_DEBUG(TAG, "Waiting for heating request (handover, %lu ms)", ctx.timeInStateMs);
            break;
        case Reason::SWITCH_NO_DEMAND:
            LOG_INFO(TAG, "No request for new mode %s after %lu ms (heating wanted: %s) - stopping",
                     toMode, ctx.timeInStateMs, d.heatingWanted ? "yes" : "no");
            break;
        case Reason::SWITCH_RESUME:
            LOG_WARN(TAG, "Mode reverted during switch - resuming at low power");
            break;
        case Reason::SWITCH_REVERT_WAIT:
            LOG_DEBUG(TAG, "Mode reverted but %s not set - waiting (%lu ms)",
                      d.fromWater ? "WATER_ON" : "HEATING_ON", ctx.timeInStateMs);
            break;
        case Reason::SWITCH_REVERT_STOP:
            LOG_INFO(TAG, "Mode reverted but %s still not set after %lu ms - stopping",
                     d.fromWater ? "WATER_ON" : "HEATING_ON", ctx.timeInStateMs);
            break;
        case Reason::SWITCH_DONE:
            LOG_INFO(TAG, "Mode switch complete - resuming %s operation", toMode);
            break;
        case Reason::RESTART_FROM_POST_PURGE:
            LOG_INFO(TAG, "Heat demand returned during post-purge - restarting after %lu ms", ctx.timeInStateMs);
            break;
        case Reason::SWITCH_FAILED:  // logged by FirmwareTransitionEnvironment::switchMode()
        default:
            break;
    }
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

    // Heat demand returned: restart without waiting out the post-purge
    // (BurnerTransitions::step - same conditions as from IDLE, minimum off-time applies)
    if (runTransitionStep() == BurnerSMState::PRE_PURGE) {
        postPurgeEntryTime = 0;
        return BurnerSMState::PRE_PURGE;
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
        // No manual exit: resetLockout() only acts in LOCKOUT
        return BurnerSMState::ERROR;
    }

    // While EMERGENCY_STOP is latched the safety check fails anyway, and
    // performSafetyCheck() re-ran emergencyShutdown() and logged errors on every tick
    // (syslog flood, one HA notification per error line). Wait for the release.
    if (xEventGroupGetBits(SRP::getSystemStateEventGroup()) & SystemEvents::SystemState::EMERGENCY_STOP) {
        constexpr uint32_t LATCH_LOG_INTERVAL_MS = 300000;  // 5 min
        static uint32_t lastLatchLog = 0;
        if (lastLatchLog == 0 || now - lastLatchLog >= LATCH_LOG_INTERVAL_MS) {
            lastLatchLog = now;
            LOG_WARN(TAG, "Emergency stop latched - burner stays in ERROR until released (boiler/cmd/emergency_reset)");
        }
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
    transitionMemory.runningModeIsWater = isWaterMode;

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
            handlePowerLevelFault(false);
            return;  // onEntry action aborted - state machine already left RUNNING_LOW
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
            handlePowerLevelFault(true);
            return;  // onEntry action aborted - state machine already left RUNNING_HIGH
        }
    }
    // Set system burner on bit
    xEventGroupSetBits(SRP::getSystemStateEventGroup(), SystemEvents::SystemState::BURNER_ON);
    // Record start time for runtime tracking (if transitioning from non-running state)
    BurnerRuntimeTracker::recordStartTime();
}

// A refused power level change (relay rate limit, queue full, Modbus error) used to
// emergency-stop the burner: ERROR for the recovery delay, pumps switched off
// (2026-09-14 15:41:25). Stop gracefully instead - post-purge deactivates the burner
// relays and escalates itself if that fails, the pumps keep following their modes -
// and escalate only when the fault keeps repeating.
void BurnerStateMachine::handlePowerLevelFault(bool high) {
    static uint8_t faultCount = 0;
    static uint32_t faultWindowStartMs = 0;
    const bool escalate = BurnerTransitionPolicy::recordPowerFault(faultCount, faultWindowStartMs, millis());

    CentralizedFailsafe::triggerFailsafe(
        CentralizedFailsafe::FailsafeLevel::DEGRADED,
        SystemError::RELAY_OPERATION_FAILED,
        high ? "Failed to set power level to HIGH" : "Failed to set power level to LOW"
    );

    if (escalate) {
        LOG_ERROR(TAG, "RUNNING_%s: power level relay failed %u times within %lu s - emergency stop",
                  high ? "HIGH" : "LOW", static_cast<unsigned>(faultCount),
                  static_cast<unsigned long>(BurnerTransitionPolicy::POWER_FAULT_WINDOW_MS / 1000));
        faultCount = 0;
        emergencyStop();
        return;
    }

    LOG_ERROR(TAG, "RUNNING_%s: failed to set power level (%u/%u) - stopping burner via post-purge",
              high ? "HIGH" : "LOW", static_cast<unsigned>(faultCount),
              static_cast<unsigned>(BurnerTransitionPolicy::POWER_FAULT_MAX_COUNT));
    stateMachine.transitionTo(BurnerSMState::POST_PURGE);
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
    // The next start gets all ignition attempts again
    transitionMemory.ignitionRetries = 0;
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
    // Water <-> heating handover (bounded wait, revert handling, relay switch) is
    // decided in BurnerTransitions::step()
    return runTransitionStep();
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

    // Record power level changes for anti-flapping (entering MODE_SWITCHING keeps the
    // level, IGNITION records its start level in onEnterIgnition()). Transitions out of
    // MODE_SWITCHING count: a stop from there left anti-flapping "on", so the restart
    // from POST_PURGE ignored the minimum off-time (review 2026-09-14).
    if (BurnerTransitionPolicy::recordsPowerLevelOnTransition(to)) {
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