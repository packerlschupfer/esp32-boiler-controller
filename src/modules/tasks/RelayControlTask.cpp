// src/modules/tasks/RelayControlTask.cpp
#include "RelayControlTask.h"
#include <MutexGuard.h>  // Include MutexGuard before other headers
#include <RYN4.h>
#include <TaskManager.h>
#include <esp_task_wdt.h>
#include "LoggingMacros.h"
#include "config/ProjectConfig.h"
#include "config/SafetyConfig.h"
#include "config/SystemConstants.h"  // FMEA Round 6: For WDT_RELAY_CONTROL_MS
#include "config/RelayIndices.h"  // For RelayIndex constants
#include "shared/RelayBindings.h"  // For RelayBindings::getStatePtr
#include "ryn4/RelayDefs.h"
#include <SemaphoreGuard.h>
#include <esp_task_wdt.h>
#include "utils/ErrorHandler.h"
#include "utils/LibraryErrorMapper.h"
#include "utils/MutexRetryHelper.h"
#include "core/SystemResourceProvider.h"
#include "core/SharedResourceManager.h"
#include "events/SystemEventsGenerated.h"
#include "shared/RelayFunctionDefs.h"
#include "shared/SharedRelayReadings.h"
#include "modules/control/CentralizedFailsafe.h"


static const char* TAG = "RelayControl";
// No external declarations needed - using SRP methods


// Utility macros (from version 2)
#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif

#ifndef CLAMP
#define CLAMP(x, low, high) (((x) > (high)) ? (high) : (((x) < (low)) ? (low) : (x)))
#endif

// Static member initialization
RYN4* RelayControlTask::ryn4Device = nullptr;
TaskHandle_t RelayControlTask::taskHandle = nullptr;
SemaphoreHandle_t RelayControlTask::taskMutex = nullptr;
bool RelayControlTask::initialized = false;
bool RelayControlTask::running = false;
uint32_t RelayControlTask::commandsProcessed = 0;
uint32_t RelayControlTask::commandsFailed = 0;
TickType_t RelayControlTask::lastCommandTime = 0;
uint32_t RelayControlTask::toggleCount[8] = {0};                    // Added from v2
TickType_t RelayControlTask::toggleTimestamps[8] = {0};            // Added from v2
TickType_t RelayControlTask::rateWindowStart = 0;                  // Added from v2
bool RelayControlTask::currentRelayStates[8] = {false};            // State tracking
std::atomic<bool> RelayControlTask::relayStatesKnown{false};       // Round 20 Issue #6: Made atomic
SemaphoreHandle_t RelayControlTask::relayStateMutex_ = nullptr;    // Round 20 Issue #6: Protects state array
uint8_t RelayControlTask::consecutiveFailures[8] = {0};            // Consecutive failure tracking
TickType_t RelayControlTask::pumpLastStateChangeTime[2] = {0, 0};  // Pump motor protection tracking

// Static buffer for string operations to avoid stack allocation
static char logBuffer[128];  // Max ~80 chars for relay state logging

bool RelayControlTask::init(RYN4* device) {
    if (initialized) {
        LOG_WARN(TAG, "Relay control task already initialized");
        return true;
    }
    
    if (!device) {
        LOG_ERROR(TAG, "Invalid device pointer");
        return false;
    }
    
    // Create mutex
    taskMutex = xSemaphoreCreateMutex();
    if (!taskMutex) {
        LOG_ERROR(TAG, "Failed to create task mutex");
        return false;
    }

    // Round 20 Issue #6: Create mutex for relay state array protection
    relayStateMutex_ = xSemaphoreCreateMutex();
    if (!relayStateMutex_) {
        LOG_ERROR(TAG, "Failed to create relay state mutex");
        vSemaphoreDelete(taskMutex);
        taskMutex = nullptr;
        return false;
    }

    // Initialize rate limiting (from v2)
    rateWindowStart = xTaskGetTickCount();
    memset(toggleCount, 0, sizeof(toggleCount));
    memset(toggleTimestamps, 0, sizeof(toggleTimestamps));
    
    // Store device reference
    ryn4Device = device;
    initialized = true;
    
    LOG_INFO(TAG, "Relay control task initialized");
    return true;
}

bool RelayControlTask::start() {
    if (!initialized) {
        LOG_ERROR(TAG, "Not init");
        return false;
    }
    
    if (running) {
        LOG_WARN(TAG, "Already running");
        return true;
    }
    
    // Create the task using TaskManager with watchdog config
    // Pin to core 1 to avoid conflicts with BLE on core 0
    // FMEA Round 6: Enable watchdog for safety-critical relay control
    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::enabled(
        true,  // Critical - triggers system reset
        SystemConstants::System::WDT_RELAY_CONTROL_MS  // 10s timeout
    );
    
    bool success = SRP::getTaskManager().startTaskPinned(
        taskFunction,
        "RelayControl",
        STACK_SIZE_RELAY_CONTROL_TASK,
        nullptr,
        PRIORITY_RELAY_CONTROL_TASK,
        1,  // Pin to core 1
        wdtConfig
    );
    
    if (success) {
        // Retrieve the task handle after creation
        taskHandle = SRP::getTaskManager().getTaskHandleByName("RelayControl");
        running = true;
        LOG_INFO(TAG, "Started, handle: %p", taskHandle);
    } else {
        LOG_ERROR(TAG, "Start failed");
    }
    
    return success;
}

void RelayControlTask::stop() {
    if (!running || !taskHandle) {
        LOG_WARN(TAG, "Not running");
        return;
    }
    
    running = false;
        
    // Give task time to exit cleanly
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Delete task if it hasn't exited
    if (taskHandle) {
        vTaskDelete(taskHandle);
        taskHandle = nullptr;
    }
    
    LOG_INFO(TAG, "Stopped");
}

