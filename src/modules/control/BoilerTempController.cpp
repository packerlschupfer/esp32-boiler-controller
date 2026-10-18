// src/modules/control/BoilerTempController.cpp
#include "modules/control/BoilerTempController.h"
#include "modules/control/AutotuneRelayConfig.h"  // autotune amplitude/hysteresis settings
#include "modules/control/BurnerAntiFlapping.h"
#include "modules/control/BurnerRequestManager.h"
#include "modules/control/PIDGainFixedPoint.h"
#include "config/SystemConstants.h"
#include "config/SafetyConfig.h"
#include "config/SystemSettingsStruct.h"
#include "core/SystemResourceProvider.h"
#include "events/SystemEventsGenerated.h"
#include "LoggingMacros.h"
#include <MutexGuard.h>
#include <Arduino.h>

static_assert(PIDGainFixedPoint::SCALE == SystemConstants::PID::PID_FIXED_POINT_SCALE,
              "PIDGainFixedPoint::SCALE must match the fixed-point PID scale");

const char* BoilerTempController::TAG = "BoilerTempCtrl";

// Autotune method index (pid/autotune/method): 0=ZN_PI, 1=ZN_PID, 2=Tyreus-Luyben,
// 3=Cohen-Coon, 4=Lambda
static bool tuningMethodFromIndex(int32_t index, PIDAutoTuner::TuningMethod& method) {
    static const PIDAutoTuner::TuningMethod kMethods[] = {
        PIDAutoTuner::TuningMethod::ZIEGLER_NICHOLS_PI,
        PIDAutoTuner::TuningMethod::ZIEGLER_NICHOLS_PID,
        PIDAutoTuner::TuningMethod::TYREUS_LUYBEN,
        PIDAutoTuner::TuningMethod::COHEN_COON,
        PIDAutoTuner::TuningMethod::LAMBDA_TUNING
    };
    if (index < 0 || index > 4) {
        return false;
    }
    method = kMethods[index];
    return true;
}

bool BoilerTempController::initialize() {
    if (initialized_) {
        LOG_WARN(TAG, "Already initialized");
        return true;
    }

    // Create mutex for thread-safe access
    if (mutex_ == nullptr) {
        mutex_ = xSemaphoreCreateMutex();
        if (mutex_ == nullptr) {
            LOG_ERROR(TAG, "Failed to create mutex");
            return false;
        }
    }

    // Create PID controller for modulating mode
    if (pidController_ == nullptr) {
        pidController_ = new PIDControlModuleFixedPoint();
        if (pidController_ == nullptr) {
            LOG_ERROR(TAG, "Failed to create PID controller");
            return false;
        }
        // Set integral limits to prevent windup
        pidController_->setIntegralLimits(SafetyConfig::pidIntegralMin,
                                          SafetyConfig::pidIntegralMax);
        // Output limits = where the power mapping saturates, so anti-windup stops
        // integrating once power is at 0 or 100 %
        pidController_->setOutputLimits(-PIDGainFixedPoint::POWER_ADJUSTMENT_LIMIT,
                                        PIDGainFixedPoint::POWER_ADJUSTMENT_LIMIT);
        LOG_INFO(TAG, "PID controller created with integral limits [%ld, %ld]",
                 (long)SafetyConfig::pidIntegralMin, (long)SafetyConfig::pidIntegralMax);
    }

    // Create auto-tuner
    if (autoTuner_ == nullptr) {
        autoTuner_ = new PIDAutoTuner();
        if (autoTuner_ == nullptr) {
            LOG_ERROR(TAG, "Failed to create auto-tuner");
            return false;
        }
        LOG_INFO(TAG, "Auto-tuner created");
    }

    // Initialize last output to safe state
    lastOutput_.burnerOn = false;
    lastOutput_.powerLevel = PowerLevel::OFF;
    lastOutput_.modulationPercent = 0;
    lastOutput_.changed = false;

    lastPIDOutput_ = 0;
    lastCycleTime_ = millis();

    // Load configuration from SystemSettings
    SystemSettings& settings = SRP::getSystemSettings();

    // Set burner type based on useBoilerTempPID flag
    config_.burnerType = settings.useBoilerTempPID ?
                        BurnerType::MODULATING :
                        BurnerType::TWO_STAGE;

    // Load PID gains for boiler temperature control
    // Start with space heating gains (will switch to water gains when in water mode)
    config_.modKp = settings.spaceHeatingKp;
    config_.modKi = settings.spaceHeatingKi;
    config_.modKd = settings.spaceHeatingKd;

    // Store water heating gains for mode switching
    config_.waterKp = settings.wHeaterKp;
    config_.waterKi = settings.wHeaterKi;
    config_.waterKd = settings.wHeaterKd;

    // Autotune method from settings. This runs before PersistentStorageTask has loaded
    // NVS, so startAutoTuning() reads the setting again at every start.
    PIDAutoTuner::TuningMethod method;
    if (tuningMethodFromIndex(settings.autotuneMethod, method)) {
        tuningMethod_ = method;
    }
    LOG_INFO(TAG, "Autotune method from settings: %ld", static_cast<long>(settings.autotuneMethod));

    initialized_ = true;

    LOG_INFO(TAG, "Initialized - Mode:%s",
             config_.burnerType == BurnerType::TWO_STAGE ? "BANG_BANG" : "PID_MODULATING");
    LOG_INFO(TAG, "Space heating PID: Kp:%.2f Ki:%.4f Kd:%.2f",
             settings.spaceHeatingKp, settings.spaceHeatingKi, settings.spaceHeatingKd);
    LOG_INFO(TAG, "Water heating PID: Kp:%.2f Ki:%.4f Kd:%.2f",
             config_.waterKp, config_.waterKi, config_.waterKd);
    LOG_INFO(TAG, "Thresholds: OFF<%u HALF>%u FULL>%u (hyst:%u)",
             config_.offThreshold, config_.halfThreshold,
             config_.fullThreshold, config_.thresholdHysteresis);

    return true;
}

