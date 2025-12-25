// src/modules/control/PIDControlModuleFixedPoint.cpp
#include "modules/control/PIDControlModuleFixedPoint.h"
#include "SemaphoreGuard.h"
#include "LoggingMacros.h"
#include "config/SystemConstants.h"
#include <algorithm>
#include "core/SystemResourceProvider.h"
#include <RuntimeStorage.h>
#include <Arduino.h>  // for millis()

static const char* TAG = "PIDFixedPoint";

PIDControlModuleFixedPoint::PIDControlModuleFixedPoint()
    : integral(0)
    , previousPV(0)
    , firstRun(true)
    , lastUpdateTime(0)
    , integralMin(INTEGRAL_MIN)
    , integralMax(INTEGRAL_MAX)
    , outputMin(OUTPUT_MIN)
    , outputMax(OUTPUT_MAX) {

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
    
    // Calculate error in 0.1°C units
    Temperature_t error = setPoint - currentTemp;
    
    // === Proportional term ===
    // P = Kp * error
    // Since Kp is scaled by PID_FIXED_POINT_SCALE, divide to get actual value
    PIDProduct_t P_raw = static_cast<PIDProduct_t>(Kp) * error;
    PIDValue_t P = static_cast<PIDValue_t>(P_raw / SystemConstants::PID::PID_FIXED_POINT_SCALE);
    
    // === Derivative term (on PV to avoid derivative kick) ===
    // D = -Kd * (currentPV - previousPV) / dt
    // Note: negative because we want D to oppose rapid PV changes
    // Using PV instead of error prevents "derivative kick" on setpoint changes
    PIDValue_t D = 0;
    constexpr auto SCALE = SystemConstants::PID::PID_FIXED_POINT_SCALE;

    if (firstRun) {
        // Skip derivative on first run - no previous PV to compare
        firstRun = false;
    } else {
        // Calculate rate of change of process variable
        Temperature_t pvDelta = currentTemp - previousPV;
        // D = -Kd * pvDelta / dt (negative to oppose PV change)
        PIDProduct_t D_raw = static_cast<PIDProduct_t>(Kd) * (-pvDelta) * SCALE / dtMs;
        D = static_cast<PIDValue_t>(D_raw / SCALE);
    }

    // Store PV for next iteration
    previousPV = currentTemp;

    // === Integral term with anti-windup ===
    // Calculate current I term from existing integral
    PIDProduct_t I_raw = static_cast<PIDProduct_t>(Ki) * integral;
    PIDValue_t I = static_cast<PIDValue_t>(I_raw / SCALE);

    // Calculate tentative output to check for saturation
    PIDValue_t tentativeOutput = P + I + D;
    Temperature_t tentativeAdjustment = static_cast<Temperature_t>(tentativeOutput);

    // Anti-windup: only accumulate integral if output is NOT saturated
    // This prevents integral from winding up when output is at limits
    bool wouldSaturateHigh = (tentativeAdjustment >= outputMax) && (error > 0);
    bool wouldSaturateLow = (tentativeAdjustment <= outputMin) && (error < 0);

    if (!wouldSaturateHigh && !wouldSaturateLow) {
        // Safe to accumulate integral
        PIDProduct_t integralDelta = static_cast<PIDProduct_t>(error) * dtMs;
        integral += static_cast<PIDValue_t>(integralDelta / SCALE);
        integral = clamp(integral, integralMin, integralMax);

        // Recalculate I with updated integral
        I_raw = static_cast<PIDProduct_t>(Ki) * integral;
        I = static_cast<PIDValue_t>(I_raw / SCALE);
    }

    // Update last update time
    lastUpdateTime = millis();

    // === Calculate final output ===
    PIDValue_t output = P + I + D;

    // Convert back to temperature units and apply limits
    Temperature_t tempAdjustment = static_cast<Temperature_t>(output);
    tempAdjustment = clamp(tempAdjustment, outputMin, outputMax);
    
    // Debug logging (comment out in production)
    #ifdef PID_DEBUG
    LOG_DEBUG(TAG, "PID: SP=%d, PV=%d, E=%d, P=%d, I=%d, D=%d, Out=%d",
              setPoint, currentTemp, error, 
              static_cast<int>(P), static_cast<int>(I), static_cast<int>(D),
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

    integral = 0;
    previousPV = 0;
    firstRun = true;  // Skip derivative on first call after reset

    LOG_INFO(TAG, "PID controller reset");
}

void PIDControlModuleFixedPoint::setIntegralLimits(PIDValue_t min, PIDValue_t max) {
    SemaphoreGuard guard(pidMutex, pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "Failed to acquire mutex for setting integral limits");
        return;
    }
    
    integralMin = min;
    integralMax = max;
    
    // Apply limits to current integral
    integral = clamp(integral, integralMin, integralMax);
    
    LOG_INFO(TAG, "Integral limits set: [%d, %d]", 
             static_cast<int>(min), static_cast<int>(max));
}

void PIDControlModuleFixedPoint::setOutputLimits(Temperature_t min, Temperature_t max) {
    SemaphoreGuard guard(pidMutex, pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "Failed to acquire mutex for setting output limits");
        return;
    }
    
    outputMin = min;
    outputMax = max;
    
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
    state.integral = fixedToFloat(integral);
    state.lastError = static_cast<float>(previousPV) / 10.0f;  // Store previousPV (reusing field name)
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
    integral = floatToFixed(state.integral);
    previousPV = static_cast<Temperature_t>(state.lastError * 10.0f);  // Restore previousPV from storage
    lastUpdateTime = state.lastUpdateTime;
    firstRun = false;  // We have valid previous state

    // Clamp integral to current limits
    integral = clamp(integral, integralMin, integralMax);

    LOG_INFO(TAG, "PID state restored for controller %d (integral=%d, previousPV=%d)",
             controllerId, static_cast<int>(integral), previousPV);
    
    return true;
}