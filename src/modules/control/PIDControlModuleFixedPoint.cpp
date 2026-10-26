// src/modules/control/PIDControlModuleFixedPoint.cpp
#include "modules/control/PIDControlModuleFixedPoint.h"
#include "SemaphoreGuard.h"
#include "LoggingMacros.h"
#include "config/SystemConstants.h"
#include <algorithm>
#include "core/SystemResourceProvider.h"
#include <RuntimeStorage.h>
#include <Arduino.h>  // for millis()

static_assert(PIDGainFixedPoint::SCALE == SystemConstants::PID::PID_FIXED_POINT_SCALE,
              "FixedPointPIDStep scale must match the fixed-point PID scale");

static const char* TAG = "PIDFixedPoint";

PIDControlModuleFixedPoint::PIDControlModuleFixedPoint()
    : stepState(FixedPointPIDStep::resetState())
    , lastUpdateTime(0) {

    stepLimits.integralMin = INTEGRAL_MIN;
    stepLimits.integralMax = INTEGRAL_MAX;
    stepLimits.outputMin = OUTPUT_MIN;
    stepLimits.outputMax = OUTPUT_MAX;

    // Create mutex for thread safety
    pidMutex = xSemaphoreCreateMutex();
    if (pidMutex == nullptr) {
        LOG_ERROR(TAG, "Failed to create mutex");
    }
}

PIDControlModuleFixedPoint::~PIDControlModuleFixedPoint() {
    if (pidMutex != nullptr) {
        vSemaphoreDelete(pidMutex);
    }
}

Temperature_t PIDControlModuleFixedPoint::calculatePIDAdjustment(
    Temperature_t setPoint, Temperature_t currentTemp,
    PIDValue_t Kp, PIDValue_t Ki, PIDValue_t Kd,
    uint32_t dtMs) {

    // Use SemaphoreGuard for automatic mutex management
    SemaphoreGuard guard(pidMutex, pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "Failed to acquire mutex for PID calculation");
        return 0; // Safe default
    }

    // Validate time delta
    if (dtMs == 0) {
        LOG_WARN(TAG, "Zero time delta, using 1ms");
        dtMs = 1;
    }

    // P, derivative-on-PV and integral with anti-windup, clamped to the output limits.
    // The arithmetic lives in FixedPointPIDStep so the native tests run the same code.
    Temperature_t tempAdjustment = FixedPointPIDStep::step(stepState, stepLimits,
                                                           setPoint, currentTemp,
                                                           Kp, Ki, Kd, dtMs);

    // Update last update time
    lastUpdateTime = millis();

    // Debug logging (comment out in production)
    #ifdef PID_DEBUG
    LOG_DEBUG(TAG, "PID: SP=%d, PV=%d, I=%d, Out=%d",
              setPoint, currentTemp, static_cast<int>(stepState.integral),
              tempAdjustment);
    #endif

    return tempAdjustment;
}

void PIDControlModuleFixedPoint::reset() {
    SemaphoreGuard guard(pidMutex, pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "Failed to acquire mutex for reset");
        return;
    }

    // integral 0, previous PV 0, skip derivative on first call after reset
    stepState = FixedPointPIDStep::resetState();

    LOG_INFO(TAG, "PID controller reset");
}

bool PIDControlModuleFixedPoint::snapshot(FixedPointPIDStep::State& outState,
                                          FixedPointPIDStep::Limits& outLimits) const {
    SemaphoreGuard guard(pidMutex, pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        LOG_WARN(TAG, "Failed to acquire mutex for snapshot");
        return false;
    }

    outState = stepState;
    outLimits = stepLimits;
    return true;
}

void PIDControlModuleFixedPoint::setIntegralLimits(PIDValue_t min, PIDValue_t max) {
    SemaphoreGuard guard(pidMutex, pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "Failed to acquire mutex for setting integral limits");
        return;
    }

    stepLimits.integralMin = min;
    stepLimits.integralMax = max;

    // Apply limits to current integral
    stepState.integral = clamp(stepState.integral, stepLimits.integralMin, stepLimits.integralMax);

    LOG_INFO(TAG, "Integral limits set: [%d, %d]",
             static_cast<int>(min), static_cast<int>(max));
}