void BoilerTempController::setConfig(const Config& config) {
    MutexGuard guard(mutex_, MUTEX_TIMEOUT);
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "setConfig: mutex timeout");
        return;
    }

    config_ = config;

    LOG_INFO(TAG, "Config updated - OffHyst:%.1f OnHyst:%.1f FullThresh:%.1f",
             tempToFloat(config_.offHysteresis),
             tempToFloat(config_.onHysteresis),
             tempToFloat(config_.fullPowerThreshold));
}

BoilerTempController::Config BoilerTempController::getConfig() const {
    MutexGuard guard(mutex_, MUTEX_TIMEOUT);
    if (!guard.hasLock()) {
        LOG_WARN(TAG, "getConfig: mutex timeout - returning cached config");
    }
    return config_;
}

BoilerTempController::ControlOutput BoilerTempController::calculate(
    Temperature_t targetTemp, Temperature_t currentTemp) {

    if (!initialized_) {
        LOG_ERROR(TAG, "Not initialized");
        return {false, PowerLevel::OFF, 0, false};
    }

    // Safety check: invalid target
    if (!isValidTarget(targetTemp)) {
        LOG_WARN(TAG, "Invalid target temp: %.1f - returning OFF",
                 tempToFloat(targetTemp));
        ControlOutput safeOutput = {false, PowerLevel::OFF, 0, lastOutput_.burnerOn};
        lastOutput_ = safeOutput;
        lastOutput_.changed = false;
        return safeOutput;
    }

    // Safety check: invalid current reading
    if (currentTemp == TEMP_INVALID) {
        LOG_WARN(TAG, "Invalid current temp - returning OFF for safety");
        ControlOutput safeOutput = {false, PowerLevel::OFF, 0, lastOutput_.burnerOn};
        lastOutput_ = safeOutput;
        lastOutput_.changed = false;
        return safeOutput;
    }

    ControlOutput output;

    switch (config_.burnerType) {
        case BurnerType::TWO_STAGE:
            output = calculateBangBang(targetTemp, currentTemp);
            break;

        case BurnerType::MODULATING:
            output = calculateModulating(targetTemp, currentTemp);
            break;

        default:
            LOG_ERROR(TAG, "Unknown burner type");
            output = {false, PowerLevel::OFF, 0, false};
            break;
    }

    // Check if output changed
    output.changed = (output.burnerOn != lastOutput_.burnerOn) ||
                     (output.powerLevel != lastOutput_.powerLevel);

    if (output.changed) {
        LOG_INFO(TAG, "Output changed: %s/%s -> %s/%s (target:%.1f current:%.1f)",
                 lastOutput_.burnerOn ? "ON" : "OFF",
                 powerLevelToString(lastOutput_.powerLevel),
                 output.burnerOn ? "ON" : "OFF",
                 powerLevelToString(output.powerLevel),
                 tempToFloat(targetTemp),
                 tempToFloat(currentTemp));
    }

    lastOutput_ = output;
    return output;
}

