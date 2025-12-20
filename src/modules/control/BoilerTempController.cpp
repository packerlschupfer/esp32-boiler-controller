// src/modules/control/BoilerTempController.cpp
#include "modules/control/BoilerTempController.h"
#include "modules/control/BurnerAntiFlapping.h"
#include "modules/control/BurnerRequestManager.h"
#include "config/SystemConstants.h"
#include "config/SafetyConfig.h"
#include "config/SystemSettingsStruct.h"
#include "core/SystemResourceProvider.h"
#include "events/SystemEventsGenerated.h"
#include "LoggingMacros.h"
#include <MutexGuard.h>
#include <Arduino.h>

const char* BoilerTempController::TAG = "BoilerTempCtrl";

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
    lastPIDTime_ = millis();

    // Load PID gains from SystemSettings
    SystemSettings& settings = SRP::getSystemSettings();
    config_.modKp = settings.spaceHeatingKp;
    config_.modKi = settings.spaceHeatingKi;
    config_.modKd = settings.spaceHeatingKd;
    config_.waterKp = settings.wHeaterKp;
    config_.waterKi = settings.wHeaterKi;
    config_.waterKd = settings.wHeaterKd;

    initialized_ = true;

    LOG_INFO(TAG, "Initialized - Mode:%s",
             config_.burnerType == BurnerType::TWO_STAGE ? "BANG_BANG" : "PID");
    LOG_INFO(TAG, "Space PID: Kp:%.2f Ki:%.4f Kd:%.2f",
             config_.modKp, config_.modKi, config_.modKd);
    LOG_INFO(TAG, "Water PID: Kp:%.2f Ki:%.4f Kd:%.2f",
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

    ControlOutput output = lastOutput_;
    output.modulationPercent = 0;  // Not used for two-stage

    // Calculate error (positive = need more heat)
    Temperature_t error = tempSub(target, current);

    // Convert thresholds to signed for comparison
    Temperature_t offThreshold = config_.offHysteresis;      // e.g., +50 (+5.0°C)
    Temperature_t onThreshold = config_.onHysteresis;        // e.g., +30 (+3.0°C)
    Temperature_t fullThreshold = config_.fullPowerThreshold; // e.g., +100 (+10.0°C)

    // Three-point bang-bang control with hysteresis
    //
    // State transitions (error = target - current):
    //   error > fullThreshold      → FULL power (very cold, need max heat)
    //   error > onThreshold        → HALF power (moderately cold)
    //   error < -offThreshold      → OFF (above target + hysteresis)
    //
    // Hysteresis prevents rapid switching at boundaries:
    //   - Stay in current state unless error crosses a threshold
    //   - Different thresholds for turning ON vs OFF

    bool shouldBeOff = (error < -offThreshold);  // Too hot: turn off
    bool shouldBeFull = (error > fullThreshold); // Very cold: full power
    bool shouldBeOn = (error > onThreshold);     // Cold enough: at least half power

    PowerLevel desiredLevel;

    if (shouldBeOff) {
        desiredLevel = PowerLevel::OFF;
    } else if (shouldBeFull) {
        desiredLevel = PowerLevel::FULL;
    } else if (shouldBeOn) {
        desiredLevel = PowerLevel::HALF;
    } else {
        // In hysteresis band - maintain current state
        desiredLevel = lastOutput_.powerLevel;
    }

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

    ControlOutput output = lastOutput_;

    // Calculate time delta since last PID calculation
    uint32_t now = millis();
    uint32_t dtMs = now - lastPIDTime_;
    if (dtMs == 0) dtMs = 100;  // Minimum 100ms
    lastPIDTime_ = now;

    // Calculate PID adjustment
    // The PID outputs a temperature adjustment; we scale this to 0-100%
    Temperature_t pidAdjustment = pidController_->calculatePIDAdjustment(
        target,
        current,
        config_.modKp,
        config_.modKi,
        config_.modKd,
        dtMs
    );

    // Pure PID control - no feedforward
    // PID adjustment is in tenths of degrees
    // Scale to 0-100% power output:
    // - 50% = equilibrium (at target)
    // - Each 10 tenths (1°C) of adjustment shifts output by ~10%
    // With Kp=5.0, error=10 (1°C): adjustment=50 → 5% shift
    // With Kp=5.0, error=100 (10°C): adjustment=500 → 50% shift (maxed at FULL)
    int32_t pidPower = 50 + (pidAdjustment / 10);

    // Calculate error for logging only
    [[maybe_unused]] Temperature_t error = tempSub(target, current);  // positive = need heat

    // Clamp to 0-100 range
    if (pidPower < 0) pidPower = 0;
    if (pidPower > 100) pidPower = 100;

    uint8_t pidOutput = static_cast<uint8_t>(pidPower);
    lastPIDOutput_ = pidOutput;
    output.modulationPercent = pidOutput;

    // Map PID output to power level with hysteresis
    PowerLevel desiredLevel = lastOutput_.powerLevel;

    // Apply hysteresis around thresholds to prevent rapid switching
    uint8_t offLow = config_.offThreshold;
    uint8_t halfHigh = config_.halfThreshold + config_.thresholdHysteresis;
    uint8_t fullLow = config_.fullThreshold - config_.thresholdHysteresis;
    uint8_t fullHigh = config_.fullThreshold;

    // State machine with hysteresis
    switch (lastOutput_.powerLevel) {
        case PowerLevel::OFF:
            // Currently OFF - need to cross halfThreshold + hysteresis to turn on
            if (pidOutput > halfHigh) {
                desiredLevel = (pidOutput > fullHigh) ? PowerLevel::FULL : PowerLevel::HALF;
            }
            break;

        case PowerLevel::HALF:
            // Currently HALF - check both directions
            if (pidOutput < offLow) {
                desiredLevel = PowerLevel::OFF;
            } else if (pidOutput > fullHigh) {
                desiredLevel = PowerLevel::FULL;
            }
            break;

        case PowerLevel::FULL:
            // Currently FULL - need to drop below fullThreshold - hysteresis to reduce
            if (pidOutput < offLow) {
                desiredLevel = PowerLevel::OFF;
            } else if (pidOutput < fullLow) {
                desiredLevel = PowerLevel::HALF;
            }
            break;
    }

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
    if (output.powerLevel != lastOutput_.powerLevel || (now - lastLogTime) > 30000) {
        LOG_DEBUG(TAG, "PID: err=%.1f adj=%.1f out=%u%% -> %s",
                 tempToFloat(error),
                 tempToFloat(pidAdjustment),
                 pidOutput,
                 powerLevelToString(output.powerLevel));
        lastLogTime = now;
    }

    return output;
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
    lastPIDTime_ = millis();

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

    if (isWaterMode_ != isWaterMode) {
        isWaterMode_ = isWaterMode;

        MutexGuard guard(mutex_, MUTEX_TIMEOUT);
        if (!guard.hasLock()) {
            LOG_ERROR(TAG, "updateMode: mutex timeout");
            return;
        }

        // Switch PID gains based on mode
        if (isWaterMode) {
            config_.modKp = config_.waterKp;
            config_.modKi = config_.waterKi;
            config_.modKd = config_.waterKd;
            LOG_INFO(TAG, "Switched to water heating PID: Kp=%.2f Ki=%.4f Kd=%.2f",
                     config_.waterKp, config_.waterKi, config_.waterKd);
        } else {
            // Reload space heating gains from SystemSettings
            SystemSettings& settings = SRP::getSystemSettings();
            config_.modKp = settings.spaceHeatingKp;
            config_.modKi = settings.spaceHeatingKi;
            config_.modKd = settings.spaceHeatingKd;
            LOG_INFO(TAG, "Switched to space heating PID: Kp=%.2f Ki=%.4f Kd=%.2f",
                     config_.modKp, config_.modKi, config_.modKd);
        }

        // Reset PID to prevent integral windup on mode switch
        if (pidController_ != nullptr) {
            pidController_->reset();
        }
        lastPIDOutput_ = 0;
    }
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

    // Use relay feedback test parameters
    float relayAmplitude = 50.0f;  // 50% output swing (HALF power)
    float hysteresis = 1.0f;       // 1°C hysteresis band
    float setpointFloat = tempToFloat(setpoint);

    if (autoTuner_->startTuning(setpointFloat, relayAmplitude, hysteresis, tuningMethod_)) {
        autoTuningActive_ = true;
        autoTuneSetpoint_ = setpoint;

        // Reset PID for fresh start after tuning
        if (pidController_ != nullptr) {
            pidController_->reset();
        }

        LOG_INFO(TAG, "Auto-tuning started at setpoint %.1f°C", setpointFloat);
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

    // Update auto-tuner with current temperature
    float currentTempFloat = tempToFloat(currentTemp);
    float currentTime = static_cast<float>(millis()) / 1000.0f;  // Convert to seconds

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

    // Apply the tuned gains to active gains
    config_.modKp = results.Kp;
    config_.modKi = results.Ki;
    config_.modKd = results.Kd;

    // Also update mode-specific gains so they persist across mode switches
    if (isWaterMode_) {
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