void RelayControlTask::taskFunction(void* pvParameters) {
    LOG_INFO(TAG, "RelayControlTask started on core %d - waiting for RYN4", xPortGetCoreID());
    
    // Check if ryn4Device is valid
    if (!ryn4Device) {
        LOG_ERROR(TAG, "ryn4Device is NULL! Cannot proceed");
        vTaskDelete(nullptr);
        return;
    }
    
    LOG_INFO(TAG, "ryn4Device pointer: %p, initialized: %s", 
             (void*)ryn4Device, ryn4Device->isInitialized() ? "YES" : "NO");
    
    // Register with watchdog - this is a safety-critical task controlling physical relays
    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::enabled(
        true,   // critical task - will reset system on timeout
        SystemConstants::System::WDT_RELAY_CONTROL_MS
    );

    if (!SRP::getTaskManager().registerCurrentTaskWithWatchdog("RelayControlTask", wdtConfig)) {
        LOG_ERROR(TAG, "WDT reg failed - entering degraded mode");
        // Critical task without watchdog protection - enter degraded mode
        CentralizedFailsafe::triggerFailsafe(
            CentralizedFailsafe::FailsafeLevel::DEGRADED,
            SystemError::WATCHDOG_INIT_FAILED,
            "RelayControlTask watchdog registration failed"
        );
    } else {
        LOG_INFO(TAG, "WDT OK %lums", SystemConstants::System::WDT_RELAY_CONTROL_MS);
    }
    
    // Wait before first watchdog feed to ensure task is fully initialized
    vTaskDelay(pdMS_TO_TICKS(200));
    (void)SRP::getTaskManager().feedWatchdog();
        
    // Wait for RYN4 initialization with exponential backoff (from v2)
    TickType_t waitTime = pdMS_TO_TICKS(1000);  // Start with 1 second
    const TickType_t maxWaitTime = pdMS_TO_TICKS(30000);  // Max 30 seconds
    bool waitLogged = false;
    
    while (!ryn4Device->isInitialized() && running) {
        if (!waitLogged) {
            LOG_INFO(TAG, "Wait RYN4...");
            waitLogged = true;
        }
        
        // Break up the wait time into smaller chunks to feed watchdog
        TickType_t remainingWait = waitTime;
        const TickType_t maxChunkWait = pdMS_TO_TICKS(1000); // Max 1 second chunks
        
        while (remainingWait > 0 && running && !ryn4Device->isInitialized()) {
            TickType_t chunkWait = MIN(remainingWait, maxChunkWait);
            vTaskDelay(chunkWait);
            remainingWait -= chunkWait;
            (void)SRP::getTaskManager().feedWatchdog();  // Feed watchdog after each chunk
        }
        
        if (ryn4Device->isInitialized()) {
            break; // Exit if initialized
        }
        
        // Exponential backoff
        waitTime = MIN(waitTime * 2, maxWaitTime);
        
        // Log periodically
        static TickType_t lastLogTime = 0;
        TickType_t now = xTaskGetTickCount();
        if (now - lastLogTime > pdMS_TO_TICKS(10000)) {  // Log every 10 seconds
            LOG_WARN(TAG, "Still waiting for RYN4 initialization (next check in %d seconds)...",
                     pdTICKS_TO_MS(waitTime) / 1000);
            lastLogTime = now;
        }
    }
    
    if (!running) {
        LOG_INFO(TAG, "Task stopped before initialization complete");
        taskHandle = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    
    LOG_INFO(TAG, "RYN4 initialized, starting command processing");
    
    // Read initial relay states
    LOG_INFO(TAG, "Reading initial relay states...");
    
    // Log stack usage before getData call
    UBaseType_t stackHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
    LOG_INFO(TAG, "Stack high water mark before getData: %d words", stackHighWaterMark);
    
    auto stateResult = ryn4Device->getData(IDeviceInstance::DeviceDataType::RELAY_STATE);
    
    // Log stack usage after getData call
    stackHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
    LOG_INFO(TAG, "Stack high water mark after getData: %d words", stackHighWaterMark);
    
    if (stateResult.isOk() && stateResult.value().size() >= 8) {
        // Round 20 Issue #6: Protect state array access with mutex
        if (xSemaphoreTake(relayStateMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
            for (int i = 0; i < 8; i++) {
                currentRelayStates[i] = stateResult.value()[i] > 0.5f;
                LOG_INFO(TAG, "Initial state - Relay %d: %s",
                         i + 1, currentRelayStates[i] ? "ON" : "OFF");
            }
            xSemaphoreGive(relayStateMutex_);
        }
        relayStatesKnown.store(true);
        
        // Set relay status synchronized bit after successful initial read
        EventGroupHandle_t relayStatusEventGroup = SRP::getRelayStatusEventGroup();
        if (relayStatusEventGroup) {
            xEventGroupSetBits(relayStatusEventGroup, SystemEvents::RelayStatus::SYNCHRONIZED | SystemEvents::RelayStatus::COMM_OK);
            LOG_INFO(TAG, "Relay status synchronized");
        }
    } else {
        LOG_ERROR(TAG, "Failed to read initial relay states - isOk: %s, size: %d",
                 stateResult.isOk() ? "true" : "false",
                 stateResult.isOk() ? stateResult.value().size() : 0);
    }
    
    // Main task loop - event-driven without polling
    while (running) {
        // Feed watchdog at start of loop
        (void)SRP::getTaskManager().feedWatchdog();
        
        // Update rate limiting counters periodically (from v2)
        updateRateLimitCounters();
        
        // Monitor system state
        monitorSystemState();
        
        // Wait for relay requests with timeout for watchdog feeding
        // This will block until an event occurs or timeout expires
        waitForRelayRequests();
        
        // Process any pending requests
        processRelayRequests();
    }
    
    LOG_INFO(TAG, "End P:%lu F:%lu",
             commandsProcessed, commandsFailed);
    
    // Clean up
    taskHandle = nullptr;
    vTaskDelete(nullptr);
}

bool RelayControlTask::processSingleRelay(uint8_t relayIndex, bool state) {
    // Check if device is offline
    if (!ryn4Device) {
        LOG_ERROR(TAG, "Cannot control relay - ryn4Device is NULL!");
        return false;
    }
    
    LOG_DEBUG(TAG, "ryn4Device pointer: %p", (void*)ryn4Device);
    
    if (ryn4Device->isModuleOffline()) {
        LOG_ERROR(TAG, "Cannot control relay - device is offline");
        return false;
    }
    
    // Check if device is initialized
    if (!ryn4Device->isInitialized()) {
        LOG_ERROR(TAG, "Cannot control relay - device not initialized");
        return false;
    }
    
    // Always check rate limit for relay protection (no longer conditional)
    if (!checkRateLimit(relayIndex)) {
        LOG_WARN(TAG, "Rate limit exceeded for relay %d", relayIndex);
        return false;
    }

    // Check pump motor protection for relays 1 and 2 (heating and water pumps)
    // This prevents rapid on/off cycling that can damage pump motors
    if (!checkPumpProtection(relayIndex, state)) {
        // Pump protection blocks this state change - not an error, just too soon
        return false;
    }

    LOG_INFO(TAG, "Setting relay %d to %s", relayIndex, state ? "ON" : "OFF");
    
    // Get the update event group from RYN4
    EventGroupHandle_t updateEventGroup = ryn4Device->getUpdateEventGroup();
    if (!updateEventGroup) {
        LOG_ERROR(TAG, "Failed to get update event group");
        return false;
    }
    
    // Clear the specific relay update bit before sending command
    if (relayIndex >= 1 && relayIndex <= 8) {
        uint32_t relayUpdateBit = ryn4::RELAY_UPDATE_BITS[relayIndex - 1];
        xEventGroupClearBits(updateEventGroup, relayUpdateBit);
        
        #if defined(LOG_MODE_DEBUG_FULL)
        LOG_DEBUG(TAG, "Cleared update bit 0x%08X for relay %d", 
                 relayUpdateBit, relayIndex);
        #endif
    }
    
    // Acquire device mutex using RAII
    LOG_DEBUG(TAG, "Attempting to acquire device mutex...");
    auto guard = MutexRetryHelper::acquireGuard(ryn4Device->getMutexInterface(), "RYN4-SetRelay", pdMS_TO_TICKS(1000));
    if (!guard) {
        LOG_ERROR(TAG, "Failed to acquire device mutex");
        return false;
    }
    LOG_DEBUG(TAG, "Device mutex acquired successfully");

    // Note: In RYN4 terminology: OPEN = relay ON (energized), CLOSE = relay OFF (de-energized)
    ryn4::RelayAction action = state ? ryn4::RelayAction::OPEN : ryn4::RelayAction::CLOSE;

    LOG_DEBUG(TAG, "About to call controlRelayVerified(%d, %s) on device %p",
             relayIndex, state ? "OPEN" : "CLOSE", (void*)ryn4Device);

    // Use verified relay control - confirms state change via readback
    ryn4::RelayErrorCode result = ryn4Device->controlRelayVerified(relayIndex, action);
    // Mutex released automatically when guard goes out of scope

    LOG_DEBUG(TAG, "controlRelayVerified returned: %d", static_cast<int>(result));

    if (result == ryn4::RelayErrorCode::SUCCESS) {
        #ifdef ENABLE_RELAY_EVENT_LOGGING
        LOG_DEBUG(TAG, "[CMD] Relay %d: %s (verified)", relayIndex, state ? "OPEN" : "CLOSE");
        #endif

        // controlRelayVerified already verified the state, update our tracking
        // Round 20 Issue #6: Protect state array access with mutex
        if (xSemaphoreTake(relayStateMutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
            currentRelayStates[relayIndex - 1] = state;
            xSemaphoreGive(relayStateMutex_);
        }
        relayStatesKnown.store(true);

        // Update pump protection timestamp for relays 1 and 2 (heating and water pumps)
        if (relayIndex >= 1 && relayIndex <= 2) {
            pumpLastStateChangeTime[relayIndex - 1] = xTaskGetTickCount();
            LOG_DEBUG(TAG, "Pump %d protection timer reset (30s until next change allowed)", relayIndex);
        }

        // Immediately update SharedRelayReadings - moved before logging for faster update
        updateSharedRelayReadings(relayIndex, state);

        // Increment success counter
        commandsProcessed++;
        lastCommandTime = xTaskGetTickCount();

        // Track success for health monitoring
        checkRelayHealthAndEscalate(relayIndex, true);

        LOG_DEBUG(TAG, "Relay %d state verified and command completed", relayIndex);
        return true;
    } else if (result == ryn4::RelayErrorCode::TIMEOUT) {
        commandsFailed++;
        LOG_WARN(TAG, "Relay %d verification timeout", relayIndex);

        // SM-CRIT-3: Check if this was burner OFF command - safety critical
        // Must escalate immediately, not wait for consecutive failures
        if ((relayIndex - 1) == RelayIndex::BURNER_ENABLE && !state) {
            LOG_ERROR(TAG, "CRITICAL: Single burner OFF verification timeout!");
            CentralizedFailsafe::triggerFailsafe(
                CentralizedFailsafe::FailsafeLevel::CRITICAL,
                SystemError::RELAY_VERIFICATION_FAILED,
                "Single burner relay OFF timeout"
            );
        }

        // Track failure for health monitoring and potential escalation
        checkRelayHealthAndEscalate(relayIndex, false);
        return false;
    } else {
        commandsFailed++;
        LOG_ERROR(TAG, "Relay %d control failed: %d", relayIndex, static_cast<int>(result));

        // SM-CRIT-3: Check if this was burner OFF command - safety critical
        if ((relayIndex - 1) == RelayIndex::BURNER_ENABLE && !state) {
            LOG_ERROR(TAG, "CRITICAL: Single burner OFF command failed!");
            CentralizedFailsafe::triggerFailsafe(
                CentralizedFailsafe::FailsafeLevel::CRITICAL,
                SystemError::RELAY_VERIFICATION_FAILED,
                "Single burner relay OFF failed"
            );
        }

        // Track failure for health monitoring and potential escalation
        checkRelayHealthAndEscalate(relayIndex, false);
        return false;
    }
}

// NEW METHOD: Process toggle relay (from v2)
bool RelayControlTask::processToggleRelay(uint8_t relayIndex) {
    // Get current state
    auto result = ryn4Device->getData(
        IDeviceInstance::DeviceDataType::RELAY_STATE);
    
    if (!result.isOk() || result.value().size() < relayIndex) {
        LOG_ERROR(TAG, "Failed to get current state for toggle");
        return false;
    }
    
    bool currentState = result.value()[relayIndex - 1] > 0.5f;
    // Use the updated processSingleRelay which now handles confirmation
    return processSingleRelay(relayIndex, !currentState);
}

// NEW METHOD: Process set all relays (from v2)
bool RelayControlTask::processSetAllRelays(bool state) {
    LOG_INFO(TAG, "Setting all relays to %s", state ? "ON" : "OFF");

    std::array<bool, 8> states;
    for (int i = 0; i < 8; i++) {
        states[i] = state;
    }

    return processSetMultipleRelays(states);
}

// NEW METHOD: Process set multiple relays with smart update detection (from v2)
bool RelayControlTask::processSetMultipleRelays(const std::array<bool, 8>& states) {
    // Get the update event group
    EventGroupHandle_t updateEventGroup = ryn4Device->getUpdateEventGroup();
    if (!updateEventGroup) {
        LOG_ERROR(TAG, "Failed to get update event group");
        return false;
    }

    // Use the global constant for all relay update bits

    // Clear all relay update bits before sending command
    xEventGroupClearBits(updateEventGroup, SRP::getRelayAllUpdateBits());

    // Get current states to determine which relays will actually change
    EventBits_t expectedChangeBits = 0;
    auto currentStates = ryn4Device->getData(
        IDeviceInstance::DeviceDataType::RELAY_STATE);

    if (currentStates.isOk() && currentStates.value().size() == 8) {
        // Build mask of only relays that will change
        for (size_t i = 0; i < 8; i++) {
            bool currentState = currentStates.value()[i] > 0.5f;
            if (currentState != states[i]) {
                expectedChangeBits |= ryn4::RELAY_UPDATE_BITS[i];
                #if defined(LOG_MODE_DEBUG_FULL)
                LOG_DEBUG(TAG, "Relay %zu will change: %s -> %s",
                         i + 1, currentState ? "ON" : "OFF", states[i] ? "ON" : "OFF");
                #endif
            }
        }

        // If no changes expected, we're done
        if (expectedChangeBits == 0) {
            LOG_INFO(TAG, "No relay state changes needed");
            return true;
        }
    } else {
        // If we can't get current state, expect all relays might update
        expectedChangeBits = relayAllUpdateBits;
        LOG_DEBUG(TAG, "Could not get current states, expecting all relays to update");
    }
    
    // Acquire device mutex using RAII
    auto guard = MutexRetryHelper::acquireGuard(ryn4Device->getMutexInterface(), "RYN4-SetMultiple", pdMS_TO_TICKS(1000));
    if (!guard) {
        LOG_ERROR(TAG, "Failed to acquire device mutex");
        return false;
    }

    // Use verified version - confirms state changes via readback
    ryn4::RelayErrorCode result = ryn4Device->setMultipleRelayStatesVerified(states);
    // Mutex released automatically when guard goes out of scope

    if (result == ryn4::RelayErrorCode::SUCCESS) {
        #ifdef ENABLE_RELAY_EVENT_LOGGING
        // Use static buffer for logging to avoid stack allocation
        int offset = snprintf(logBuffer, sizeof(logBuffer), "[CMD] Multi: ");
        for (size_t i = 0; i < states.size() && offset < sizeof(logBuffer) - 10; i++) {
            offset += snprintf(logBuffer + offset, sizeof(logBuffer) - offset,
                             "R%zu:%s ", i + 1, states[i] ? "ON" : "OFF");
        }
        LOG_INFO(TAG, "%s (verified)", logBuffer);
        #endif

        // Update state tracking for all relays
        // Round 20 Issue #6: Protect state array access with mutex
        if (xSemaphoreTake(relayStateMutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
            for (size_t i = 0; i < states.size() && i < 8; i++) {
                currentRelayStates[i] = states[i];
            }
            xSemaphoreGive(relayStateMutex_);
        }
        relayStatesKnown.store(true);

        // Update SharedRelayReadings for all changed relays
        for (size_t i = 0; i < states.size() && i < 8; i++) {
            updateSharedRelayReadings(i + 1, states[i]);
        }

        LOG_DEBUG(TAG, "Multi-relay batch command verified");
        return true;
    } else if (result == ryn4::RelayErrorCode::TIMEOUT) {
        LOG_WARN(TAG, "Multi-relay verification timeout");
        // H5: Check if burner relay was being turned OFF - this is safety-critical
        if (!states[RelayIndex::BURNER_ENABLE]) {
            LOG_ERROR(TAG, "CRITICAL: Burner OFF command verification timeout - possible stuck relay!");
            CentralizedFailsafe::triggerFailsafe(
                CentralizedFailsafe::FailsafeLevel::CRITICAL,
                SystemError::RELAY_VERIFICATION_FAILED,
                "Burner relay OFF verification timeout"
            );
        }
        return false;
    } else {
        LOG_ERROR(TAG, "Failed to set multiple relays: error %d",
                 static_cast<int>(result));
        // H5: Check if burner relay was being turned OFF - this is safety-critical
        if (!states[RelayIndex::BURNER_ENABLE]) {
            LOG_ERROR(TAG, "CRITICAL: Burner OFF command failed - possible stuck relay!");
            CentralizedFailsafe::triggerFailsafe(
                CentralizedFailsafe::FailsafeLevel::CRITICAL,
                SystemError::RELAY_VERIFICATION_FAILED,
                "Burner relay OFF command failed"
            );
        }
        return false;
    }
}

// NEW METHOD: Process toggle all relays (from v2)
bool RelayControlTask::processToggleAllRelays() {
    // Get current states
    auto result = ryn4Device->getData(
        IDeviceInstance::DeviceDataType::RELAY_STATE);

    if (!result.isOk() || result.value().size() != 8) {
        LOG_ERROR(TAG, "Failed to get current states for toggle all");
        return false;
    }

    // Create inverted states array
    std::array<bool, 8> newStates;

    for (size_t i = 0; i < 8; i++) {
        newStates[i] = !(result.value()[i] > 0.5f);
    }

    return processSetMultipleRelays(newStates);
}

// NEW METHOD: Check rate limit (from v2)
bool RelayControlTask::checkRateLimit(uint8_t relayIndex) {
    if (relayIndex < 1 || relayIndex > 8) {
        return false;
    }
    
    uint8_t idx = relayIndex - 1;
    TickType_t now = xTaskGetTickCount();
    
    // Check minimum interval
    if (toggleTimestamps[idx] != 0) {
        TickType_t elapsed = now - toggleTimestamps[idx];
        if (elapsed < pdMS_TO_TICKS(MIN_RELAY_SWITCH_INTERVAL_MS)) {
            return false;
        }
    }
    
    // Check rate limit
    if (toggleCount[idx] >= MAX_RELAY_TOGGLE_RATE_PER_MIN) {
        return false;
    }
    
    // Update tracking
    toggleTimestamps[idx] = now;
    toggleCount[idx]++;
    
    return true;
}

// NEW METHOD: Update rate limit counters (from v2)
void RelayControlTask::updateRateLimitCounters() {
    TickType_t now = xTaskGetTickCount();
    TickType_t elapsed = now - rateWindowStart;

    // Reset counters every minute
    if (elapsed >= pdMS_TO_TICKS(60000)) {
        memset(toggleCount, 0, sizeof(toggleCount));
        rateWindowStart = now;
        #if defined(LOG_MODE_DEBUG_FULL)
        LOG_DEBUG(TAG, "Rate limit counters reset");
        #endif
    }
}

// Pump motor protection - check if configurable time has elapsed since last state change
// Returns true if state change is allowed, false if blocked by protection
bool RelayControlTask::checkPumpProtection(uint8_t relayIndex, bool desiredState) {
    // Only applies to relay 1 (heating pump) and relay 2 (water pump)
    if (relayIndex < 1 || relayIndex > 2) {
        return true;  // Not a pump relay, no protection needed
    }

    uint8_t pumpIdx = relayIndex - 1;  // 0 for heating, 1 for water
    TickType_t now = xTaskGetTickCount();

    // If this is the first state change (timestamp is 0), allow it
    if (pumpLastStateChangeTime[pumpIdx] == 0) {
        return true;
    }

    // Check if relay is already in desired state - no protection needed
    // Round 20 Issue #6: Protect state array read with mutex
    if (relayStatesKnown.load()) {
        bool currentState = false;
        if (xSemaphoreTake(relayStateMutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
            currentState = currentRelayStates[relayIndex - 1];
            xSemaphoreGive(relayStateMutex_);
            if (currentState == desiredState) {
                return true;  // No actual state change, protection doesn't apply
            }
        }
    }

    // Calculate elapsed time since last state change
    TickType_t elapsed = now - pumpLastStateChangeTime[pumpIdx];
    uint32_t elapsedMs = pdTICKS_TO_MS(elapsed);

    if (elapsedMs < SafetyConfig::pumpProtectionMs) {
        // Block the state change - motor protection
        uint32_t remainingMs = SafetyConfig::pumpProtectionMs - elapsedMs;

        static TickType_t lastBlockLog[2] = {0, 0};
        if (now - lastBlockLog[pumpIdx] > pdMS_TO_TICKS(5000)) {  // Log every 5s max
            LOG_WARN(TAG, "Pump %d state change blocked by motor protection - %lu ms remaining",
                     relayIndex, remainingMs);
            lastBlockLog[pumpIdx] = now;
        }
        return false;
    }

    return true;  // Protection period has elapsed, allow state change
}

// Get time remaining until pump can change state (for debugging/status)
uint32_t RelayControlTask::getPumpProtectionTimeRemaining(uint8_t relayIndex) {
    if (relayIndex < 1 || relayIndex > 2) {
        return 0;  // Not a pump relay
    }

    uint8_t pumpIdx = relayIndex - 1;

    if (pumpLastStateChangeTime[pumpIdx] == 0) {
        return 0;  // No protection active yet
    }

    TickType_t now = xTaskGetTickCount();
    TickType_t elapsed = now - pumpLastStateChangeTime[pumpIdx];
    uint32_t elapsedMs = pdTICKS_TO_MS(elapsed);

    if (elapsedMs >= SafetyConfig::pumpProtectionMs) {
        return 0;  // Protection period has elapsed
    }

    return SafetyConfig::pumpProtectionMs - elapsedMs;
}

void RelayControlTask::waitForRelayRequests() {
    // Get relay request event group
    auto& resourceManager = SharedResourceManager::getInstance();
    EventGroupHandle_t relayRequestEventGroup = resourceManager.getEventGroup(SharedResourceManager::EventGroups::RELAY_REQUEST);
    
    if (!relayRequestEventGroup) {
        // If no event group, just delay to prevent tight loop
        vTaskDelay(pdMS_TO_TICKS(1000));
        return;
    }
    
    // Wait for ANY relay request bit with a timeout for watchdog feeding
    // This blocks the task until a relay request is made or timeout occurs
    const EventBits_t ALL_RELAY_REQUEST_BITS = 0x00FFFFFF;  // Mask for bits 0-23
    const TickType_t WATCHDOG_TIMEOUT = pdMS_TO_TICKS(3000); // 3 second timeout for watchdog (must be less than WDT timeout)
    
    EventBits_t bits = xEventGroupWaitBits(
        relayRequestEventGroup,
        ALL_RELAY_REQUEST_BITS,  // Wait for any relay request bit
        pdFALSE,                 // Don't clear on exit (processRelayRequests will clear)
        pdFALSE,                 // Wait for ANY bit (not all)
        WATCHDOG_TIMEOUT         // Block for up to 3 seconds
    );
    
    if (bits != 0) {
        LOG_DEBUG(TAG, "Relay request event received: 0x%08X", bits);
    }
    // If timeout (bits == 0), that's fine - we'll feed watchdog and check again
}

void RelayControlTask::monitorSystemState() {
    // Get shared resources from SharedResourceManager
    auto& resourceManager = SharedResourceManager::getInstance();
    EventGroupHandle_t systemStateEventGroup = resourceManager.getEventGroup(SharedResourceManager::EventGroups::SYSTEM_STATE);
    EventGroupHandle_t heatingEventGroup = resourceManager.getEventGroup(SharedResourceManager::EventGroups::HEATING);
    EventGroupHandle_t controlRequestsEventGroup = resourceManager.getEventGroup(SharedResourceManager::EventGroups::CONTROL_REQUESTS);
    
    if (!systemStateEventGroup || !heatingEventGroup || !controlRequestsEventGroup) {
        return;
    }
    
    // Check system state bits
    EventBits_t systemStateBits = xEventGroupGetBits(systemStateEventGroup);
    
    // ============================================================================
    // THREAD-SAFETY NOTE (Round 14 Issue #4):
    // Static variables below are SAFE because monitorSystemState() is only called
    // from RelayControlTask::taskFunction() (single task context).
    // DO NOT call this function from other tasks.
    // ============================================================================

    // Monitor mode changes
    static uint8_t lastMode = 0xFF;
    uint8_t currentMode = 0;
    
    if (systemStateBits & SystemEvents::SystemState::HEATING_ON) {
        currentMode = 1;  // Heating mode
    } else if (systemStateBits & SystemEvents::SystemState::WATER_ON) {
        currentMode = 2;  // Water mode
    }
    
    if (currentMode != lastMode) {
        #if defined(LOG_MODE_DEBUG_SELECTIVE) || defined(LOG_MODE_DEBUG_FULL)
        LOG_INFO(TAG, "System mode changed: %s", 
                 currentMode == 1 ? "HEATING" : (currentMode == 2 ? "WATER" : "OFF"));
        #endif
        lastMode = currentMode;
    }
    
    // Monitor burner state
    static bool lastBurnerActive = false;
    bool burnerActive = (systemStateBits & SystemEvents::SystemState::BURNER_ON) != 0;
    
    if (burnerActive != lastBurnerActive) {
        #if defined(LOG_MODE_DEBUG_SELECTIVE) || defined(LOG_MODE_DEBUG_FULL)
        LOG_INFO(TAG, "Burner state changed: %s", burnerActive ? "ACTIVE" : "INACTIVE");
        #endif
        lastBurnerActive = burnerActive;
    }
}

void RelayControlTask::processRelayRequests() {
    // Get relay request event group
    auto& resourceManager = SharedResourceManager::getInstance();
    EventGroupHandle_t relayRequestEventGroup = resourceManager.getEventGroup(SharedResourceManager::EventGroups::RELAY_REQUEST);
    
    if (!relayRequestEventGroup) {
        LOG_ERROR(TAG, "Failed to get relay request event group!");
        return;
    }
    
    // Get current state of all request bits without waiting
    // We already waited in waitForRelayRequests(), so just get current state
    EventBits_t requestBits = xEventGroupGetBits(relayRequestEventGroup);
    
    // Mask to only process valid bits (FreeRTOS event groups only support 24 bits)
    const EventBits_t ALL_RELAY_REQUEST_BITS = 0x00FFFFFF;  // Mask for bits 0-23
    requestBits &= ALL_RELAY_REQUEST_BITS;
    
    if (requestBits == 0) {
        return;  // No requests pending
    }
    
    LOG_DEBUG(TAG, "processRelayRequests: Got request bits: 0x%08X", requestBits);
    
    // Process heating pump requests
    if (requestBits & SystemEvents::RelayRequest::HEATING_PUMP_ON) {
        LOG_DEBUG(TAG, "Processing heating pump ON request");
        if (setRelayState(RelayFunction::HEATING_PUMP, true)) {
            xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::HEATING_PUMP_ON);
        }
    }
    
    if (requestBits & SystemEvents::RelayRequest::HEATING_PUMP_OFF) {
        LOG_DEBUG(TAG, "Processing heating pump OFF request");
        if (setRelayState(RelayFunction::HEATING_PUMP, false)) {
            xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::HEATING_PUMP_OFF);
        }
    }
    
    // Process water pump requests
    if (requestBits & SystemEvents::RelayRequest::WATER_PUMP_ON) {
        LOG_INFO(TAG, "Processing water pump ON request for relay %d", RelayFunction::WATER_PUMP);
        
        // Log current state before attempting change
        // Round 20 Issue #6: Protect state array read with mutex
        if (relayStatesKnown.load() && xSemaphoreTake(relayStateMutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
            LOG_INFO(TAG, "Current water pump state before command: %s",
                     currentRelayStates[RelayFunction::WATER_PUMP - 1] ? "ON" : "OFF");
            xSemaphoreGive(relayStateMutex_);
        }
        
        if (setRelayState(RelayFunction::WATER_PUMP, true)) {
            xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::WATER_PUMP_ON);
            LOG_INFO(TAG, "Water pump ON request completed");
        } else {
            LOG_ERROR(TAG, "Water pump ON request FAILED");
        }
    }
    
    if (requestBits & SystemEvents::RelayRequest::WATER_PUMP_OFF) {
        LOG_DEBUG(TAG, "Processing water pump OFF request");
        if (setRelayState(RelayFunction::WATER_PUMP, false)) {
            xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::WATER_PUMP_OFF);
        }
    }
    
    // Process burner requests
    if (requestBits & SystemEvents::RelayRequest::BURNER_ENABLE) {
        LOG_DEBUG(TAG, "Processing burner ON request");
        if (setRelayState(RelayFunction::BURNER_ENABLE, true)) {
            xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::BURNER_ENABLE);
        }
    }
    
    if (requestBits & SystemEvents::RelayRequest::BURNER_DISABLE) {
        LOG_DEBUG(TAG, "Processing burner OFF request");
        if (setRelayState(RelayFunction::BURNER_ENABLE, false)) {
            xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::BURNER_DISABLE);
        }
    }
    
    // Process power select requests
    if (requestBits & SystemEvents::RelayRequest::POWER_HALF) {
        LOG_DEBUG(TAG, "Processing half power request");
        if (setRelayState(RelayFunction::POWER_SELECT, true)) {
            xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::POWER_HALF);
        }
    }
    
    if (requestBits & SystemEvents::RelayRequest::POWER_FULL) {
        LOG_DEBUG(TAG, "Processing full power request");
        if (setRelayState(RelayFunction::POWER_SELECT, false)) {
            xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::POWER_FULL);
        }
    }
    
    // Process water mode requests
    if (requestBits & SystemEvents::RelayRequest::WATER_MODE_ON) {
        LOG_DEBUG(TAG, "Processing water mode ON request");
        if (setRelayState(RelayFunction::WATER_MODE, true)) {
            xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::WATER_MODE_ON);
        }
    }
    
    if (requestBits & SystemEvents::RelayRequest::WATER_MODE_OFF) {
        LOG_DEBUG(TAG, "Processing water mode OFF request");
        if (setRelayState(RelayFunction::WATER_MODE, false)) {
            xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::WATER_MODE_OFF);
        }
    }
}