void PIDControlModuleFixedPoint::setOutputLimits(Temperature_t min, Temperature_t max) {
    SemaphoreGuard guard(pidMutex, pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "Failed to acquire mutex for setting output limits");
        return;
    }

    stepLimits.outputMin = min;
    stepLimits.outputMax = max;

    LOG_INFO(TAG, "Output limits set: [%d, %d] (0.1°C units)", min, max);
}

bool PIDControlModuleFixedPoint::saveState(uint8_t controllerId) {
    SemaphoreGuard guard(pidMutex, pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "Failed to acquire mutex for save state");
        return false;
    }

    // Get RuntimeStorage instance
    rtstorage::RuntimeStorage* storage = SRP::getRuntimeStorage();
    if (!storage) {
        LOG_WARN(TAG, "RuntimeStorage not available");
        return false;
    }

    // Create PID state structure
    rtstorage::PIDState state;

    // Convert fixed-point values to float for storage
    state.integral = fixedToFloat(stepState.integral);
    state.lastError = static_cast<float>(stepState.previousPV) / 10.0f;  // Store previousPV (reusing field name)
    state.output = 0.0f;  // Not used for fixed-point controller
    state.lastUpdateTime = lastUpdateTime;

    // Calculate CRC (simple checksum for now)
    uint32_t* data = reinterpret_cast<uint32_t*>(&state);
    state.crc = 0;
    for (size_t i = 0; i < (sizeof(state) - sizeof(state.crc)) / sizeof(uint32_t); i++) {
        state.crc ^= data[i];
    }

    // Save to FRAM
    if (storage->savePIDState(controllerId, state)) {
        LOG_INFO(TAG, "PID state saved for controller %d", controllerId);
        return true;
    } else {
        LOG_ERROR(TAG, "Failed to save PID state for controller %d", controllerId);
        return false;
    }
}

bool PIDControlModuleFixedPoint::restoreState(uint8_t controllerId) {
    SemaphoreGuard guard(pidMutex, pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "Failed to acquire mutex for restore state");
        return false;
    }

    // Get RuntimeStorage instance
    rtstorage::RuntimeStorage* storage = SRP::getRuntimeStorage();
    if (!storage) {
        LOG_WARN(TAG, "RuntimeStorage not available");
        return false;
    }

    // Load PID state from FRAM
    rtstorage::PIDState state;
    if (!storage->loadPIDState(controllerId, state)) {
        LOG_WARN(TAG, "No saved PID state for controller %d", controllerId);
        return false;
    }

    // Verify CRC
    uint32_t savedCrc = state.crc;
    state.crc = 0;
    uint32_t* data = reinterpret_cast<uint32_t*>(&state);
    uint32_t calculatedCrc = 0;
    for (size_t i = 0; i < (sizeof(state) - sizeof(state.crc)) / sizeof(uint32_t); i++) {
        calculatedCrc ^= data[i];
    }

    if (calculatedCrc != savedCrc) {
        LOG_WARN(TAG, "PID state CRC mismatch for controller %d (no saved state or corrupted)", controllerId);
        return false;
    }

    // Restore state from float values
    stepState.integral = floatToFixed(state.integral);
    stepState.previousPV = static_cast<Temperature_t>(state.lastError * 10.0f);  // Restore previousPV from storage
    lastUpdateTime = state.lastUpdateTime;
    stepState.firstRun = false;  // We have valid previous state

    // Clamp integral to current limits
    stepState.integral = clamp(stepState.integral, stepLimits.integralMin, stepLimits.integralMax);

    LOG_INFO(TAG, "PID state restored for controller %d (integral=%d, previousPV=%d)",
             controllerId, static_cast<int>(stepState.integral), stepState.previousPV);

    return true;
}
