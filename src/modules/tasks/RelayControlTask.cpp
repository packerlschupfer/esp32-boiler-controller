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
#include "shared/SharedRelayReadings.h"
#include "shared/RelayState.h"
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
    const TickType_t maxWaitTime = pdMS_TO_TICKS(SystemConstants::Tasks::RelayControl::MAX_WAIT_TIME_MS);
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
    // Validate relay index
    if (relayIndex < 1 || relayIndex > 8) {
        LOG_ERROR(TAG, "Invalid relay index: %d", relayIndex);
        return false;
    }

    // Always check rate limit for relay protection
    if (!checkRateLimit(relayIndex)) {
        LOG_WARN(TAG, "Rate limit exceeded for relay %d", relayIndex);
        return false;
    }

    // Check pump motor protection for relays 5 and 6 (heating and water pumps)
    // This prevents rapid on/off cycling that can damage pump motors
    if (!checkPumpProtection(relayIndex, state)) {
        // Pump protection blocks this state change - not an error, just too soon
        return false;
    }

    LOG_INFO(TAG, "Queuing relay %d to %s", relayIndex, state ? "ON" : "OFF");

    // Update desired state via g_relayState (atomic)
    // RYN4ProcessingTask will handle actual Modbus operation at next SET tick
    // relayIndex is 1-based, g_relayState uses 0-based bit positions
    g_relayState.setRelay(relayIndex - 1, state);

    // Update pump protection timestamp for pump relays
    const uint8_t heatingPumpPhysical = RelayIndex::toPhysical(RelayIndex::HEATING_PUMP);
    const uint8_t waterPumpPhysical = RelayIndex::toPhysical(RelayIndex::WATER_PUMP);
    if (relayIndex == heatingPumpPhysical) {
        pumpLastStateChangeTime[0] = xTaskGetTickCount();
        LOG_DEBUG(TAG, "Heating pump protection timer reset");
    } else if (relayIndex == waterPumpPhysical) {
        pumpLastStateChangeTime[1] = xTaskGetTickCount();
        LOG_DEBUG(TAG, "Water pump protection timer reset");
    }

    // Update local state tracking
    if (xSemaphoreTake(relayStateMutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
        currentRelayStates[relayIndex - 1] = state;
        xSemaphoreGive(relayStateMutex_);
    }
    relayStatesKnown.store(true);

    // Increment success counter (command queued successfully)
    commandsProcessed++;
    lastCommandTime = xTaskGetTickCount();

    LOG_DEBUG(TAG, "Relay %d command queued (will be sent at next SET tick)", relayIndex);
    return true;
}

// NEW METHOD: Process toggle relay (from v2)
bool RelayControlTask::processToggleRelay(uint8_t relayIndex) {
    if (relayIndex < 1 || relayIndex > 8) {
        LOG_ERROR(TAG, "Invalid relay index for toggle: %d", relayIndex);
        return false;
    }

    // Get current desired state from g_relayState (0-based index)
    bool currentState = g_relayState.getRelay(relayIndex - 1);

    // Toggle to opposite state
    return processSingleRelay(relayIndex, !currentState);
}

