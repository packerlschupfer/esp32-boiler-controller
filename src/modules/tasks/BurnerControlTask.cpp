// src/modules/tasks/BurnerControlTask_EventDriven.cpp
// Burner control task - manages burner state machine and safety
#include "modules/tasks/BurnerControlTask.h"

#include "config/SystemConstants.h"
#include "config/SystemSettingsStruct.h"
#include "modules/control/BurnerSystemController.h"
#include "modules/control/BurnerStateMachine.h"
#include "modules/control/TemperatureSensorFallback.h"
#include "modules/control/BurnerSafetyValidator.h"
#include "modules/control/BurnerRequestManager.h"
#include "modules/control/SafetyInterlocks.h"
#include "modules/control/CentralizedFailsafe.h"
#include "modules/control/ReturnPreheater.h"
// Note: ErrorRecoveryManager.h removed - its sync retry logic can exceed watchdog timeout
#include "shared/SharedResources.h"
#include "events/SystemEventsGenerated.h"
#include "events/TemperatureEventHelpers.h"
#include "utils/Utils.h"
#include "utils/ResourceGuard.h"
#include "utils/ErrorHandlingStrategy.h"
#include "utils/MutexRetryHelper.h"
#include "core/SystemResourceProvider.h"
#include "core/StateManager.h"
#include "config/ProjectConfig.h"
#include "LoggingMacros.h"
#include <TaskManager.h>
#include <atomic>
#include <climits>
#include <cmath>

// Timer handle for state machine timeouts only
static TimerHandle_t stateTimeoutTimer = nullptr;

// State tracking
static struct {
    bool initialized = false;
    bool sensorsReady = false;
    bool lastHeatDemand = false;
    Temperature_t lastTargetTemp = 0;
    bool lastIsWaterMode = false;
    uint32_t operationStartTime = 0;
    Temperature_t maxAllowedTemp = 0;
    float maxPowerFactor = 1.0f;  // Keep as float for power factor (0.0-1.0 range)
    uint32_t maxRunTime = UINT32_MAX;
} burnerState;

// Cached event group handles to avoid repeated mutex acquisitions
static struct {
    EventGroupHandle_t sensorEventGroup = nullptr;
    EventGroupHandle_t burnerEventGroup = nullptr;
    EventGroupHandle_t burnerRequestEventGroup = nullptr;
    EventGroupHandle_t systemStateEventGroup = nullptr;
    EventGroupHandle_t controlRequestsEventGroup = nullptr;
    bool initialized = false;
} cachedHandles;

// Forward declarations
static void stateTimeoutCallback(TimerHandle_t xTimer);
static void processBurnerRequest();
static void processTemperatureUpdate();
static void processSafetyEvent(EventBits_t safetyBits);
static void updateBurnerState(bool heatDemand, bool isWaterMode, Temperature_t targetTemp, bool highPower);