// Keep all the existing public methods below...

bool RelayControlTask::setRelayState(uint8_t relayIndex, bool state) {
    LOG_DEBUG(TAG, "setRelayState called: relay=%d, state=%s", relayIndex, state ? "ON" : "OFF");

    if (!initialized || !ryn4Device) {
        LOG_ERROR(TAG, "Task not initialized: init=%d, device=%p",
                  initialized, (void*)ryn4Device);
        return false;
    }

    if (relayIndex < 1 || relayIndex > 8) {
        LOG_ERROR(TAG, "Invalid relay index: %d", relayIndex);
        return false;
    }
    
    // Check if we know the current state and if it's already in the desired state
    // H2: Check relayStatesKnown INSIDE mutex to prevent race where flag changes
    // between the check and array access
    bool skipCommand = false;
    if (xSemaphoreTake(relayStateMutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
        // Now atomically check flag AND read array under same mutex
        if (relayStatesKnown.load()) {
            bool currentState = currentRelayStates[relayIndex - 1];
            LOG_DEBUG(TAG, "State check: known=true, current[%d]=%s, desired=%s",
                      relayIndex - 1, currentState ? "ON" : "OFF", state ? "ON" : "OFF");
            if (currentState == state) {
                skipCommand = true;
            }
        }
        xSemaphoreGive(relayStateMutex_);
    }

    if (skipCommand) {
        LOG_DEBUG(TAG, "Relay %d already in desired state (%s), skipping command",
                  relayIndex, state ? "ON" : "OFF");
        return true;  // Already in desired state, no action needed
    }
    
    // Directly process the relay command using the verified method
    LOG_INFO(TAG, "Setting relay %d to %s (direct call)", relayIndex, state ? "ON" : "OFF");
    return processSingleRelay(relayIndex, state);
}

bool RelayControlTask::setAllRelays(bool state) {
    if (!initialized || !ryn4Device) {
        LOG_ERROR(TAG, "Task not initialized");
        return false;
    }
    
    // Directly process the command without queue
    LOG_INFO(TAG, "Setting all relays to %s (direct call)", state ? "ON" : "OFF");
    return processSetAllRelays(state);
}

bool RelayControlTask::setMultipleRelays(const std::array<bool, 8>& states) {
    if (!initialized || !ryn4Device) {
        LOG_ERROR(TAG, "Task not initialized");
        return false;
    }

    // Directly process the command without queue
    LOG_INFO(TAG, "Setting multiple relays (direct call)");
    return processSetMultipleRelays(states);
}

bool RelayControlTask::toggleRelay(uint8_t relayIndex) {
    if (!initialized || !ryn4Device) {
        LOG_ERROR(TAG, "Task not initialized");
        return false;
    }

    if (relayIndex < 1 || relayIndex > 8) {
        LOG_ERROR(TAG, "Invalid relay index: %d", relayIndex);
        return false;
    }
    
    // Directly process the command without queue
    LOG_INFO(TAG, "Toggling relay %d (direct call)", relayIndex);
    return processToggleRelay(relayIndex);
}

bool RelayControlTask::toggleAllRelays() {
    if (!initialized || !ryn4Device) {
        LOG_ERROR(TAG, "Task not initialized");
        return false;
    }
    
    // Directly process the command without queue
    LOG_INFO(TAG, "Toggling all relays (direct call)");
    return processToggleAllRelays();
}

bool RelayControlTask::isRunning() {
    return running && taskHandle != nullptr;
}

TaskHandle_t RelayControlTask::getTaskHandle() {
    return taskHandle;
}

void RelayControlTask::getStatistics(uint32_t& processed, uint32_t& failed) {
    // Check if mutex is initialized
    if (!taskMutex) {
        processed = 0;
        failed = 0;
        return;
    }
    
    auto guard = MutexRetryHelper::acquireGuard(taskMutex, "RelayTask-GetStats", pdMS_TO_TICKS(100));
    if (guard) {
        processed = commandsProcessed;
        failed = commandsFailed;
    } else {
        processed = 0;
        failed = 0;
    }
}

// Helper method to update SharedRelayReadings immediately
// NOTE: With unified mapping, RYN4 library writes DIRECTLY to SharedRelayReadings
// via bound pointers. This function is now redundant but kept for explicit updates.
void RelayControlTask::updateSharedRelayReadings(uint8_t relayIndex, bool state) {
    // Relay index is 1-based, convert to 0-based for array access
    if (relayIndex < 1 || relayIndex > 8) {
        return;
    }

    uint8_t arrayIndex = relayIndex - 1;
    bool* statePtr = RelayBindings::getStatePtr(arrayIndex);

    if (statePtr != nullptr && SRP::takeRelayReadingsMutex(pdMS_TO_TICKS(100))) {
        *statePtr = state;
        SRP::giveRelayReadingsMutex();
        
        // Also set the relay event bit to notify other tasks
        SRP::setRelayEventBits(SystemEvents::RelayControl::DATA_AVAILABLE);
        
        // Set relay status synchronized bit to indicate communication is OK
        EventGroupHandle_t relayStatusEventGroup = SRP::getRelayStatusEventGroup();
        if (relayStatusEventGroup) {
            xEventGroupSetBits(relayStatusEventGroup, SystemEvents::RelayStatus::SYNCHRONIZED | SystemEvents::RelayStatus::COMM_OK);
        }
        
        LOG_DEBUG(TAG, "SharedRelayReadings updated for relay %d = %s",
                 relayIndex, state ? "ON" : "OFF");
    }
}

void RelayControlTask::checkRelayHealthAndEscalate(uint8_t relayIndex, bool success) {
    if (relayIndex < 1 || relayIndex > 8) {
        return;
    }

    uint8_t idx = relayIndex - 1;

    if (success) {
        // Reset consecutive failure counter on success
        if (consecutiveFailures[idx] > 0) {
            LOG_INFO(TAG, "Relay %d recovered after %d failures", relayIndex, consecutiveFailures[idx]);
            consecutiveFailures[idx] = 0;
        }
    } else {
        // Increment failure counter
        consecutiveFailures[idx]++;

        LOG_WARN(TAG, "Relay %d consecutive failures: %d/%d",
                 relayIndex, consecutiveFailures[idx], MAX_CONSECUTIVE_FAILURES);

        // Check if escalation threshold reached
        if (consecutiveFailures[idx] >= MAX_CONSECUTIVE_FAILURES) {
            LOG_ERROR(TAG, "CRITICAL: Relay %d failed %d consecutive times - escalating to failsafe",
                     relayIndex, consecutiveFailures[idx]);

            // Set relay error bit
            xEventGroupSetBits(SRP::getErrorNotificationEventGroup(), SystemEvents::Error::RELAY);

            // FMEA Round 6: Use CRITICAL level for burner relay (triggers emergency shutdown)
            // Other relays use WARNING level (triggers monitoring only)
            // idx is array index (0-7), RelayIndex::BURNER_ENABLE is array index 0
            CentralizedFailsafe::FailsafeLevel level =
                (idx == RelayIndex::BURNER_ENABLE)
                    ? CentralizedFailsafe::FailsafeLevel::CRITICAL
                    : CentralizedFailsafe::FailsafeLevel::WARNING;

            CentralizedFailsafe::triggerFailsafe(
                level,
                SystemError::RELAY_OPERATION_FAILED,
                (idx == RelayIndex::BURNER_ENABLE)
                    ? "BURNER relay verification failed - emergency shutdown"
                    : "Relay verification failed repeatedly"
            );

            // Reset counter to avoid repeated escalations
            // Next failure will start counting again
            consecutiveFailures[idx] = 0;
        }
    }
}