BoilerTempController::ControlOutput BoilerTempController::calculateBangBang(
    Temperature_t target, Temperature_t current) {

    // Same pause rule as the modulating path (BoilerPowerLevel::bangBangCycleStart):
    // this controller only runs while a request is active and not during autotune, so
    // after a pause the last level belongs to a request that ended minutes or hours ago.
    // A stale HALF/FULL would hold the burner on until the boiler is offHysteresis
    // (5.0 °C) above the new target. Last cycle time read before millis(), like
    // calculateModulating(), so the difference cannot wrap.
    const uint32_t lastCycleMs = lastCycleTime_;
    const uint32_t now = millis();
    lastCycleTime_ = now;

    if (BoilerPowerLevel::pidPaused(now, lastCycleMs)) {
        const PowerLevel startLevel =
            BoilerPowerLevel::bangBangCycleStart(lastOutput_.powerLevel, true);
        if (startLevel != lastOutput_.powerLevel) {
            LOG_INFO(TAG, "Bang-bang resumed after %lu ms pause - level %s -> %s",
                     static_cast<unsigned long>(now - lastCycleMs),
                     powerLevelToString(lastOutput_.powerLevel),
                     powerLevelToString(startLevel));
        }
        // calculate() compares against lastOutput_ for `changed`; the burner is not
        // driven by this controller during a pause
        lastOutput_.powerLevel = startLevel;
        lastOutput_.burnerOn = (startLevel != PowerLevel::OFF);
    }

    ControlOutput output = lastOutput_;
    output.modulationPercent = 0;  // Not used for two-stage

    // Calculate error (positive = need more heat)
    Temperature_t error = tempSub(target, current);

    // Bands, e.g. off +50 (+5.0°C), on +30 (+3.0°C), full +100 (+10.0°C)
    const BoilerPowerLevel::BangBangBands bands = {
        config_.offHysteresis, config_.onHysteresis, config_.fullPowerThreshold
    };

    // Three-point bang-bang control with hysteresis (BoilerPowerLevel::fromBangBangError)
    //
    // State transitions (error = target - current):
    //   error > fullThreshold      → FULL power (very cold, need max heat)
    //   error > onThreshold        → HALF power (moderately cold)
    //   error < -offThreshold      → OFF (above target + hysteresis)
    //
    // Hysteresis prevents rapid switching at boundaries:
    //   - Stay in current state unless error crosses a threshold
    //   - Different thresholds for turning ON vs OFF
    PowerLevel desiredLevel = BoilerPowerLevel::fromBangBangError(lastOutput_.powerLevel, error, bands);

    // Check anti-flapping before changing power level
    if (desiredLevel != lastOutput_.powerLevel) {
        BurnerAntiFlapping::PowerLevel afLevel;
        switch (desiredLevel) {
            case PowerLevel::OFF:
                afLevel = BurnerAntiFlapping::PowerLevel::OFF;
                break;
            case PowerLevel::HALF:
                afLevel = BurnerAntiFlapping::PowerLevel::POWER_LOW;
                break;
            case PowerLevel::FULL:
                afLevel = BurnerAntiFlapping::PowerLevel::POWER_HIGH;
                break;
            default:
                afLevel = BurnerAntiFlapping::PowerLevel::OFF;
                break;
        }

        if (!BurnerAntiFlapping::canChangePowerLevel(afLevel)) {
            // Anti-flapping prevents change - maintain current state
            LOG_DEBUG(TAG, "Anti-flapping: cannot change %s -> %s yet",
                     powerLevelToString(lastOutput_.powerLevel),
                     powerLevelToString(desiredLevel));
            desiredLevel = lastOutput_.powerLevel;
        }
    }

    output.powerLevel = desiredLevel;
    output.burnerOn = (desiredLevel != PowerLevel::OFF);

    // Set modulation percent based on power level (for status reporting)
    switch (output.powerLevel) {
        case PowerLevel::OFF:
            output.modulationPercent = 0;
            break;
        case PowerLevel::HALF:
            output.modulationPercent = 50;
            break;
        case PowerLevel::FULL:
            output.modulationPercent = 100;
            break;
    }

    return output;
}