void BurnerControlTask(void* parameter) {
    (void)parameter;
    const char* TAG = "BurnerControlTask";
    
    LOG_INFO(TAG, "Started (Event-Driven) C%d Stk:%d", xPortGetCoreID(), uxTaskGetStackHighWaterMark(NULL));
    
    // Cache event group handles early to avoid mutex contention
    if (!cachedHandles.initialized) {
        LOG_INFO(TAG, "Caching event group handles...");
        cachedHandles.sensorEventGroup = SRP::getSensorEventGroup();
        cachedHandles.burnerEventGroup = SRP::getBurnerEventGroup();
        cachedHandles.burnerRequestEventGroup = SRP::getBurnerRequestEventGroup();
        cachedHandles.systemStateEventGroup = SRP::getSystemStateEventGroup();
        cachedHandles.controlRequestsEventGroup = SRP::getControlRequestsEventGroup();
        
        if (!cachedHandles.sensorEventGroup || !cachedHandles.burnerEventGroup ||
            !cachedHandles.burnerRequestEventGroup || !cachedHandles.systemStateEventGroup ||
            !cachedHandles.controlRequestsEventGroup) {
            LOG_ERROR(TAG, "Failed to cache one or more event groups!");
            vTaskDelete(NULL);
            return;
        }
        
        cachedHandles.initialized = true;
        LOG_INFO(TAG, "Event group handles cached successfully");
    }
    
    // Watchdog will be registered after initialization
    
    // Register cleanup handler
    TaskCleanupHandler::registerCleanup([TAG]() {
        LOG_WARN(TAG, "BurnerControlTask cleanup - emergency stop");
        BurnerStateMachine::emergencyStop();

        // Round 19 Issue #6: Stop and delete timer to prevent memory leak and dangling callback
        if (stateTimeoutTimer != nullptr) {
            xTimerStop(stateTimeoutTimer, 0);
            xTimerDelete(stateTimeoutTimer, 0);
            stateTimeoutTimer = nullptr;
        }

        if (cachedHandles.initialized && cachedHandles.burnerRequestEventGroup) {
            xEventGroupClearBits(cachedHandles.burnerRequestEventGroup, SystemEvents::BurnerRequest::ALL_BITS);
        }
    });
    
    // Initialize the burner state machine
    BurnerStateMachine::initialize();
    
    // Initialize temperature sensor fallback system
    TemperatureSensorFallback::initialize();

    // Start with NONE mode - only require sensors that are ACTUALLY needed
    // processBurnerRequest() will set appropriate mode when heating/water is requested
    // This prevents blocking on room temp sensor when no heating is actually requested
    TemperatureSensorFallback::setOperationMode(
        TemperatureSensorFallback::OperationMode::NONE);
    LOG_INFO(TAG, "Initial operation mode set to NONE (will update on actual request)");
    
    // Create state timeout timer for safety updates
    stateTimeoutTimer = xTimerCreate(
        "BurnerTimeout",
        pdMS_TO_TICKS(1000),  // 1 second safety interval
        pdTRUE,               // Auto-reload
        nullptr,
        stateTimeoutCallback
    );

    if (!stateTimeoutTimer) {
        LOG_ERROR(TAG, "Failed to create state timeout timer");
        // No cleanup needed - timer wasn't created
        vTaskDelete(NULL);
        return;
    }
    
    // Wait for initial sensor data
    LOG_INFO(TAG, "Waiting for sensor initialization...");
    EventBits_t sensorBits = xEventGroupWaitBits(
        cachedHandles.sensorEventGroup,
        SystemEvents::SensorUpdate::FIRST_READ_COMPLETE | SystemEvents::SensorUpdate::DATA_AVAILABLE,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(BURNER_STARTUP_GRACE_PERIOD_MS)
    );
    
    if (sensorBits & (SystemEvents::SensorUpdate::FIRST_READ_COMPLETE | SystemEvents::SensorUpdate::DATA_AVAILABLE)) {
        LOG_INFO(TAG, "Sensor data available (bits: 0x%X)", sensorBits);
        
        // Verify we have valid readings - use retry helper for robustness
        {
            auto guard = MutexRetryHelper::acquireGuard(
                SRP::getSensorReadingsMutex(),
                "SensorReadings"
            );
            if (guard) {
                SharedSensorReadings readings = SRP::getSensorReadings();
                if (readings.isBoilerTempOutputValid || readings.isBoilerTempReturnValid ||
                    readings.isWaterHeaterTempTankValid || readings.isInsideTempValid) {
                    burnerState.sensorsReady = true;
                    LOG_INFO(TAG, "Sensors ready - valid readings available");
                }
            }
            // Mutex auto-released when guard goes out of scope
        }
    }
    
    if (!burnerState.sensorsReady) {
        LOG_WARN(TAG, "Sensor initialization timeout - checking fallback status");
        if (!TemperatureSensorFallback::hasRequiredSensors()) {
            // Log which sensors are missing with detailed context
            const auto& status = TemperatureSensorFallback::getStatus();
            LOG_ERROR(TAG, "Required sensors missing for mode %d: BO=%s BR=%s WT=%s RT=%s",
                     static_cast<int>(TemperatureSensorFallback::getOperationMode()),
                     status.boilerOutputValid ? "OK" : "MISS",
                     status.boilerReturnValid ? "OK" : "MISS",
                     status.waterTempValid ? "OK" : "MISS",
                     status.roomTempValid ? "OK" : "MISS");

            // Burner cannot safely operate without required sensors
            // Set error bit and prevent burner from starting
            xEventGroupSetBits(SRP::getErrorNotificationEventGroup(),
                              SystemEvents::Error::SENSOR_FAILURE);
        }
    }

    burnerState.initialized = true;
    
    // Start a safety timer to ensure state machine gets periodic updates
    // This is a temporary measure until BurnerStateMachine is fully event-based
    xTimerChangePeriod(stateTimeoutTimer, pdMS_TO_TICKS(1000), 0);  // 1 second safety interval
    if (xTimerStart(stateTimeoutTimer, pdMS_TO_TICKS(100)) != pdPASS) {
        LOG_ERROR(TAG, "Failed to start state timeout timer");
        // Cleanup: delete timer before exiting
        xTimerDelete(stateTimeoutTimer, 0);
        stateTimeoutTimer = nullptr;
        vTaskDelete(NULL);
        return;
    }

    LOG_INFO(TAG, "Event-driven mode activated");
    
    // Register with watchdog after initialization is complete
    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::enabled(
        true,   // critical task - will reset system on timeout
        SystemConstants::System::WDT_BURNER_CONTROL_MS
    );

    if (!SRP::getTaskManager().registerCurrentTaskWithWatchdog("BurnerControlTask", wdtConfig)) {
        LOG_ERROR(TAG, "WDT reg failed - entering degraded mode");
        // Critical task without watchdog protection - enter degraded mode
        CentralizedFailsafe::triggerFailsafe(
            CentralizedFailsafe::FailsafeLevel::DEGRADED,
            SystemError::WATCHDOG_INIT_FAILED,
            "BurnerControlTask watchdog registration failed"
        );
    } else {
        LOG_INFO(TAG, "WDT OK %lums", SystemConstants::System::WDT_BURNER_CONTROL_MS);
        (void)SRP::getTaskManager().feedWatchdog();  // Feed immediately
    }
    
    // Main task loop - truly event-driven
    while (true) {
        // Build event masks
        const EventBits_t TEMP_UPDATE_BITS = SystemEvents::SensorUpdate::BOILER_OUTPUT | 
                                            SystemEvents::SensorUpdate::BOILER_RETURN | 
                                            SystemEvents::SensorUpdate::WATER_TANK;
        const EventBits_t SAFETY_EVENT_BITS = SystemEvents::Burner::FLAME_STATE_CHANGED | 
                                             SystemEvents::Burner::PRESSURE_CHANGED |
                                             SystemEvents::Burner::FLOW_CHANGED | 
                                             SystemEvents::Burner::SAFETY_EVENT | 
                                             SystemEvents::Burner::STATE_TIMEOUT;
        
        // Round 15 Issue #9: Dynamic timeout tuning rationale
        //
        // This task uses dynamic timeouts based on burner state to balance:
        //   1. SAFETY: Fast response during active combustion (100ms)
        //   2. CPU EFFICIENCY: Longer sleep when idle (3s)
        //   3. WATCHDOG: All timeouts maintain >5x margin to 15s hardware watchdog
        //
        // Why 100ms during active operation:
        //   - Gas combustion requires fast fault detection (<1s)
        //   - Sensor events fire every 100-200ms (Modbus read cycle)
        //   - 100ms provides 10 checks per second for temperature excursions
        //   - CPU impact: ~1% per check at priority 4
        //
        // Why 3s when idle:
        //   - No combustion = no urgent safety checks needed
        //   - Saves ~29 wakeups per second vs 100ms polling
        //   - Still responds to new heating requests within 3s
        //
        uint32_t timeoutMs = 1000;  // Default 1 second
        BurnerSMState currentState = BurnerStateMachine::getCurrentState();

        // Check if we have any heating demand from the request bits
        EventBits_t requestBits = xEventGroupGetBits(cachedHandles.burnerRequestEventGroup);
        bool hasHeatingDemand = (requestBits & (SystemEvents::BurnerRequest::HEATING |
                                                SystemEvents::BurnerRequest::WATER)) != 0;

        if (currentState == BurnerSMState::IDLE && !hasHeatingDemand) {  // IDLE state and no demand
            timeoutMs = 3000;  // 3 seconds when idle - save CPU while maintaining 5x margin to 15s watchdog
        } else if (currentState >= BurnerSMState::IGNITION &&
                   currentState <= BurnerSMState::RUNNING_HIGH) {  // Active operation
            timeoutMs = 100;   // 100ms during active operation for faster safety response
        }
        
        // Wait for burner request changes with a timeout
        // This is the primary event we care about
        EventBits_t requestEvents = xEventGroupWaitBits(
            cachedHandles.burnerRequestEventGroup,
            SystemEvents::BurnerRequest::CHANGE_EVENT_BITS,
            pdFALSE,  // Don't clear yet - we'll clear after processing
            pdFALSE,  // Wait for any bit
            pdMS_TO_TICKS(timeoutMs)  // Dynamic timeout based on state
        );
        
        // Process events in priority order
        // Feed watchdog between each handler to prevent timeout during cascading failures

        // 1. Emergency stop has highest priority
        // Round 20 Issue #3: Add state tracking to prevent re-entry within cooldown
        // C5: Use atomic to prevent race with timer callbacks
        static std::atomic<bool> emergencyStopActive{false};
        static std::atomic<uint32_t> emergencyStopTime{0};
        constexpr uint32_t EMERGENCY_COOLDOWN_MS = 5000;  // 5 second re-entry prevention

        // Use atomic read-and-clear to prevent race condition
        EventBits_t emergencyBits = xEventGroupWaitBits(
            cachedHandles.systemStateEventGroup,
            SystemEvents::SystemState::EMERGENCY_STOP,
            pdTRUE,   // Clear bits on exit (atomic with read)
            pdFALSE,  // Wait for any bit
            0         // No wait - just check current state
        );
        if ((emergencyBits & SystemEvents::SystemState::EMERGENCY_STOP) && !emergencyStopActive.load()) {
            emergencyStopActive.store(true);
            emergencyStopTime.store(millis());
            LOG_ERROR(TAG, "Emergency stop initiated");
            BurnerStateMachine::emergencyStop();
        } else if (emergencyStopActive.load() && Utils::elapsedMs(emergencyStopTime.load()) > EMERGENCY_COOLDOWN_MS) {
            // Allow re-triggering after cooldown period
            emergencyStopActive.store(false);
        }
        (void)SRP::getTaskManager().feedWatchdog();

        // 2. Safety events (skip STATE_TIMEOUT as it's handled separately in #5)
        // Use atomic read-and-clear to prevent race condition where new events
        // could be lost between reading and clearing
        EventBits_t safetyOnlyBits = xEventGroupWaitBits(
            cachedHandles.burnerEventGroup,
            SAFETY_EVENT_BITS & ~SystemEvents::Burner::STATE_TIMEOUT,
            pdTRUE,   // Clear bits on exit (atomic with read)
            pdFALSE,  // Wait for any bit
            0         // No wait - just check current state
        );
        if (safetyOnlyBits) {
            processSafetyEvent(safetyOnlyBits);
            // Bits already cleared atomically by xEventGroupWaitBits
        }
        (void)SRP::getTaskManager().feedWatchdog();

        // 2.5. Check for stale/expired burner requests (watchdog)
        // This prevents runaway burner if a control task crashes
        if (BurnerRequestManager::checkAndClearExpiredRequests(SystemConstants::Burner::REQUEST_EXPIRATION_MS)) {
            LOG_ERROR(TAG, "Stale burner request detected and cleared - control task may have crashed");
            SafetyInterlocks::triggerEmergencyShutdown("Burner request watchdog expired");
        }
        (void)SRP::getTaskManager().feedWatchdog();

        // 3. Temperature updates trigger state machine update
        // Use atomic read-and-clear to prevent race condition
        EventBits_t tempUpdateBits = xEventGroupWaitBits(
            cachedHandles.sensorEventGroup,
            TEMP_UPDATE_BITS,
            pdTRUE,   // Clear bits on exit (atomic with read)
            pdFALSE,  // Wait for any bit
            0         // No wait - just check current state
        );
        if (tempUpdateBits & TEMP_UPDATE_BITS) {
            processTemperatureUpdate();
            // Bits already cleared atomically by xEventGroupWaitBits
        }
        (void)SRP::getTaskManager().feedWatchdog();

        // 4. Burner requests - process if we got a change event
        if (requestEvents & SystemEvents::BurnerRequest::CHANGE_EVENT_BITS) {
            LOG_DEBUG(TAG, "Processing burner request change event (events: 0x%06X)", requestEvents);
            processBurnerRequest();
            // Clear the change event bits after processing
            xEventGroupClearBits(cachedHandles.burnerRequestEventGroup,
                                SystemEvents::BurnerRequest::CHANGE_EVENT_BITS);
        }
        (void)SRP::getTaskManager().feedWatchdog();

        // 5. State timeout (timer-generated event for periodic updates)
        // Use atomic read-and-clear to prevent race condition
        EventBits_t timeoutBits = xEventGroupWaitBits(
            cachedHandles.burnerEventGroup,
            SystemEvents::Burner::STATE_TIMEOUT,
            pdTRUE,   // Clear bits on exit (atomic with read)
            pdFALSE,  // Wait for any bit
            0         // No wait - just check current state
        );
        if (timeoutBits & SystemEvents::Burner::STATE_TIMEOUT) {
            BurnerStateMachine::update();
            // Bit already cleared atomically by xEventGroupWaitBits
        }
        (void)SRP::getTaskManager().feedWatchdog();

        // Note: Pump control is now handled independently by PumpControlModule tasks
        // (HeatingPumpTask, WaterPumpTask) which watch HEATING_ON/WATER_ON event bits.
        // No cooldown handling needed here.

        // Update return preheater state machine (needs continuous updates for pump cycling)
        // This must be called every iteration, not just on state changes
        if (ReturnPreheater::getState() == ReturnPreheater::State::PREHEATING) {
            ReturnPreheater::update();
        }

        // Final watchdog feed at end of loop
        (void)SRP::getTaskManager().feedWatchdog();
    }
}