// NEW METHOD: Process set all relays (from v2)
bool RelayControlTask::processSetAllRelays(bool state) {
    LOG_INFO(TAG, "Queuing all relays to %s", state ? "ON" : "OFF");

    // Update g_relayState - set all relays to same state
    uint8_t bitmask = state ? 0xFF : 0x00;
    g_relayState.setAllRelays(bitmask);

    // Update local state tracking
    if (xSemaphoreTake(relayStateMutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (int i = 0; i < 8; i++) {
            currentRelayStates[i] = state;
        }
        xSemaphoreGive(relayStateMutex_);
    }
    relayStatesKnown.store(true);

    LOG_DEBUG(TAG, "All relays queued to %s (will be sent at next SET tick)", state ? "ON" : "OFF");
    return true;
}

// NEW METHOD: Process set multiple relays with smart update detection (from v2)
bool RelayControlTask::processSetMultipleRelays(const std::array<bool, 8>& states) {
    LOG_INFO(TAG, "Queuing multiple relay states");

    // Convert array to bitmask and update g_relayState
    uint8_t bitmask = 0;
    for (int i = 0; i < 8; i++) {
        if (states[i]) {
            bitmask |= (1 << i);
        }
    }
    g_relayState.setAllRelays(bitmask);

    // Update local state tracking
    if (xSemaphoreTake(relayStateMutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (size_t i = 0; i < states.size() && i < 8; i++) {
            currentRelayStates[i] = states[i];
        }
        xSemaphoreGive(relayStateMutex_);
    }
    relayStatesKnown.store(true);

    LOG_DEBUG(TAG, "Multiple relays queued: 0x%02X (will be sent at next SET tick)", bitmask);
    return true;
}

// NEW METHOD: Process toggle all relays (from v2)
bool RelayControlTask::processToggleAllRelays() {
    // Get current desired states from g_relayState and invert
    uint8_t current = g_relayState.desired.load(std::memory_order_acquire);
    uint8_t inverted = ~current;

    // Create inverted states array
    std::array<bool, 8> newStates;
    for (int i = 0; i < 8; i++) {
        newStates[i] = (inverted >> i) & 0x01;
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
    // Only applies to pump relays (physical 5=heating pump, physical 6=water pump)
    const uint8_t heatingPumpPhysical = RelayIndex::toPhysical(RelayIndex::HEATING_PUMP);  // 5
    const uint8_t waterPumpPhysical = RelayIndex::toPhysical(RelayIndex::WATER_PUMP);      // 6

    uint8_t pumpIdx;
    if (relayIndex == heatingPumpPhysical) {
        pumpIdx = 0;  // Heating pump
    } else if (relayIndex == waterPumpPhysical) {
        pumpIdx = 1;  // Water pump
    } else {
        return true;  // Not a pump relay, no protection needed
    }
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
    // Only applies to pump relays (physical 5=heating pump, physical 6=water pump)
    const uint8_t heatingPumpPhysical = RelayIndex::toPhysical(RelayIndex::HEATING_PUMP);  // 5
    const uint8_t waterPumpPhysical = RelayIndex::toPhysical(RelayIndex::WATER_PUMP);      // 6

    uint8_t pumpIdx;
    if (relayIndex == heatingPumpPhysical) {
        pumpIdx = 0;  // Heating pump
    } else if (relayIndex == waterPumpPhysical) {
        pumpIdx = 1;  // Water pump
    } else {
        return 0;  // Not a pump relay
    }

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
        bool success = setRelayState(RelayIndex::toPhysical(RelayIndex::HEATING_PUMP), true);
        // Clear bit unconditionally - event bits are edge-triggered, not level
        xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::HEATING_PUMP_ON);
        if (!success) {
            LOG_DEBUG(TAG, "Heating pump ON blocked by protection (will retry on next request)");
        }
    }

    if (requestBits & SystemEvents::RelayRequest::HEATING_PUMP_OFF) {
        LOG_DEBUG(TAG, "Processing heating pump OFF request");
        bool success = setRelayState(RelayIndex::toPhysical(RelayIndex::HEATING_PUMP), false);
        // Clear bit unconditionally - event bits are edge-triggered, not level
        xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::HEATING_PUMP_OFF);
        if (!success) {
            LOG_DEBUG(TAG, "Heating pump OFF blocked by protection (will retry on next request)");
        }
    }

    // Process water pump requests
    if (requestBits & SystemEvents::RelayRequest::WATER_PUMP_ON) {
        LOG_INFO(TAG, "Processing water pump ON request for relay %d", RelayIndex::toPhysical(RelayIndex::WATER_PUMP));

        // Log current state before attempting change
        // Round 20 Issue #6: Protect state array read with mutex
        if (relayStatesKnown.load() && xSemaphoreTake(relayStateMutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
            LOG_INFO(TAG, "Current water pump state before command: %s",
                     currentRelayStates[RelayIndex::WATER_PUMP] ? "ON" : "OFF");
            xSemaphoreGive(relayStateMutex_);
        }

        bool success = setRelayState(RelayIndex::toPhysical(RelayIndex::WATER_PUMP), true);
        // Clear bit unconditionally - event bits are edge-triggered, not level
        xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::WATER_PUMP_ON);
        if (success) {
            LOG_INFO(TAG, "Water pump ON request completed");
        } else {
            LOG_DEBUG(TAG, "Water pump ON blocked by protection (will retry on next request)");
        }
    }

    if (requestBits & SystemEvents::RelayRequest::WATER_PUMP_OFF) {
        LOG_DEBUG(TAG, "Processing water pump OFF request");
        bool success = setRelayState(RelayIndex::toPhysical(RelayIndex::WATER_PUMP), false);
        // Clear bit unconditionally - event bits are edge-triggered, not level
        xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::WATER_PUMP_OFF);
        if (!success) {
            LOG_DEBUG(TAG, "Water pump OFF blocked by protection (will retry on next request)");
        }
    }

    // Process burner requests
    if (requestBits & SystemEvents::RelayRequest::BURNER_ENABLE) {
        LOG_DEBUG(TAG, "Processing burner ON request");
        bool success = setRelayState(RelayIndex::toPhysical(RelayIndex::BURNER_ENABLE), true);
        // Clear bit unconditionally - event bits are edge-triggered, not level
        xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::BURNER_ENABLE);
        if (!success) {
            LOG_DEBUG(TAG, "Burner ON blocked by protection (will retry on next request)");
        }
    }

    if (requestBits & SystemEvents::RelayRequest::BURNER_DISABLE) {
        LOG_DEBUG(TAG, "Processing burner OFF request");
        bool success = setRelayState(RelayIndex::toPhysical(RelayIndex::BURNER_ENABLE), false);
        // Clear bit unconditionally - event bits are edge-triggered, not level
        xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::BURNER_DISABLE);
        if (!success) {
            LOG_DEBUG(TAG, "Burner OFF blocked by protection (will retry on next request)");
        }
    }

    // Process power boost requests
    if (requestBits & SystemEvents::RelayRequest::POWER_HALF) {
        LOG_DEBUG(TAG, "Processing half power request");
        bool success = setRelayState(RelayIndex::toPhysical(RelayIndex::POWER_BOOST), false);
        // Clear bit unconditionally - event bits are edge-triggered, not level
        xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::POWER_HALF);
        if (!success) {
            LOG_DEBUG(TAG, "Power HALF blocked by protection (will retry on next request)");
        }
    }

    if (requestBits & SystemEvents::RelayRequest::POWER_FULL) {
        LOG_DEBUG(TAG, "Processing full power request");
        bool success = setRelayState(RelayIndex::toPhysical(RelayIndex::POWER_BOOST), true);
        // Clear bit unconditionally - event bits are edge-triggered, not level
        xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::POWER_FULL);
        if (!success) {
            LOG_DEBUG(TAG, "Power FULL blocked by protection (will retry on next request)");
        }
    }

    // Process water mode requests
    if (requestBits & SystemEvents::RelayRequest::WATER_MODE_ON) {
        LOG_DEBUG(TAG, "Processing water mode ON request");
        bool success = setRelayState(RelayIndex::toPhysical(RelayIndex::WATER_MODE), true);
        // Clear bit unconditionally - event bits are edge-triggered, not level
        xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::WATER_MODE_ON);
        if (!success) {
            LOG_DEBUG(TAG, "Water mode ON blocked by protection (will retry on next request)");
        }
    }

    if (requestBits & SystemEvents::RelayRequest::WATER_MODE_OFF) {
        LOG_DEBUG(TAG, "Processing water mode OFF request");
        bool success = setRelayState(RelayIndex::toPhysical(RelayIndex::WATER_MODE), false);
        // Clear bit unconditionally - event bits are edge-triggered, not level
        xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::WATER_MODE_OFF);
        if (!success) {
            LOG_DEBUG(TAG, "Water mode OFF blocked by protection (will retry on next request)");
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