BoilerTempController::ControlOutput BoilerTempController::calculateModulating(
    Temperature_t target, Temperature_t current) {

    // Calculate time delta since last PID calculation (last time read before millis(),
    // like predictHeatDemand())
    const uint32_t lastPidMs = lastCycleTime_;
    uint32_t now = millis();
    uint32_t dtMs = now - lastPidMs;
    if (dtMs == 0) dtMs = 100;  // Minimum 100ms
    lastCycleTime_ = now;

    // The PID only runs while a request is active and not during autotune, so the
    // first call after a pause sees a dt of minutes or hours. Integrating that
    // throws the integral straight to its clamp (2026-09-13: burner FULL above
    // target right after an autotune stop). Restart the PID instead, and start the
    // power mapping from OFF: the level of the previous request (HALF, or FULL after
    // an autotune stop) is stale and HALF holds down to 35 % (review R1-1).
    // (BoilerPowerLevel::PID_MAX_DT_MS 10 s, PID_NOMINAL_DT_MS 2.5 s)
    if (BoilerPowerLevel::pidPaused(now, lastPidMs)) {
        const BoilerPowerLevel::CycleStart start =
            BoilerPowerLevel::modulatingCycleStart(lastOutput_.powerLevel, true, false);
        LOG_INFO(TAG, "PID resumed after %lu ms pause - resetting PID state, level %s -> %s",
                 dtMs, powerLevelToString(lastOutput_.powerLevel), powerLevelToString(start.level));
        pidController_->reset();
        // calculate() compares against lastOutput_ for `changed`; the burner is not
        // driven by this controller during a pause
        lastOutput_.powerLevel = start.level;
        lastOutput_.burnerOn = (start.level != PowerLevel::OFF);
        dtMs = BoilerPowerLevel::PID_NOMINAL_DT_MS;
    }

    ControlOutput output = lastOutput_;

    // Calculate PID adjustment
    // The PID outputs a temperature adjustment; we scale this to 0-100%
    // Gains are float (SystemSettings/autotune) but the PID takes fixed-point x1000.
    // Passing them unscaled truncated Kp 34.206 -> 34 (=0.034) and Ki -> 0, which
    // left the output pinned at ~50% so the controller never commanded OFF.
    Temperature_t pidAdjustment = pidController_->calculatePIDAdjustment(
        target,
        current,
        PIDGainFixedPoint::fromFloat(config_.modKp),
        PIDGainFixedPoint::fromFloat(config_.modKi),
        PIDGainFixedPoint::fromFloat(config_.modKd),
        dtMs
    );

    // Pure PID control - no feedforward
    // PID adjustment is in tenths of degrees
    // Scale to 0-100% power output:
    // - 50% = equilibrium (at target)
    // - Each 10 tenths (1°C) of adjustment shifts output by ~10%
    // With Kp=5.0, error=10 (1°C): adjustment=50 → 5% shift
    // With Kp=5.0, error=100 (10°C): adjustment=500 → 50% shift (maxed at FULL)
    int32_t pidPower = PIDGainFixedPoint::powerPercentFromAdjustment(pidAdjustment);  // 0..100

    // Calculate error for logging only
    [[maybe_unused]] Temperature_t error = tempSub(target, current);  // positive = need heat

    uint8_t pidOutput = static_cast<uint8_t>(pidPower);
    lastPIDOutput_ = pidOutput;
    output.modulationPercent = pidOutput;

    // Map PID output to power level with hysteresis (BoilerPowerLevel::fromPidOutput):
    // OFF needs > halfThreshold + hysteresis to turn on, FULL drops to HALF below
    // fullThreshold - hysteresis, HALF/FULL turn off below offThreshold
    const BoilerPowerLevel::Thresholds thresholds = {
        config_.offThreshold, config_.halfThreshold, config_.fullThreshold, config_.thresholdHysteresis
    };
    PowerLevel desiredLevel = BoilerPowerLevel::fromPidOutput(lastOutput_.powerLevel, pidOutput, thresholds);

    // Check anti-flapping before changing power level
    if (desiredLevel != lastOutput_.powerLevel) {
        BurnerAntiFlapping::PowerLevel afLevel;
        switch (desiredLevel) {
            case PowerLevel::OFF:
                afLevel = BurnerAntiFlapping::PowerLevel::OFF;
                break;
            case PowerLevel::HALF:
                afLevel = BurnerAntiFlapping::PowerLevel::POWER_LOW;
                break;
            case PowerLevel::FULL:
                afLevel = BurnerAntiFlapping::PowerLevel::POWER_HIGH;
                break;
            default:
                afLevel = BurnerAntiFlapping::PowerLevel::OFF;
                break;
        }

        if (!BurnerAntiFlapping::canChangePowerLevel(afLevel)) {
            LOG_DEBUG(TAG, "Anti-flapping: cannot change %s -> %s yet",
                     powerLevelToString(lastOutput_.powerLevel),
                     powerLevelToString(desiredLevel));
            desiredLevel = lastOutput_.powerLevel;
        }
    }

    output.powerLevel = desiredLevel;
    output.burnerOn = (desiredLevel != PowerLevel::OFF);

    // Log PID state periodically or on change
    static uint32_t lastLogTime = 0;
    if (output.powerLevel != lastOutput_.powerLevel || (now - lastLogTime) > SystemConstants::Tasks::BoilerTempControl::LOG_INTERVAL_MS) {
        LOG_DEBUG(TAG, "PID: err=%.1f adj=%.1f out=%u%% -> %s",
                 tempToFloat(error),
                 tempToFloat(pidAdjustment),
                 pidOutput,
                 powerLevelToString(output.powerLevel));
        lastLogTime = now;
    }

    return output;
}

