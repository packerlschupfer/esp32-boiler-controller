// src/diagnostics/DiagnosticsRecoveryTimer.cpp
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "diagnostics/MQTTDiagnostics.h"
#include "LoggingMacros.h"
#include <esp_log.h>

static const char* TAG = "DiagnosticsRecovery";
static TimerHandle_t recoveryTimer = nullptr;

/**
 * @brief Timer callback to restore normal diagnostic operation
 */
static void diagnosticsRecoveryCallback(TimerHandle_t xTimer) {
    LOG_INFO(TAG, "Restoring MQTT diagnostics after memory recovery");
    MQTTDiagnostics::restoreNormalOperation();
    
    // Delete the one-shot timer after use
    if (xTimer != nullptr) {
        xTimerDelete(xTimer, 0);
        recoveryTimer = nullptr;
    }
}

/**
 * @brief Schedule diagnostics restoration after a delay
 * 
 * This creates a one-shot timer that will restore normal diagnostic
 * operation after the specified delay. Used after emergency memory
 * recovery to give the system time to stabilize.
 * 
 * @param delayMs Delay in milliseconds before restoration (default 30000)
 * @return true if timer was created successfully
 */
bool scheduleDiagnosticsRecovery(uint32_t delayMs = 30000) {
    // Cancel any existing timer
    if (recoveryTimer != nullptr) {
        xTimerDelete(recoveryTimer, 0);
        recoveryTimer = nullptr;
    }
    
    // Create a one-shot timer
    recoveryTimer = xTimerCreate(
        "DiagRecovery",           // Timer name
        pdMS_TO_TICKS(delayMs),   // Period
        pdFALSE,                  // One-shot (not auto-reload)
        nullptr,                  // Timer ID (not used)
        diagnosticsRecoveryCallback // Callback function
    );
    
    if (recoveryTimer == nullptr) {
        LOG_ERROR(TAG, "Failed to create diagnostics recovery timer");
        return false;
    }
    
    // Start the timer
    if (xTimerStart(recoveryTimer, 0) != pdPASS) {
        LOG_ERROR(TAG, "Failed to start diagnostics recovery timer");
        xTimerDelete(recoveryTimer, 0);
        recoveryTimer = nullptr;
        return false;
    }
    
    LOG_INFO(TAG, "Diagnostics recovery scheduled in %lu ms", delayMs);
    return true;
}