static void stateTimeoutCallback(TimerHandle_t xTimer) {
    (void)xTimer;
    
    // Set timeout event bit
    if (cachedHandles.initialized && cachedHandles.burnerEventGroup) {
        xEventGroupSetBits(cachedHandles.burnerEventGroup, SystemEvents::Burner::STATE_TIMEOUT);
    }
}

static void processTemperatureUpdate() {
    const char* TAG = "BurnerTempUpdate";
    
    // Update state machine when temperatures change
    BurnerStateMachine::update();
    
    // Check temperature sensor status
    if (!TemperatureSensorFallback::canContinueOperation()) {
        if (burnerState.lastHeatDemand) {
            LOG_ERROR(TAG, "Temperature sensor failure - emergency shutdown");
            BurnerStateMachine::emergencyStop();
            burnerState.lastHeatDemand = false;
            // Signal sensor error (bits not defined in current system)
        }
    }
}

static void processSafetyEvent(EventBits_t safetyBits) {
    const char* TAG = "BurnerSafety";

    // safetyBits passed from caller (already cleared atomically by xEventGroupWaitBits)

    if (safetyBits & SystemEvents::Burner::FLAME_STATE_CHANGED) {
        LOG_INFO(TAG, "Flame state changed");
        // Future: process flame sensor change
    }
    
    if (safetyBits & SystemEvents::Burner::PRESSURE_CHANGED) {
        LOG_INFO(TAG, "Pressure changed");
        // Future: process pressure sensor change
    }
    
    if (safetyBits & SystemEvents::Burner::FLOW_CHANGED) {
        LOG_INFO(TAG, "Flow changed");
        // Future: process flow sensor change
    }
    
    // Perform safety check via BurnerSystemController
    BurnerSystemController* controller = SRP::getBurnerSystemController();
    if (!controller) {
        LOG_ERROR(TAG, "BurnerSystemController not available - triggering emergency stop");
        BurnerStateMachine::emergencyStop();
        burnerState.lastHeatDemand = false;
        return;
    }
    Result<void> safetyResult = controller->performSafetyCheck();
    if (safetyResult.isError()) {
        ErrorHandling::TaskErrorHandler::handleTaskError(
            TAG,
            safetyResult.error(),
            safetyResult.message().c_str(),
            0, 0  // No error bits defined for burner safety
        );
        BurnerStateMachine::emergencyStop();
        burnerState.lastHeatDemand = false;
    }
    
    // Update state machine with new safety inputs
    BurnerStateMachine::update();
}