bool BoilerTempController::predictHeatDemand(Temperature_t targetTemp, Temperature_t currentTemp,
                                             bool& wantsHeat) const {
    if (!initialized_ || autoTuningActive_ || pidController_ == nullptr) {
        return false;
    }

    // Inputs of the next cycle: updateMode() takes the gains of the mode given by the
    // WATER request bit straight from SystemSettings (read before mutex_, as there)
    EventBits_t requestBits = BurnerRequestManager::getCurrentRequests();
    const bool waterMode = (requestBits & SystemEvents::BurnerRequest::WATER) != 0;
    const SystemSettings& settings = SRP::getSystemSettings();
    const float kp = waterMode ? settings.wHeaterKp : settings.spaceHeatingKp;
    const float ki = waterMode ? settings.wHeaterKi : settings.spaceHeatingKi;
    const float kd = waterMode ? settings.wHeaterKd : settings.spaceHeatingKd;

    FixedPointPIDStep::State pidState;
    FixedPointPIDStep::Limits pidLimits;
    if (!pidController_->snapshot(pidState, pidLimits)) {
        return false;
    }

    MutexGuard guard(mutex_, MUTEX_TIMEOUT);
    if (!guard.hasLock()) {
        return false;
    }

    // calculate() writes lastCycleTime_ and lastOutput_ from BoilerTempControlTask without
    // mutex_. Copy them first and take millis() afterwards: every stored lastCycleTime_ is
    // a millis() value from before our read, so now - lastPidMs cannot wrap into a false
    // pause (review R1-2). A cycle completing concurrently only makes this prediction
    // one cycle old, which BoilerTempControlTask's level-triggered decide() corrects.
    const uint32_t lastPidMs = lastCycleTime_;
    const PowerLevel lastLevel = lastOutput_.powerLevel;
    const uint32_t nowMs = millis();

    // calculate() returns OFF for an invalid target or reading
    if (!isValidTarget(targetTemp) || currentTemp == TEMP_INVALID) {
        wantsHeat = false;
        return true;
    }

    // Both paths start from OFF after a pause (BoilerPowerLevel::modulatingCycleStart /
    // bangBangCycleStart), so the prediction must use the same start level
    const bool paused = BoilerPowerLevel::pidPaused(nowMs, lastPidMs);

    PowerLevel level;
    if (config_.burnerType == BurnerType::TWO_STAGE) {
        const BoilerPowerLevel::BangBangBands bands = {
            config_.offHysteresis, config_.onHysteresis, config_.fullPowerThreshold
        };
        level = BoilerPowerLevel::fromBangBangError(
            BoilerPowerLevel::bangBangCycleStart(lastLevel, paused),
            tempSub(targetTemp, currentTemp), bands);
    } else {
        // updateMode() resets the PID on a mode or gain change (level kept),
        // calculateModulating() after a pause (level from OFF, nominal dt):
        // BoilerPowerLevel::modulatingCycleStart()
        const bool modeOrGainsChanged = (waterMode != isWaterMode_) ||
            (kp != config_.modKp) || (ki != config_.modKi) || (kd != config_.modKd);
        const BoilerPowerLevel::Thresholds thresholds = {
            config_.offThreshold, config_.halfThreshold, config_.fullThreshold, config_.thresholdHysteresis
        };
        level = BoilerPowerLevel::predictModulatingLevel(lastLevel, pidState, pidLimits,
                                                         paused, modeOrGainsChanged,
                                                         targetTemp, currentTemp,
                                                         PIDGainFixedPoint::fromFloat(kp),
                                                         PIDGainFixedPoint::fromFloat(ki),
                                                         PIDGainFixedPoint::fromFloat(kd),
                                                         thresholds);
    }

    wantsHeat = (level != PowerLevel::OFF);
    return true;
}

void BoilerTempController::reset() {
    MutexGuard guard(mutex_, MUTEX_TIMEOUT);
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "reset: mutex timeout");
    }

    lastOutput_.burnerOn = false;
    lastOutput_.powerLevel = PowerLevel::OFF;
    lastOutput_.modulationPercent = 0;
    lastOutput_.changed = false;

    // Reset PID state
    if (pidController_ != nullptr) {
        pidController_->reset();
    }
    lastPIDOutput_ = 0;
    lastCycleTime_ = millis();

    LOG_INFO(TAG, "Controller reset (including PID)");
}

void BoilerTempController::setPIDGains(float kp, float ki, float kd) {
    MutexGuard guard(mutex_, MUTEX_TIMEOUT);
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "setPIDGains: mutex timeout");
        return;
    }

    config_.modKp = kp;
    config_.modKi = ki;
    config_.modKd = kd;

    LOG_INFO(TAG, "PID gains updated: Kp=%.2f Ki=%.3f Kd=%.2f", kp, ki, kd);
}

void BoilerTempController::getPIDGains(float& kp, float& ki, float& kd) const {
    MutexGuard guard(mutex_, MUTEX_TIMEOUT);
    if (!guard.hasLock()) {
        LOG_WARN(TAG, "getPIDGains: mutex timeout - returning cached values");
    }

    kp = config_.modKp;
    ki = config_.modKi;
    kd = config_.modKd;
}

void BoilerTempController::resetPID() {
    MutexGuard guard(mutex_, MUTEX_TIMEOUT);
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "resetPID: mutex timeout");
        return;
    }

    if (pidController_ != nullptr) {
        pidController_->reset();
    }
    lastPIDOutput_ = 0;

    LOG_DEBUG(TAG, "PID controller reset");
}

