// src/modules/control/PIDControlModule.cpp
#include "PIDControlModule.h"
#include "config/SystemConstants.h"
#include <algorithm>
#include "SemaphoreGuard.h"
#include "config/ProjectConfig.h"

PIDControlModule::PIDControlModule() 
    : integral(0.0f)
    , previousError(0.0f)
    , autoTuner(nullptr)
    , autoTuningActive(false)
    , autoTuneSetpoint(0.0f)
    , currentKp(SystemConstants::PID::DEFAULT_KP)
    , currentKi(SystemConstants::PID::DEFAULT_KI)
    , currentKd(SystemConstants::PID::DEFAULT_KD) {
    // Create mutex for thread safety
    pidMutex = xSemaphoreCreateMutex();
    if (pidMutex == nullptr) {
        LOG_ERROR("PIDControl", "Failed to create mutex");
    }
    
    // Create auto-tuner instance
    autoTuner = new PIDAutoTuner();
    if (!autoTuner) {
        LOG_ERROR("PIDControl", "CRITICAL: Failed to allocate PIDAutoTuner - OOM");
    }
}

PIDControlModule::~PIDControlModule() {
    if (autoTuner != nullptr) {
        delete autoTuner;
    }
    if (pidMutex != nullptr) {
        vSemaphoreDelete(pidMutex);
    }
}

float PIDControlModule::calculatePIDAdjustment(float setPoint, float currentTemp,
                                               float Kp, float Ki, float Kd) {
    // Use SemaphoreGuard for automatic mutex management
    // 10ms timeout: PID calculation is fast, avoid blocking control loop
    SemaphoreGuard guard(pidMutex, pdMS_TO_TICKS(10));
    if (!guard.hasLock()) {
        LOG_ERROR("PIDControl", "Failed to acquire mutex for PID calculation");
        return 0.0f; // Safe default
    }
    
    // Calculate error
    float error = setPoint - currentTemp;
    
    // Proportional term
    float P = Kp * error;
    
    // Integral term with anti-windup
    integral += error;
    // Limit integral to prevent windup
    integral = std::max(SystemConstants::PID::INTEGRAL_MIN, std::min(SystemConstants::PID::INTEGRAL_MAX, integral));
    float I = Ki * integral;
    
    // Derivative term
    float derivative = error - previousError;
    float D = Kd * derivative;
    
    // Store error for next iteration
    previousError = error;
    
    // Calculate total adjustment
    float adjustment = P + I + D;
    
    // Limit output to reasonable range
    adjustment = std::max(SystemConstants::PID::OUTPUT_MIN, std::min(SystemConstants::PID::OUTPUT_MAX, adjustment));
    
    return adjustment;
}

void PIDControlModule::reset() {
    // 10ms timeout: reset is infrequent, keep consistent with calculatePIDAdjustment
    SemaphoreGuard guard(pidMutex, pdMS_TO_TICKS(10));
    if (!guard.hasLock()) {
        LOG_ERROR("PIDControl", "Failed to acquire mutex for reset");
        return;
    }
    
    integral = 0.0f;
    previousError = 0.0f;
}

uint32_t PIDControlModule::determineAdjustmentLevel(float adjustment) {
    // Convert continuous adjustment to discrete levels
    // Assuming adjustment range is -100 to +100
    
    if (adjustment < SystemConstants::PID::LEVEL_0_THRESHOLD) {
        return 0; // Maximum cooling/decrease
    } else if (adjustment < SystemConstants::PID::LEVEL_1_THRESHOLD) {
        return 1; // Moderate cooling/decrease
    } else if (adjustment < SystemConstants::PID::LEVEL_2_THRESHOLD) {
        return 2; // Light cooling/decrease
    } else if (adjustment < SystemConstants::PID::LEVEL_3_THRESHOLD) {
        return 3; // No change/maintain
    } else if (adjustment < SystemConstants::PID::LEVEL_4_THRESHOLD) {
        return 4; // Light heating/increase
    } else if (adjustment < SystemConstants::PID::LEVEL_5_THRESHOLD) {
        return 5; // Moderate heating/increase
    } else {
        return 6; // Maximum heating/increase
    }
}