static void processBurnerRequest() {
    const char* TAG = "BurnerProcess";
    
    // Use cached event group handles
    if (!cachedHandles.initialized) {
        LOG_ERROR(TAG, "Event group handles not cached!");
        return;
    }
    
    // Get current request bits using cached handles
    EventBits_t requestBits = xEventGroupGetBits(cachedHandles.burnerRequestEventGroup);
    EventBits_t systemStateBits = xEventGroupGetBits(cachedHandles.systemStateEventGroup);
    
    // Track if we were previously disabled to reduce log spam
    static bool wasSystemDisabled = false;

    // Check if boiler is enabled
    if (!(systemStateBits & SystemEvents::SystemState::BOILER_ENABLED)) {
        // Always ensure burner is off when system is disabled
        if (!wasSystemDisabled) {
            LOG_INFO(TAG, "Boiler disabled - ensuring burner is off");
            wasSystemDisabled = true;

            // Immediate deactivation when boiler disabled
            BurnerSystemController* controller = SRP::getBurnerSystemController();
            if (controller) {
                // L10: Check deactivate result - system disable is critical
                auto result = controller->deactivate();
                if (result.isError()) {
                    LOG_ERROR(TAG, "SYSTEM DISABLE: Failed to deactivate: %s", result.message().c_str());
                    // Try emergency shutdown
                    controller->emergencyShutdown("System disable deactivate failed");
                }
            }
        }

        BurnerStateMachine::setHeatDemand(false, 0);
        burnerState.lastHeatDemand = false;

        // C2: Atomic clear-and-set to prevent race with other tasks reading state
        constexpr EventBits_t BURNER_STATE_BITS =
            SystemEvents::SystemState::BURNER_OFF | SystemEvents::SystemState::BURNER_HEATING_LOW |
            SystemEvents::SystemState::BURNER_HEATING_HIGH | SystemEvents::SystemState::BURNER_WATER_LOW |
            SystemEvents::SystemState::BURNER_WATER_HIGH;
        portMUX_TYPE spinlock = portMUX_INITIALIZER_UNLOCKED;
        portENTER_CRITICAL(&spinlock);
        xEventGroupClearBits(cachedHandles.systemStateEventGroup, BURNER_STATE_BITS);
        xEventGroupSetBits(cachedHandles.systemStateEventGroup, SystemEvents::SystemState::BURNER_OFF);
        portEXIT_CRITICAL(&spinlock);
        return;
    }

    // Reset flag when system is enabled again
    wasSystemDisabled = false;
    
    // Check temperature sensor status
    if (!TemperatureSensorFallback::canContinueOperation()) {
        return;
    }
    
    // Get safe operating parameters
    TemperatureSensorFallback::getSafeOperatingParams(
        burnerState.maxAllowedTemp, 
        burnerState.maxPowerFactor, 
        burnerState.maxRunTime
    );
    
    // Determine heat demand
    bool heatingRequested = (requestBits & SystemEvents::BurnerRequest::HEATING) != 0;
    bool waterRequested = (requestBits & SystemEvents::BurnerRequest::WATER) != 0;
    bool waterPriority = (systemStateBits & SystemEvents::SystemState::WATER_PRIORITY) != 0;
    
    // Update operation mode
    if (heatingRequested && waterRequested) {
        TemperatureSensorFallback::setOperationMode(
            TemperatureSensorFallback::OperationMode::BOTH);
    } else if (heatingRequested) {
        TemperatureSensorFallback::setOperationMode(
            TemperatureSensorFallback::OperationMode::SPACE_HEATING);
    } else if (waterRequested) {
        TemperatureSensorFallback::setOperationMode(
            TemperatureSensorFallback::OperationMode::WATER_HEATING);
    } else {
        TemperatureSensorFallback::setOperationMode(
            TemperatureSensorFallback::OperationMode::NONE);
    }
    
    // Determine actual heat demand
    bool heatDemand = false;
    bool isWaterMode = false;
    Temperature_t targetTemp = 0;
    bool highPower = false;
    
    if (waterRequested && (waterPriority || !heatingRequested)) {
        heatDemand = true;
        isWaterMode = true;
        targetTemp = SystemEvents::BurnerRequest::decode_temperature_t(requestBits);
        highPower = (requestBits & SystemEvents::BurnerRequest::POWER_HIGH) != 0;
    } else if (heatingRequested) {
        heatDemand = true;
        isWaterMode = false;
        targetTemp = SystemEvents::BurnerRequest::decode_temperature_t(requestBits);
        highPower = (requestBits & SystemEvents::BurnerRequest::POWER_HIGH) != 0;
    }
    
    // Validate and update state
    if (targetTemp < tempFromWhole(20) && heatDemand) {  // 20°C minimum
        targetTemp = tempFromWhole(70);  // 70°C default
        char tempBuf[16];
        formatTemp(tempBuf, sizeof(tempBuf), targetTemp);
        LOG_WARN(TAG, "Invalid target temperature, using default: %s°C", tempBuf);
    }
    
    // Apply safety limits
    if (heatDemand && burnerState.maxAllowedTemp > 0) {
        if (targetTemp > burnerState.maxAllowedTemp) {
            char targetBuf[16], limitBuf[16];
            formatTemp(targetBuf, sizeof(targetBuf), targetTemp);
            formatTemp(limitBuf, sizeof(limitBuf), burnerState.maxAllowedTemp);
            LOG_WARN(TAG, "Limiting target temp from %s°C to %s°C",
                    targetBuf, limitBuf);
            targetTemp = burnerState.maxAllowedTemp;
        }
        
        if (burnerState.maxPowerFactor < 1.0f && highPower) {
            LOG_WARN(TAG, "Disabling high power due to sensor fallback");
            highPower = false;
        }
        
        // Check runtime limit
        if (burnerState.operationStartTime == 0) {
            burnerState.operationStartTime = millis();
        } else if (burnerState.maxRunTime < UINT32_MAX) {
            uint32_t runtime = millis() - burnerState.operationStartTime;
            if (runtime > burnerState.maxRunTime) {
                LOG_ERROR(TAG, "Maximum runtime exceeded - shutting down");
                heatDemand = false;
                BurnerStateMachine::emergencyStop();
            }
        }
    } else if (!heatDemand) {
        burnerState.operationStartTime = 0;
    }
    
    // Update state if changed
    if (heatDemand != burnerState.lastHeatDemand || 
        (heatDemand && tempAbs(tempSub(targetTemp, burnerState.lastTargetTemp)) > tempFromWhole(1)) ||  // > 1°C difference
        (heatDemand && isWaterMode != burnerState.lastIsWaterMode)) {
        
        updateBurnerState(heatDemand, isWaterMode, targetTemp, highPower);
    }
}