void BoilerTempController::updateMode() {
    // Check event bits to determine current mode
    EventBits_t requestBits = BurnerRequestManager::getCurrentRequests();
    bool isWaterMode = (requestBits & SystemEvents::BurnerRequest::WATER) != 0;

    bool modeChanged = (isWaterMode_ != isWaterMode);
    isWaterMode_ = isWaterMode;

    if (config_.burnerType != BurnerType::MODULATING) {
        // TWO_STAGE mode: Mode switching doesn't affect bang-bang thresholds
        if (modeChanged) {
            LOG_DEBUG(TAG, "Mode switched to %s - bang-bang thresholds unchanged",
                     isWaterMode ? "WATER" : "SPACE");
        }
        return;
    }

    // PID mode: use the gains of the active mode straight from SystemSettings,
    // checked every cycle. Previously the water gains were copied once at init
    // and only mode switches reloaded anything, so an MQTT/UI gain change had no
    // effect until reboot.
    // Space heating: Large thermal mass (radiators) → conservative gains (avoid overshoot)
    // Water heating: Medium thermal mass (tank) → aggressive gains (fast charging)
    SystemSettings& settings = SRP::getSystemSettings();
    float kp = isWaterMode ? settings.wHeaterKp : settings.spaceHeatingKp;
    float ki = isWaterMode ? settings.wHeaterKi : settings.spaceHeatingKi;
    float kd = isWaterMode ? settings.wHeaterKd : settings.spaceHeatingKd;

    MutexGuard guard(mutex_, MUTEX_TIMEOUT);
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "updateMode: mutex timeout");
        return;
    }

    bool gainsChanged = (kp != config_.modKp) || (ki != config_.modKi) || (kd != config_.modKd);
    if (!modeChanged && !gainsChanged) {
        return;
    }

    config_.modKp = kp;
    config_.modKi = ki;
    config_.modKd = kd;
    if (isWaterMode) {
        config_.waterKp = kp;
        config_.waterKi = ki;
        config_.waterKd = kd;
    }
    LOG_INFO(TAG, "%s %s heating PID: Kp=%.2f Ki=%.4f Kd=%.2f",
             modeChanged ? "Switched to" : "Gains updated for",
             isWaterMode ? "WATER" : "SPACE", kp, ki, kd);

    // Reset PID to prevent integral windup / output bumps on mode or gain change
    if (pidController_ != nullptr) {
        pidController_->reset();
    }
    lastPIDOutput_ = 0;
}

BoilerTempController::ControlOutput BoilerTempController::getLastOutput() const {
    MutexGuard guard(mutex_, MUTEX_TIMEOUT);
    if (!guard.hasLock()) {
        LOG_WARN(TAG, "getLastOutput: mutex timeout");
    }
    return lastOutput_;
}

bool BoilerTempController::isValidTarget(Temperature_t target) const {
    // Check for invalid marker
    if (target == TEMP_INVALID) {
        return false;
    }

    // Check minimum target (safety)
    if (target < config_.minTargetTemp) {
        return false;
    }

    // Check maximum target (safety - should not exceed MAX_BOILER_TEMP)
    if (target > SystemConstants::Temperature::MAX_BOILER_TEMP_C) {
        return false;
    }

    return true;
}

const char* BoilerTempController::powerLevelToString(PowerLevel level) {
    switch (level) {
        case PowerLevel::OFF:  return "OFF";
        case PowerLevel::HALF: return "HALF";
        case PowerLevel::FULL: return "FULL";
        default:               return "UNKNOWN";
    }
}

// ===== Auto-Tuning Implementation =====

bool BoilerTempController::setTuningMethod(const char* method) {
    MutexGuard guard(mutex_, MUTEX_TIMEOUT);
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "setTuningMethod: mutex timeout");
        return false;
    }

    if (autoTuningActive_) {
        LOG_WARN(TAG, "Cannot change tuning method while tuning is active");
        return false;
    }

    // Map string to enum
    if (strcmp(method, "zn_pi") == 0) {
        tuningMethod_ = PIDAutoTuner::TuningMethod::ZIEGLER_NICHOLS_PI;
        LOG_INFO(TAG, "Tuning method set to Ziegler-Nichols PI (conservative)");
    } else if (strcmp(method, "zn_pid") == 0) {
        tuningMethod_ = PIDAutoTuner::TuningMethod::ZIEGLER_NICHOLS_PID;
        LOG_INFO(TAG, "Tuning method set to Ziegler-Nichols PID (classic)");
    } else if (strcmp(method, "tyreus") == 0) {
        tuningMethod_ = PIDAutoTuner::TuningMethod::TYREUS_LUYBEN;
        LOG_INFO(TAG, "Tuning method set to Tyreus-Luyben (less overshoot)");
    } else if (strcmp(method, "cohen") == 0) {
        tuningMethod_ = PIDAutoTuner::TuningMethod::COHEN_COON;
        LOG_INFO(TAG, "Tuning method set to Cohen-Coon (for delayed processes)");
    } else if (strcmp(method, "lambda") == 0) {
        tuningMethod_ = PIDAutoTuner::TuningMethod::LAMBDA_TUNING;
        LOG_INFO(TAG, "Tuning method set to Lambda (minimal overshoot)");
    } else {
        LOG_WARN(TAG, "Unknown tuning method: %s (valid: zn_pi, zn_pid, tyreus, cohen, lambda)", method);
        return false;
    }

    return true;
}