bool PIDControlModule::startAutoTuning(float setpoint, PIDAutoTuner::TuningMethod method) {
    SemaphoreGuard guard(pidMutex, pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        LOG_ERROR("PIDControl", "Failed to acquire mutex for auto-tuning start");
        return false;
    }
    
    if (autoTuningActive) {
        LOG_WARN("PIDControl", "Auto-tuning already in progress");
        return false;
    }
    
    if (autoTuner == nullptr) {
        LOG_ERROR("PIDControl", "Auto-tuner not initialized");
        return false;
    }
    
    // Start auto-tuning with reasonable defaults
    float relayAmplitude = 40.0f;  // 40% output swing
    float hysteresis = 1.0f;       // 1°C hysteresis band
    
    if (autoTuner->startTuning(setpoint, relayAmplitude, hysteresis, method)) {
        autoTuningActive = true;
        autoTuneSetpoint = setpoint;
        LOG_INFO("PIDControl", "Auto-tuning started with setpoint %.1f°C", setpoint);
        return true;
    }
    
    return false;
}

void PIDControlModule::stopAutoTuning() {
    SemaphoreGuard guard(pidMutex, pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        LOG_ERROR("PIDControl", "Failed to acquire mutex for auto-tuning stop");
        return;
    }
    
    if (autoTuner != nullptr && autoTuningActive) {
        autoTuner->stopTuning();
        autoTuningActive = false;
        LOG_INFO("PIDControl", "Auto-tuning stopped");
    }
}

PIDAutoTuner::TuningState PIDControlModule::getAutoTuningState() const {
    SemaphoreGuard guard(pidMutex, pdMS_TO_TICKS(100));
    if (!guard.hasLock() || autoTuner == nullptr) {
        return PIDAutoTuner::TuningState::IDLE;
    }
    
    return autoTuner->getState();
}

uint8_t PIDControlModule::getAutoTuningProgress() const {
    SemaphoreGuard guard(pidMutex, pdMS_TO_TICKS(100));
    if (!guard.hasLock() || autoTuner == nullptr) {
        return 0;
    }
    
    return autoTuner->getProgress();
}

float PIDControlModule::updateAutoTuning(float currentTemp, float currentTime) {
    SemaphoreGuard guard(pidMutex, pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        LOG_ERROR("PIDControl", "Failed to acquire mutex for auto-tuning update");
        return 0.0f;
    }
    
    if (!autoTuningActive || autoTuner == nullptr) {
        return 0.0f;
    }
    
    float output = autoTuner->update(currentTemp, currentTime);
    
    // Check if tuning is complete
    if (autoTuner->isComplete()) {
        LOG_INFO("PIDControl", "Auto-tuning completed successfully");
        autoTuningActive = false;
    } else if (autoTuner->getState() == PIDAutoTuner::TuningState::FAILED) {
        LOG_ERROR("PIDControl", "Auto-tuning failed");
        autoTuningActive = false;
    }
    
    return output;
}

bool PIDControlModule::applyAutoTuningResults() {
    SemaphoreGuard guard(pidMutex, pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        LOG_ERROR("PIDControl", "Failed to acquire mutex for applying results");
        return false;
    }
    
    if (autoTuner == nullptr || !autoTuner->isComplete()) {
        LOG_WARN("PIDControl", "No valid auto-tuning results to apply");
        return false;
    }
    
    PIDAutoTuner::TuningResult results = autoTuner->getResults();
    if (!results.valid) {
        LOG_ERROR("PIDControl", "Auto-tuning results are invalid");
        return false;
    }
    
    currentKp = results.Kp;
    currentKi = results.Ki;
    currentKd = results.Kd;
    
    // Reset the controller with new parameters
    integral = 0.0f;
    previousError = 0.0f;
    
    LOG_INFO("PIDControl", "Applied auto-tuning results: Kp=%.3f, Ki=%.3f, Kd=%.3f",
             currentKp, currentKi, currentKd);
    
    return true;
}

void PIDControlModule::getCurrentParameters(float& Kp, float& Ki, float& Kd) const {
    SemaphoreGuard guard(pidMutex, pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        // Return defaults if we can't get lock
        Kp = SystemConstants::PID::DEFAULT_KP;
        Ki = SystemConstants::PID::DEFAULT_KI;
        Kd = SystemConstants::PID::DEFAULT_KD;
        return;
    }
    
    Kp = currentKp;
    Ki = currentKi;
    Kd = currentKd;
}

void PIDControlModule::setParameters(float Kp, float Ki, float Kd) {
    SemaphoreGuard guard(pidMutex, pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        LOG_ERROR("PIDControl", "Failed to acquire mutex for setting parameters");
        return;
    }
    
    currentKp = Kp;
    currentKi = Ki;
    currentKd = Kd;
    
    LOG_INFO("PIDControl", "PID parameters set: Kp=%.3f, Ki=%.3f, Kd=%.3f", Kp, Ki, Kd);
}