static void updateBurnerState(bool heatDemand, bool isWaterMode, Temperature_t targetTemp, bool highPower) {
    const char* TAG = "BurnerUpdate";

    // Update return preheater state machine (thermal shock mitigation)
    ReturnPreheater::update();

    // Block heat demand while preheating is in progress
    if (heatDemand && ReturnPreheater::getState() == ReturnPreheater::State::PREHEATING) {
        LOG_DEBUG(TAG, "Heat demand blocked - return preheating in progress (cycle %u)",
                  ReturnPreheater::getCurrentCycle());
        heatDemand = false;
    }

    // Reset preheater state once burner turns off (ready for next cycle)
    if (!heatDemand && ReturnPreheater::isComplete()) {
        ReturnPreheater::reset();
    }

    if (heatDemand) {
        char tempBuf[16];
        formatTemp(tempBuf, sizeof(tempBuf), targetTemp);
        if (isWaterMode != burnerState.lastIsWaterMode) {
            LOG_INFO(TAG, "Switching mode from %s to %s - Target: %s°C, Power: %s",
                    burnerState.lastIsWaterMode ? "Water" : "Heating",
                    isWaterMode ? "Water" : "Heating",
                    tempBuf,
                    highPower ? "HIGH" : "LOW");
        } else {
            LOG_INFO(TAG, "Burner demand changed - Heat: ON, Mode: %s, Target: %s°C, Power: %s",
                    isWaterMode ? "Water" : "Heating",
                    tempBuf,
                    highPower ? "HIGH" : "LOW");
        }
    } else {
        LOG_INFO(TAG, "No heat demand - burner going idle");
    }
    
    burnerState.lastHeatDemand = heatDemand;
    burnerState.lastTargetTemp = targetTemp;
    burnerState.lastIsWaterMode = isWaterMode;

    // Note: Relay control is now handled by BurnerStateMachine via BurnerSystemController
    // The setHeatDemand() call below will trigger state transitions that control relays

    // C2: Moved clear-bits closer to set-bits (line ~720) to minimize race window
    // The atomic clear-and-set is done in the switch statement below

    // Safety validation using StateManager for staleness detection
    if (heatDemand) {
        SharedSensorReadings readings = {};

        // Atomic sensor read: get readings AND staleness in single mutex acquisition
        // This prevents TOCTOU race between checking staleness and reading data
        SensorReadingsWithAge sensorResult = StateManager::getSensorReadingsAtomic();

        // H4: Feed watchdog after potentially slow mutex operation
        (void)SRP::getTaskManager().feedWatchdog();

        if (!sensorResult.mutexAcquired) {
            LOG_ERROR(TAG, "Failed to acquire sensor mutex - blocking burner");
            heatDemand = false;
            xEventGroupSetBits(SRP::getErrorNotificationEventGroup(),
                              SystemEvents::Error::SENSOR_FAILURE);
        } else if (sensorResult.isStale) {
            LOG_ERROR(TAG, "Sensor data stale (%lu ms old) - blocking burner operation",
                      sensorResult.ageMs);
            heatDemand = false;
            xEventGroupSetBits(SRP::getErrorNotificationEventGroup(),
                              SystemEvents::Error::SENSOR_FAILURE);
        } else {
            readings = sensorResult.readings;
        }

        if (heatDemand) {
            BurnerSafetyValidator::SafetyConfig safetyConfig;
            safetyConfig.maxBoilerTemp = burnerState.maxAllowedTemp;
            safetyConfig.maxWaterTemp = SRP::getSystemSettings().wHeaterConfTempSafeLimitHigh;

            auto validationResult = BurnerSafetyValidator::validateBurnerOperation(
                readings, safetyConfig, isWaterMode);

            // H4: Feed watchdog after safety validation (can involve multiple mutex operations)
            (void)SRP::getTaskManager().feedWatchdog();

            if (validationResult != BurnerSafetyValidator::ValidationResult::SAFE_TO_OPERATE) {
                LOG_ERROR(TAG, "Safety validation failed: %s",
                        BurnerSafetyValidator::getValidationErrorMessage(validationResult));

                BurnerSafetyValidator::logSafetyEvent(validationResult,
                    isWaterMode ? "Water heating mode" : "Space heating mode");

                // For sensor/pump failures, disable heat demand immediately (fail-safe)
                // Recovery will happen on subsequent loop iterations when conditions improve
                // NOTE: We do NOT call ErrorRecoveryManager::handleError() synchronously here
                // because its retry logic with delays can exceed the 15s watchdog timeout.
                // The ErrorRecoveryManager's RecoveryMonitor task handles async recovery.
                heatDemand = false;

                // Special handling for thermal shock - start return preheating
                if (validationResult == BurnerSafetyValidator::ValidationResult::THERMAL_SHOCK_RISK) {
                    // Start preheating if not already running
                    if (ReturnPreheater::getState() == ReturnPreheater::State::IDLE) {
                        LOG_INFO(TAG, "Starting return preheating to mitigate thermal shock");
                        ReturnPreheater::start();
                    }
                    // Don't trigger emergency stop - preheating will resolve this
                } else if (validationResult != BurnerSafetyValidator::ValidationResult::SENSOR_FAILURE &&
                    validationResult != BurnerSafetyValidator::ValidationResult::PUMP_FAILURE) {
                    // For more severe errors, trigger emergency stop
                    BurnerStateMachine::emergencyStop();
                }
            } else {
                if (!burnerState.lastHeatDemand) {
                    burnerState.operationStartTime = millis();
                }
            }
        }
    }
    
    // Update state machine with PID-driven power level
    BurnerStateMachine::setHeatDemand(heatDemand, targetTemp, highPower);
    
    // C2: Update system state bits atomically (clear + set in critical section)
    // This prevents other tasks from seeing intermediate state with all bits cleared
    constexpr EventBits_t BURNER_STATE_BITS =
        SystemEvents::SystemState::BURNER_OFF | SystemEvents::SystemState::BURNER_HEATING_LOW |
        SystemEvents::SystemState::BURNER_HEATING_HIGH | SystemEvents::SystemState::BURNER_WATER_LOW |
        SystemEvents::SystemState::BURNER_WATER_HIGH | SystemEvents::SystemState::BURNER_ERROR;

    BurnerSMState currentState = BurnerStateMachine::getCurrentState();
    EventBits_t newStateBit = 0;

    switch (currentState) {
        case BurnerSMState::IDLE:
        case BurnerSMState::POST_PURGE:
            newStateBit = SystemEvents::SystemState::BURNER_OFF;
            break;
        case BurnerSMState::RUNNING_LOW:
            newStateBit = isWaterMode ? SystemEvents::SystemState::BURNER_WATER_LOW
                                      : SystemEvents::SystemState::BURNER_HEATING_LOW;
            break;
        case BurnerSMState::RUNNING_HIGH:
            newStateBit = isWaterMode ? SystemEvents::SystemState::BURNER_WATER_HIGH
                                      : SystemEvents::SystemState::BURNER_HEATING_HIGH;
            break;
        case BurnerSMState::LOCKOUT:
        case BurnerSMState::ERROR:
            newStateBit = SystemEvents::SystemState::BURNER_ERROR;
            break;
        default:
            break;
    }

    // Atomic clear-and-set using critical section
    if (newStateBit != 0) {
        portMUX_TYPE spinlock = portMUX_INITIALIZER_UNLOCKED;
        portENTER_CRITICAL(&spinlock);
        xEventGroupClearBits(cachedHandles.systemStateEventGroup, BURNER_STATE_BITS);
        xEventGroupSetBits(cachedHandles.systemStateEventGroup, newStateBit);
        portEXIT_CRITICAL(&spinlock);
    }
}