bool BoilerTempController::startAutoTuning(Temperature_t setpoint) {
    // Relay test parameters from settings (pid/autotune/amplitude, pid/autotune/hysteresis),
    // read before taking mutex_ so the settings mutex is never held behind it
    float relayAmplitude = SystemConstants::PID::Autotune::DEFAULT_RELAY_AMPLITUDE;
    float hysteresis = SystemConstants::PID::Autotune::DEFAULT_RELAY_HYSTERESIS;
    int32_t methodIndex = -1;
    if (SRP::takeSystemSettingsMutex(pdMS_TO_TICKS(50)) == pdTRUE) {
        const SystemSettings& settings = SRP::getSystemSettings();
        relayAmplitude = AutotuneRelayConfig::amplitudeOrDefault(settings.autotuneRelayAmplitude, relayAmplitude);
        hysteresis = AutotuneRelayConfig::hysteresisOrDefault(settings.autotuneHysteresis, hysteresis);
        methodIndex = settings.autotuneMethod;
        SRP::giveSystemSettingsMutex();
    } else {
        LOG_WARN(TAG, "startAutoTuning: settings mutex timeout - using default amplitude %.1f%% / hysteresis %.1f°C",
                 relayAmplitude, hysteresis);
    }

    // Mode of this run, from the same source as updateMode() and read before mutex_.
    // updateMode() does not run while tuning, so isWaterMode_ would still hold the mode of
    // the last normal control cycle when the result is saved (review 2026-09-14 pid-4).
    const EventBits_t startRequestBits = BurnerRequestManager::getCurrentRequests();
    const bool waterModeAtStart = (startRequestBits & SystemEvents::BurnerRequest::WATER) != 0;

    MutexGuard guard(mutex_, MUTEX_TIMEOUT);
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "startAutoTuning: mutex timeout");
        return false;
    }

    if (autoTuningActive_) {
        LOG_WARN(TAG, "Auto-tuning already in progress");
        return false;
    }

    if (autoTuner_ == nullptr) {
        LOG_ERROR(TAG, "Auto-tuner not initialized");
        return false;
    }

    // Method from the saved setting at every start: initialize() runs before NVS is loaded
    // (a method chosen before a reboot was lost), and pid/autotune/method set through
    // boiler/params never reached the controller (review 2026-09-14)
    PIDAutoTuner::TuningMethod settingsMethod;
    if (tuningMethodFromIndex(methodIndex, settingsMethod)) {
        tuningMethod_ = settingsMethod;
    }

    // The relay test below drives the burner OFF <-> FULL (updateAutoTuning), a half-swing
    // of 50 %. Another amplitude is used as-is in Ku = 4d/(pi a) and scales the gains.
    if (!AutotuneRelayConfig::matchesOutputSwing(relayAmplitude, AutotuneRelayConfig::TWO_STAGE_SWING)) {
        LOG_WARN(TAG, "Autotune amplitude %.1f%% differs from the OFF/FULL output swing (%.0f%%) - tuned gains scale by %.2f",
                 relayAmplitude, AutotuneRelayConfig::TWO_STAGE_SWING,
                 AutotuneRelayConfig::gainScale(relayAmplitude, AutotuneRelayConfig::TWO_STAGE_SWING));
    }
    float setpointFloat = tempToFloat(setpoint);

    if (autoTuner_->startTuning(setpointFloat, relayAmplitude, hysteresis, tuningMethod_)) {
        autoTuningActive_ = true;
        autoTuneSetpoint_ = setpoint;
        autoTuneWaterMode_ = waterModeAtStart;
        // The tuner gets the time since this moment, not an absolute millis() (pid-7)
        autoTuneClock_.start(millis());

        // Reset PID for fresh start after tuning
        if (pidController_ != nullptr) {
            pidController_->reset();
        }

        LOG_INFO(TAG, "Auto-tuning started at setpoint %.1f°C in %s mode",
                 setpointFloat, waterModeAtStart ? "WATER" : "SPACE");
        return true;
    }

    LOG_ERROR(TAG, "Failed to start auto-tuning");
    return false;
}

void BoilerTempController::stopAutoTuning() {
    MutexGuard guard(mutex_, MUTEX_TIMEOUT);
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "stopAutoTuning: mutex timeout");
        return;
    }

    if (autoTuner_ != nullptr && autoTuningActive_) {
        autoTuner_->stopTuning();
        autoTuningActive_ = false;
        LOG_INFO(TAG, "Auto-tuning stopped");
    }
}

BoilerTempController::ControlOutput BoilerTempController::updateAutoTuning(Temperature_t currentTemp) {
    ControlOutput output = lastOutput_;

    if (!autoTuningActive_ || autoTuner_ == nullptr) {
        return output;
    }

    // Update auto-tuner with current temperature.
    // Time base: seconds since startAutoTuning(), from uint32_t millis() differences.
    // static_cast<float>(millis()) / 1000.0f was absolute, so the ~49-day millis() wrap
    // threw the tuner's peak/trough times and its timeout backwards, and near the wrap the
    // float mantissa quantised the value to 0.5 s steps (AutotuneClock, review pid-7).
    float currentTempFloat = tempToFloat(currentTemp);
    float currentTime = autoTuneClock_.elapsedSeconds(millis());

    float tunerOutput = autoTuner_->update(currentTempFloat, currentTime);

    // Debug: log tuner output periodically
    static uint32_t lastLogTime = 0;
    uint32_t now = millis();
    if (now - lastLogTime > 5000) {  // Every 5 seconds
        LOG_INFO(TAG, "Autotune: temp=%.1f tunerOut=%.1f cycles=%u",
                 currentTempFloat, tunerOutput, autoTuner_->getCycleCount());
        lastLogTime = now;
    }

    // Map tuner output to power level
    // Tuner outputs relay amplitude (positive = ON, negative = OFF)
    //
    // IMPORTANT: Use FULL power during oscillation test!
    // At HALF power, the boiler can only maintain ~55°C but cannot overshoot
    // to 56°C (setpoint + hysteresis) which is needed to trigger relay OFF.
    // FULL power provides enough thermal energy to create proper oscillations.
    if (tunerOutput > 0) {
        output.powerLevel = PowerLevel::FULL;  // Full power for proper oscillations
        output.burnerOn = true;
    } else {
        output.powerLevel = PowerLevel::OFF;
        output.burnerOn = false;
    }

    output.modulationPercent = output.burnerOn ? 100 : 0;
    output.changed = (output.powerLevel != lastOutput_.powerLevel);

    // Check if tuning is complete
    if (autoTuner_->isComplete()) {
        LOG_INFO(TAG, "Auto-tuning completed successfully");
        autoTuningActive_ = false;
    } else if (autoTuner_->getState() == PIDAutoTuner::TuningState::FAILED) {
        LOG_ERROR(TAG, "Auto-tuning failed");
        autoTuningActive_ = false;
    }

    lastOutput_ = output;
    return output;
}

bool BoilerTempController::applyAutoTuningResults() {
    MutexGuard guard(mutex_, MUTEX_TIMEOUT);
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "applyAutoTuningResults: mutex timeout");
        return false;
    }

    if (autoTuner_ == nullptr || !autoTuner_->isComplete()) {
        LOG_WARN(TAG, "No valid auto-tuning results to apply");
        return false;
    }

    PIDAutoTuner::TuningResult results = autoTuner_->getResults();
    if (!results.valid) {
        LOG_ERROR(TAG, "Auto-tuning results are invalid");
        return false;
    }

    // Gain set of the run, not of the (stale) isWaterMode_ flag. A mode change during the
    // run means the oscillation was measured on two different loads, so the result belongs
    // to neither gain set - reject it instead of writing wrong gains (AutotuneGainTarget).
    const AutotuneGainTarget::Target gainTarget = getTunedGainTarget();
    if (gainTarget == AutotuneGainTarget::Target::REJECT_MODE_CHANGED) {
        LOG_ERROR(TAG, "Auto-tuning result rejected: started in %s mode, %s mode active now",
                  autoTuneWaterMode_ ? "WATER" : "SPACE",
                  autoTuneWaterMode_ ? "SPACE" : "WATER");
        return false;
    }
    const bool waterGains = (gainTarget == AutotuneGainTarget::Target::WATER);

    // Apply the tuned gains to active gains
    config_.modKp = results.Kp;
    config_.modKi = results.Ki;
    config_.modKd = results.Kd;

    // Also update mode-specific gains so they persist across mode switches
    if (waterGains) {
        config_.waterKp = results.Kp;
        config_.waterKi = results.Ki;
        config_.waterKd = results.Kd;
        LOG_INFO(TAG, "Applied auto-tuning to WATER gains: Kp=%.3f Ki=%.4f Kd=%.3f",
                 results.Kp, results.Ki, results.Kd);
    } else {
        LOG_INFO(TAG, "Applied auto-tuning to SPACE gains: Kp=%.3f Ki=%.4f Kd=%.3f",
                 results.Kp, results.Ki, results.Kd);
    }

    // Reset PID with new parameters
    if (pidController_ != nullptr) {
        pidController_->reset();
    }
    lastPIDOutput_ = 0;

    return true;
}

uint8_t BoilerTempController::getAutoTuningProgress() const {
    MutexGuard guard(mutex_, MUTEX_TIMEOUT);
    if (!guard.hasLock() || autoTuner_ == nullptr) {
        return 0;
    }
    return autoTuner_->getProgress();
}

AutotuneGainTarget::Target BoilerTempController::getTunedGainTarget() const {
    // Mode now, from the active burner request - the same source updateMode() uses.
    // autoTuneWaterMode_ is written once in startAutoTuning() and only read here and in
    // applyAutoTuningResults(), so no mutex is needed (like isWaterMode()); taking one
    // would also deadlock the caller that already holds mutex_.
    const EventBits_t requestBits = BurnerRequestManager::getCurrentRequests();
    const bool waterModeNow = (requestBits & SystemEvents::BurnerRequest::WATER) != 0;

    return AutotuneGainTarget::select(autoTuneWaterMode_, waterModeNow);
}

bool BoilerTempController::getTunedGains(float& kp, float& ki, float& kd) const {
    MutexGuard guard(mutex_, MUTEX_TIMEOUT);
    if (!guard.hasLock()) {
        return false;
    }

    // Return the currently applied gains (from config_)
    kp = config_.modKp;
    ki = config_.modKi;
    kd = config_.modKd;
    return true;
}
