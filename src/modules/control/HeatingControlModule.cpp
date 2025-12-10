// src/modules/control/HeatingControlModule.cpp
#include "modules/control/HeatingControlModule.h"
#include "utils/ErrorHandler.h"
#include "modules/control/HeatingControlModuleFixedPoint.h"  // For fixed-point calculations
#include "modules/control/PIDControlModule.h" // For calculatePIDAdjustment
#include "modules/control/PIDAutoTuner.h"
#include "modules/control/BurnerAntiFlapping.h"  // For PID deadband checking
#include "modules/control/BurnerRequestManager.h"  // Round 13 Issue #1: For atomic event group updates
#include "shared/Temperature.h"  // For temperature conversions
#include "config/SystemConstants.h"  // For magic number constants
#include "utils/Utils.h"
#include "utils/CriticalDataStorage.h"  // For PID tuning persistence
#include "utils/MutexRetryHelper.h"
#include "utils/MutexGuard.h"  // Round 13 Issue #2, #3: RAII mutex guard
#include "LoggingMacros.h"
#include <esp_task_wdt.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "core/SystemResourceProvider.h"
#include <TaskManager.h>
#include "events/SystemEventsGenerated.h"
#include "events/TemperatureEventHelpers.h"  // For encode_temperature_t
#include "shared/SharedSensorReadings.h"
#include "core/StateManager.h"
#include "config/ProjectConfig.h"
#include "config/SafetyConfig.h"  // M1: For configurable PID integral limits
#include "modules/tasks/MQTTTask.h"
#include "config/PIDControllerIDs.h"


[[maybe_unused]] static const char* TAG = "HeatingControl";

// Bounded mutex timeout - NEVER use MUTEX_TIMEOUT to prevent deadlock
static constexpr TickType_t MUTEX_TIMEOUT = pdMS_TO_TICKS(SystemConstants::Timing::MUTEX_DEFAULT_TIMEOUT_MS);

// Access shared resources through SRP::getSensorReadings()

HeatingControlModule::HeatingControlModule(EventGroupHandle_t systemEventGroup, SemaphoreHandle_t sensorMutex) {
    // Parameters are ignored - kept for backward compatibility
    // The module now uses SystemResourceProvider internally
    (void)systemEventGroup;
    (void)sensorMutex;

    // H2: Initialize auto-tuner with OOM handling
    // Note: Can't trigger failsafe here (system not fully initialized), but we mark
    // allocation failure for later checking via initialize() or task startup
    autoTuner = new PIDAutoTuner();
    if (!autoTuner) {
        LOG_ERROR("HeatingControl", "CRITICAL: Failed to allocate PIDAutoTuner - OOM");
        // System will check for null in initialize() and during operation
    }

    // H2: Initialize fixed-point PID controller with OOM handling
    fixedPointPID = new PIDControlModuleFixedPoint();
    if (!fixedPointPID) {
        LOG_ERROR("HeatingControl", "CRITICAL: Failed to allocate PIDControlModuleFixedPoint - OOM");
        // PID task will check for null and refuse to operate
    }
}

HeatingControlModule::~HeatingControlModule() {
    // C1: Safely stop the PID task before destroying resources
    // This prevents undefined behavior from task accessing freed memory
    stopSpaceHeatingPIDTask();

    delete autoTuner;
    delete fixedPointPID;

    // Cleanup mutexes (Round 13 Issue #2, #3)
    if (taskStateMutex_ != nullptr) {
        vSemaphoreDelete(taskStateMutex_);
        taskStateMutex_ = nullptr;
    }
    if (pidGainsMutex_ != nullptr) {
        vSemaphoreDelete(pidGainsMutex_);
        pidGainsMutex_ = nullptr;
    }
}

void HeatingControlModule::initialize() {
    // Create thread safety mutexes (Round 13 Issue #2, #3)
    if (taskStateMutex_ == nullptr) {
        taskStateMutex_ = xSemaphoreCreateMutex();
        if (!taskStateMutex_) {
            LOG_ERROR(TAG, "Failed to create task state mutex");
        }
    }
    if (pidGainsMutex_ == nullptr) {
        pidGainsMutex_ = xSemaphoreCreateMutex();
        if (!pidGainsMutex_) {
            LOG_ERROR(TAG, "Failed to create PID gains mutex");
        }
    }

    // Load PID gains from settings (Round 13 Issue #2)
    SystemSettings& settings = SRP::getSystemSettings();
    pidKp = settings.spaceHeatingKp;
    pidKi = settings.spaceHeatingKi;
    pidKd = settings.spaceHeatingKd;
    LOG_INFO(TAG, "Loaded space heating PID gains: Kp=%.2f, Ki=%.4f, Kd=%.2f",
             pidKp, pidKi, pidKd);

    LOG_INFO(TAG, "HeatingControlModule initialized.");

    // Restore PID state from FRAM if available
    if (fixedPointPID) {
        if (fixedPointPID->restoreState(PID_CONTROLLER_SPACE_HEATING)) {
            LOG_INFO(TAG, "Space heating PID state restored from FRAM");
        } else {
            LOG_INFO(TAG, "No saved PID state found, starting fresh");
        }

        // M1: Apply configurable anti-windup limits from SafetyConfig
        fixedPointPID->setIntegralLimits(SafetyConfig::pidIntegralMin,
                                          SafetyConfig::pidIntegralMax);
        LOG_INFO(TAG, "PID integral limits set: min=%ld, max=%ld",
                 (long)SafetyConfig::pidIntegralMin, (long)SafetyConfig::pidIntegralMax);
    }

    // NOTE: SpaceHeatingPIDTask is NOT started here anymore.
    // It will be started from SystemInitializer::initializeTasks() AFTER ANDRTF3Task
    // starts, so that the INSIDE sensor event is available when PID begins waiting.
}

bool HeatingControlModule::startAutoTuning(Temperature_t setpoint) {
    // Thread-safe check (Round 13 Issue #3)
    {
        MutexGuard guard(taskStateMutex_, pdMS_TO_TICKS(100));
        if (!guard.hasLock()) {
            LOG_ERROR(TAG, "Failed to acquire task state mutex for auto-tune start");
            return false;
        }
        if (isAutoTuning || !autoTuner) {
            return false;
        }
    }

    // Safety checks before starting
    EventBits_t systemState = SRP::getSystemStateEventBits();
    if (!(systemState & SystemEvents::SystemState::BOILER_ENABLED) ||
        !(systemState & SystemEvents::SystemState::HEATING_ENABLED)) {
        LOG_ERROR(TAG, "Cannot start auto-tuning: system not enabled");
        return false;
    }

    // Check for errors
    EventBits_t errorBits = xEventGroupGetBits(SRP::getErrorNotificationEventGroup());
    if (errorBits & SystemEvents::Error::ANY_ACTIVE) {
        LOG_ERROR(TAG, "Cannot start auto-tuning: errors present");
        return false;
    }

    // Pre-tuning temperature validation
    Temperature_t currentBoilerTemp = 0;
    bool sensorValid = false;
    {
        auto guard = MutexRetryHelper::acquireGuard(
            SRP::getSensorReadingsMutex(),
            "SensorReadings-AutoTunePreCheck"
        );
        if (guard) {
            const SharedSensorReadings& readings = SRP::getSensorReadings();
            if (readings.isBoilerTempOutputValid) {
                currentBoilerTemp = readings.boilerTempOutput;
                sensorValid = true;
            }
        }
    }

    if (!sensorValid) {
        LOG_ERROR(TAG, "Cannot start auto-tuning: boiler temp sensor invalid");
        return false;
    }

    // Check boiler temperature is in safe range for tuning
    if (currentBoilerTemp < SystemConstants::PID::Autotune::MIN_BOILER_TEMP) {
        char tempBuf[16];
        formatTemp(tempBuf, sizeof(tempBuf), currentBoilerTemp);
        LOG_ERROR(TAG, "Cannot start auto-tuning: boiler temp too low (%s°C, min 15°C)", tempBuf);
        return false;
    }

    if (currentBoilerTemp > SystemConstants::PID::Autotune::MAX_BOILER_TEMP) {
        char tempBuf[16];
        formatTemp(tempBuf, sizeof(tempBuf), currentBoilerTemp);
        LOG_ERROR(TAG, "Cannot start auto-tuning: boiler temp too high (%s°C, max 75°C)", tempBuf);
        return false;
    }

    char tempBuf[16];
    formatTemp(tempBuf, sizeof(tempBuf), currentBoilerTemp);
    LOG_INFO(TAG, "Pre-tuning check passed: boiler temp %s°C", tempBuf);

    // Start auto-tuning with configurable parameters from SystemSettings
    autoTuneSetpoint = setpoint;
    float setpointFloat = tempToFloat(setpoint);
    const SystemSettings& settings = SRP::getSystemSettings();

    // Map method index to enum (0=ZN_PI, 1=ZN_PID, 2=TL, 3=CC, 4=Lambda)
    PIDAutoTuner::TuningMethod method = PIDAutoTuner::TuningMethod::TYREUS_LUYBEN;
    switch (settings.autotuneMethod) {
        case 0: method = PIDAutoTuner::TuningMethod::ZIEGLER_NICHOLS_PI; break;
        case 1: method = PIDAutoTuner::TuningMethod::ZIEGLER_NICHOLS_PID; break;
        case 2: method = PIDAutoTuner::TuningMethod::TYREUS_LUYBEN; break;
        case 3: method = PIDAutoTuner::TuningMethod::COHEN_COON; break;
        case 4: method = PIDAutoTuner::TuningMethod::LAMBDA_TUNING; break;
        default: method = PIDAutoTuner::TuningMethod::TYREUS_LUYBEN; break;
    }

    LOG_INFO(TAG, "Auto-tune config: amplitude=%.1f%%, hysteresis=%.1f°C, method=%ld",
             settings.autotuneRelayAmplitude, settings.autotuneHysteresis, (long)settings.autotuneMethod);

    if (autoTuner->startTuning(setpointFloat, settings.autotuneRelayAmplitude,
                               settings.autotuneHysteresis, method)) {
        // Thread-safe state update (Round 13 Issue #3)
        {
            MutexGuard guard(taskStateMutex_, pdMS_TO_TICKS(100));
            if (guard.hasLock()) {
                isAutoTuning = true;
            }
        }
        xEventGroupSetBits(SRP::getHeatingEventGroup(), SystemEvents::HeatingEvent::AUTOTUNE_RUNNING);
        xEventGroupClearBits(SRP::getHeatingEventGroup(),
            SystemEvents::HeatingEvent::AUTOTUNE_COMPLETE | SystemEvents::HeatingEvent::AUTOTUNE_FAILED);
        formatTemp(tempBuf, sizeof(tempBuf), setpoint);
        LOG_INFO(TAG, "PID auto-tuning started with setpoint: %s°C", tempBuf);
        return true;
    }

    return false;
}

void HeatingControlModule::stopAutoTuning() {
    // Thread-safe check and update (Round 13 Issue #3)
    bool wasAutoTuning = false;
    {
        MutexGuard guard(taskStateMutex_, pdMS_TO_TICKS(100));
        if (guard.hasLock()) {
            wasAutoTuning = isAutoTuning;
            if (isAutoTuning) {
                isAutoTuning = false;
            }
        }
    }

    if (wasAutoTuning && autoTuner) {
        autoTuner->stopTuning();
        xEventGroupClearBits(SRP::getHeatingEventGroup(), SystemEvents::HeatingEvent::AUTOTUNE_RUNNING);
        xEventGroupSetBits(SRP::getHeatingEventGroup(), SystemEvents::HeatingEvent::AUTOTUNE_FAILED);
        LOG_INFO(TAG, "PID auto-tuning stopped by user");
    }
}

PIDAutoTuner::TuningState HeatingControlModule::getAutoTuningState() const {
    if (autoTuner) {
        return autoTuner->getState();
    }
    return PIDAutoTuner::TuningState::IDLE;
}

uint8_t HeatingControlModule::getAutoTuningProgress() const {
    if (autoTuner && isAutoTuning) {
        return autoTuner->getProgress();
    }
    return 0;
}

PIDAutoTuner::TuningResult HeatingControlModule::getAutoTuningResults() const {
    if (autoTuner) {
        return autoTuner->getResults();
    }
    return PIDAutoTuner::TuningResult();
}

Temperature_t HeatingControlModule::calculateSpaceHeatingTargetTemp(const SharedSensorReadings& readings, const SystemSettings& settings) const {
    // Use fixed-point calculations from HeatingControlModuleFixedPoint
    // Convert float coefficients to fixed-point (scaled by 100)
    int16_t curveCoeff = static_cast<int16_t>(settings.heating_curve_coeff * 100);
    Temperature_t curveShift = tempFromFloat(settings.heating_curve_shift);
    
    Temperature_t result = HeatingControlModuleFixedPoint::calculateHeatingCurveTarget(
        readings.insideTemp,
        readings.outsideTemp,
        curveCoeff,
        curveShift,
        settings.burner_low_limit,
        settings.heating_high_limit
    );

    char insideBuf[16], outsideBuf[16], resultBuf[16];
    formatTemp(insideBuf, sizeof(insideBuf), readings.insideTemp);
    formatTemp(outsideBuf, sizeof(outsideBuf), readings.outsideTemp);
    formatTemp(resultBuf, sizeof(resultBuf), result);
    LOG_DEBUG(TAG, "Calculated target temp: %s, based on inside: %s, outside: %s", 
             resultBuf, insideBuf, outsideBuf);
    return result;
}

bool HeatingControlModule::checkHeatingConditions(const SharedSensorReadings& readings, Temperature_t targetTemperature, Temperature_t hysteresis) const {
    bool heatingRequired = false;
    EventBits_t currentSystemState = xEventGroupGetBits(SRP::getSystemStateEventGroup());

    // Check if heating is enabled
    if ((currentSystemState & SystemEvents::SystemState::HEATING_ENABLED) != 0) {
        
        // Determine if heating should be turned on or off based on temperature and hysteresis
        if (readings.insideTemp < tempSub(targetTemperature, hysteresis)) {
            heatingRequired = true;
        } else if (readings.insideTemp >= tempAdd(targetTemperature, hysteresis)) {
            heatingRequired = false;
        }
    } else {
        // Heating is disabled; ensure it's off
        heatingRequired = false;
    }

    char insideBuf[16], targetBuf[16], hystBuf[16];
    formatTemp(insideBuf, sizeof(insideBuf), readings.insideTemp);
    formatTemp(targetBuf, sizeof(targetBuf), targetTemperature);
    formatTemp(hystBuf, sizeof(hystBuf), hysteresis);
    LOG_DEBUG(TAG, "Heating required: %s (inside: %s, target: %s, hyst: %s)",
             heatingRequired ? "Yes" : "No", insideBuf, targetBuf, hystBuf);
    return heatingRequired;
}

void HeatingControlModule::startHeating() {
    LOG_INFO(TAG, "Starting heating...");
    xEventGroupSetBits(SRP::getSystemStateEventGroup(), SystemEvents::SystemState::HEATING_ON);
    // Add additional hardware control logic to start heating if needed
}

void HeatingControlModule::stopHeating() {
    LOG_INFO(TAG, "Stopping heating...");
    xEventGroupClearBits(SRP::getSystemStateEventGroup(), SystemEvents::SystemState::HEATING_ON);

    // Reset PID controller to prevent integral windup carryover
    resetPID();
}

void HeatingControlModule::resetPID() {
    if (fixedPointPID != nullptr) {
        fixedPointPID->reset();
        LOG_INFO(TAG, "PID controller reset");
    }
}

void HeatingControlModule::setPIDGains(float kp, float ki, float kd) {
    // Thread-safe PID gains update (Round 13 Issue #2)
    MutexGuard guard(pidGainsMutex_, pdMS_TO_TICKS(100));
    if (guard.hasLock()) {
        pidKp = kp;
        pidKi = ki;
        pidKd = kd;
        LOG_INFO(TAG, "Space heating PID gains updated: Kp=%.2f, Ki=%.4f, Kd=%.2f", kp, ki, kd);
    } else {
        LOG_ERROR(TAG, "Failed to acquire mutex for PID gains update");
    }
}

void HeatingControlModule::getPIDGains(float& kp, float& ki, float& kd) const {
    // Thread-safe PID gains read (Round 13 Issue #2)
    MutexGuard guard(pidGainsMutex_, pdMS_TO_TICKS(100));
    if (guard.hasLock()) {
        kp = pidKp;
        ki = pidKi;
        kd = pidKd;
    } else {
        // Return defaults if mutex fails
        kp = 1.0f;
        ki = 0.05f;
        kd = 0.2f;
    }
}

bool HeatingControlModule::isAutoTuningActive() const {
    // Thread-safe read (Round 13 Issue #3)
    MutexGuard guard(taskStateMutex_, pdMS_TO_TICKS(50));
    if (guard.hasLock()) {
        return isAutoTuning;
    }
    return false;  // Assume not tuning if mutex fails
}

void HeatingControlModule::startSpaceHeatingPIDTask() {
    // C1: Check if already running
    {
        MutexGuard guard(taskStateMutex_, pdMS_TO_TICKS(100));
        if (guard.hasLock()) {
            if (pidTaskRunning) {
                LOG_WARN(TAG, "SpaceHeatingPID task already running");
                return;
            }
            pidTaskStopRequested = false;
        }
    }

    // Use TaskManager for watchdog integration
    // NOTE: Watchdog disabled for this task - it has long wait intervals and
    // the feedWatchdog() calls are sufficient for software monitoring
    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::disabled();

    bool result = SRP::getTaskManager().startTaskPinned(
        SpaceHeatingPIDTask,
        "SpaceHeatingPID",
        STACK_SIZE_PID_CONTROL_TASK,  // Stack size from config
        this,  // Parameter
        5,     // Priority
        1,     // Core 1
        wdtConfig
    );

    if (!result) {
        LOG_ERROR(TAG, "Failed to create SpaceHeatingPID task");
    } else {
        // C1: Mark task as running
        MutexGuard guard(taskStateMutex_, pdMS_TO_TICKS(100));
        if (guard.hasLock()) {
            pidTaskRunning = true;
        }
        LOG_INFO(TAG, "SpaceHeatingPID task started with stop control enabled");
    }
}

void HeatingControlModule::stopSpaceHeatingPIDTask() {
    // C1: Request task stop and wait for it to exit
    {
        MutexGuard guard(taskStateMutex_, pdMS_TO_TICKS(100));
        if (guard.hasLock()) {
            if (!pidTaskRunning) {
                LOG_DEBUG(TAG, "SpaceHeatingPID task not running");
                return;
            }
            pidTaskStopRequested = true;
        } else {
            LOG_ERROR(TAG, "Failed to acquire mutex for stop request");
            return;
        }
    }

    LOG_INFO(TAG, "Waiting for SpaceHeatingPID task to stop...");

    // Wait for task to acknowledge stop (max 5 seconds)
    const uint32_t maxWaitMs = 5000;
    const uint32_t pollIntervalMs = 50;
    uint32_t elapsed = 0;

    while (elapsed < maxWaitMs) {
        vTaskDelay(pdMS_TO_TICKS(pollIntervalMs));
        elapsed += pollIntervalMs;

        MutexGuard guard(taskStateMutex_, pdMS_TO_TICKS(100));
        if (guard.hasLock() && !pidTaskRunning) {
            LOG_INFO(TAG, "SpaceHeatingPID task stopped successfully");
            return;
        }
    }

    // H3: Force cleanup after timeout to prevent undefined behavior
    LOG_ERROR(TAG, "SpaceHeatingPID task did not stop within timeout - forcing cleanup");
    {
        MutexGuard guard(taskStateMutex_, pdMS_TO_TICKS(100));
        if (guard.hasLock()) {
            // Mark as stopped even though task may still be running
            // This is a last resort to prevent destructor issues
            pidTaskRunning = false;
            pidTaskStopRequested = false;
            LOG_WARN(TAG, "Forced pidTaskRunning=false - task may still be executing (potential crash risk)");
        }
    }
    // Note: We don't call vTaskDelete() because:
    // 1. We don't have the task handle (TaskManager owns it)
    // 2. Force-deleting can leak memory and cause instability
    // The task will eventually exit on its next loop iteration when it checks pidTaskStopRequested
}

bool HeatingControlModule::isPIDTaskRunning() const {
    // C1: Thread-safe check of task running state
    MutexGuard guard(taskStateMutex_, pdMS_TO_TICKS(50));
    if (guard.hasLock()) {
        return pidTaskRunning;
    }
    return false;  // Assume not running if mutex fails
}

void HeatingControlModule::SpaceHeatingPIDTask(void* parameter) {
    auto* self = static_cast<HeatingControlModule*>(parameter);

    // CONSOLIDATED: Use room sensor interval for space heating PID (room temp is the primary input)
    const uint32_t sensorInterval = SRP::getRoomSensorReadInterval();  // ANDRTF3 interval
    const uint32_t pidInterval = sensorInterval * self->pidFactorSpaceHeating;
    const TickType_t effectivePidInterval = pdMS_TO_TICKS(pidInterval);

    // NOTE: NOT registering with ESP-IDF hardware watchdog.
    // This task has long wait intervals (5+ seconds) and uses software monitoring via feedWatchdog() calls.
    LOG_INFO(TAG, "SpaceHeatingPID task using software monitoring only");

    LOG_INFO(TAG, "SpaceHeatingPID task started, PID interval: %lu ms (sensor: %lu ms × factor: %d)",
             pidInterval, sensorInterval, (int)self->pidFactorSpaceHeating);

    // ============================================================================
    // THREAD-SAFETY NOTE (Round 13 Issue #4):
    // The following static variables are SAFE because they are only accessed by
    // SpaceHeatingPIDTask (single task context). They maintain state between loop
    // iterations without requiring mutex protection:
    //   - lastProgressPublish: Auto-tuning progress publish timing
    //   - wasRequestActive: Tracks previous request state for PID reset logic
    //   - lastTargetTemp: Tracks previous target for setpoint change detection
    //   - lastPIDTime: Time delta calculation for PID controller
    //   - lastAdjustment: PID deadband filtering
    //   - lastSaveTime: Periodic PID state persistence timing
    // DO NOT access these variables from other tasks.
    // ============================================================================

    while (true) {
        // C1: Check for stop request at start of each iteration
        {
            MutexGuard guard(self->taskStateMutex_, pdMS_TO_TICKS(50));
            if (guard.hasLock() && self->pidTaskStopRequested) {
                LOG_INFO(TAG, "SpaceHeatingPID task received stop request - exiting");
                self->pidTaskRunning = false;
                vTaskDelete(nullptr);  // Delete this task
                return;  // Never reached, but satisfies compiler
            }
        }

        // Wait for room temperature update (INSIDE from ANDRTF3) or timeout
        // Space heating PID uses room temperature as primary input
        // Use shorter wait to ensure more frequent watchdog feeds
        const TickType_t maxWaitTime = pdMS_TO_TICKS(sensorInterval / 2);  // Half sensor interval for safety
        const TickType_t waitTimeout = (effectivePidInterval < maxWaitTime) ? effectivePidInterval : maxWaitTime;

        xEventGroupWaitBits(SRP::getSensorEventGroup(), SystemEvents::SensorUpdate::INSIDE, pdFALSE, pdTRUE, waitTimeout);

        // Feed watchdog immediately after wait
        (void)SRP::getTaskManager().feedWatchdog();
        
        // Check for auto-tuning requests
        EventBits_t controlBits = SRP::getControlRequestsEventBits();
        if (controlBits & SystemEvents::ControlRequest::PID_AUTOTUNE) {
            // Clear the request bit
            SRP::clearControlRequestsEventBits(SystemEvents::ControlRequest::PID_AUTOTUNE);
            
            // Get current inside temperature as starting point
            Temperature_t currentTemp = tempFromFloat(20.0f);  // Default
            {
                auto guard = MutexRetryHelper::acquireGuard(
                    SRP::getSensorReadingsMutex(),
                    "SensorReadings-AutoTune"
                );
                if (guard) {
                    currentTemp = SRP::getSensorReadings().insideTemp;
                }
            }
            
            // Start auto-tuning with target 10°C above current
            Temperature_t tuneSetpoint = tempAdd(currentTemp, SystemConstants::Temperature::AUTOTUNE_TEMP_INCREMENT_C);
            if (tuneSetpoint > SystemConstants::Temperature::AUTOTUNE_MAX_SETPOINT_C) {
                tuneSetpoint = SystemConstants::Temperature::AUTOTUNE_MAX_SETPOINT_C;  // Safety limit
            }
            
            self->startAutoTuning(tuneSetpoint);
        }
        
        // Check for stop auto-tuning request
        if (controlBits & SystemEvents::ControlRequest::PID_AUTOTUNE_STOP) {
            SRP::clearControlRequestsEventBits(SystemEvents::ControlRequest::PID_AUTOTUNE_STOP);
            self->stopAutoTuning();
        }
        
        // Check for PID save request
        if (controlBits & SystemEvents::ControlRequest::PID_SAVE) {
            SRP::clearControlRequestsEventBits(SystemEvents::ControlRequest::PID_SAVE);
            if (self->fixedPointPID && self->fixedPointPID->saveState(PID_CONTROLLER_SPACE_HEATING)) {
                LOG_INFO(TAG, "PID state saved to FRAM on request");
            }
        }
        
        // Handle auto-tuning mode (Round 13 Issue #3: Thread-safe check)
        bool currentlyAutoTuning = false;
        {
            MutexGuard guard(self->taskStateMutex_, pdMS_TO_TICKS(50));
            if (guard.hasLock()) {
                currentlyAutoTuning = self->isAutoTuning;
            }
        }

        if (currentlyAutoTuning) {
            // Get current time in milliseconds
            uint32_t currentTimeMs = millis();
            Temperature_t boilerTemp = 0;
            bool sensorValid = false;

            {
                auto guard = MutexRetryHelper::acquireGuard(
                    SRP::getSensorReadingsMutex(),
                    "SensorReadings-AutoTune"
                );
                if (guard) {
                    const SharedSensorReadings& readings = SRP::getSensorReadings();
                    if (readings.isBoilerTempOutputValid) {
                        boilerTemp = readings.boilerTempOutput;
                        sensorValid = true;
                    }
                }
            }

            // Safety check: abort if sensor invalid or stale during tuning
            if (!sensorValid) {
                LOG_ERROR(TAG, "Auto-tuning aborted: boiler temp sensor invalid");
                self->stopAutoTuning();
                MQTTTask::publish("status/boiler/pid_autotune/results",
                    "{\"state\":\"failed\",\"reason\":\"sensor_invalid\"}", 0, true);
                (void)SRP::getTaskManager().feedWatchdog();
                vTaskDelay(effectivePidInterval);
                continue;
            }

            // Safety check: abort if sensor data is stale (critical for accurate tuning)
            if (StateManager::isSensorStale(StateManager::SensorChannel::BOILER_OUTPUT)) {
                uint32_t age = StateManager::getSensorAge(StateManager::SensorChannel::BOILER_OUTPUT);
                LOG_ERROR(TAG, "Auto-tuning aborted: sensor data stale (%lu ms)", age);
                self->stopAutoTuning();
                MQTTTask::publish("status/boiler/pid_autotune/results",
                    "{\"state\":\"failed\",\"reason\":\"sensor_stale\"}", 0, true);
                (void)SRP::getTaskManager().feedWatchdog();
                vTaskDelay(effectivePidInterval);
                continue;
            }

            // Safety check: abort if temperature exceeds safe limit
            if (boilerTemp > SystemConstants::PID::Autotune::MAX_TEMP_EXCURSION) {
                char tempBuf[16];
                formatTemp(tempBuf, sizeof(tempBuf), boilerTemp);
                LOG_ERROR(TAG, "Auto-tuning ABORTED: temperature excursion! %s°C > 80°C limit",
                         tempBuf);
                self->stopAutoTuning();
                // Turn off burner immediately
                SRP::setBurnerRequestEventBits(0);
                char buffer[128];
                int written = snprintf(buffer, sizeof(buffer),
                    "{\"state\":\"failed\",\"reason\":\"temp_excursion\",\"temp\":%s}", tempBuf);
                // M1: Check for buffer overflow
                if (written < 0 || static_cast<size_t>(written) >= sizeof(buffer)) {
                    LOG_WARN(TAG, "Autotune excursion JSON truncated");
                }
                MQTTTask::publish("status/boiler/pid_autotune/results", buffer, 0, true);
                (void)SRP::getTaskManager().feedWatchdog();
                vTaskDelay(effectivePidInterval);
                continue;
            }

            // Update auto-tuner (PIDAutoTuner still uses float, so convert time to seconds)
            float currentTimeSec = static_cast<float>(currentTimeMs) / 1000.0f;
            float output = self->autoTuner->update(tempToFloat(boilerTemp), currentTimeSec);
            
            // Convert output to burner control
            // Use encode_temperature_t for Temperature_t to avoid negative-to-unsigned cast issues
            EventBits_t burnerRequest = SystemEvents::BurnerRequest::HEATING |
                                       SystemEvents::BurnerRequest::encode_temperature_t(self->autoTuneSetpoint);
            
            if (output > 0) {
                // Positive output = burner ON
                if (output > SystemConstants::PID::POWER_THRESHOLD_LOW_HIGH) {
                    burnerRequest |= SystemEvents::BurnerRequest::POWER_HIGH;
                } else {
                    burnerRequest |= SystemEvents::BurnerRequest::POWER_LOW;
                }
            } else {
                // Negative output = burner OFF
                burnerRequest = 0;  // Clear all request bits
            }
            
            SRP::setBurnerRequestEventBits(burnerRequest);
            
            // Check if tuning is complete
            if (self->autoTuner->isComplete()) {
                // Thread-safe state update (Round 13 Issue #3)
                {
                    MutexGuard guard(self->taskStateMutex_, pdMS_TO_TICKS(100));
                    if (guard.hasLock()) {
                        self->isAutoTuning = false;
                    }
                }
                xEventGroupClearBits(SRP::getHeatingEventGroup(), SystemEvents::HeatingEvent::AUTOTUNE_RUNNING);

                // Get and validate results before applying
                auto results = self->autoTuner->getResults();
                if (results.valid) {
                    // Validate tuning results are within acceptable ranges
                    bool paramsValid = true;
                    const char* validationError = nullptr;

                    if (results.Kp < SystemConstants::PID::Autotune::MIN_VALID_KP ||
                        results.Kp > SystemConstants::PID::Autotune::MAX_VALID_KP) {
                        paramsValid = false;
                        validationError = "Kp out of range";
                    } else if (results.Ki < SystemConstants::PID::Autotune::MIN_VALID_KI ||
                               results.Ki > SystemConstants::PID::Autotune::MAX_VALID_KI) {
                        paramsValid = false;
                        validationError = "Ki out of range";
                    } else if (results.Kd < SystemConstants::PID::Autotune::MIN_VALID_KD ||
                               results.Kd > SystemConstants::PID::Autotune::MAX_VALID_KD) {
                        paramsValid = false;
                        validationError = "Kd out of range";
                    } else if (results.ultimateGain <= SystemConstants::PID::Autotune::MIN_VALID_KU ||
                               results.ultimateGain > SystemConstants::PID::Autotune::MAX_VALID_KU) {
                        paramsValid = false;
                        validationError = "Ku out of range";
                    } else if (results.ultimatePeriod < SystemConstants::PID::Autotune::MIN_VALID_TU ||
                               results.ultimatePeriod > SystemConstants::PID::Autotune::MAX_VALID_TU) {
                        paramsValid = false;
                        validationError = "Tu out of range";
                    }

                    if (paramsValid) {
                        xEventGroupSetBits(SRP::getHeatingEventGroup(), SystemEvents::HeatingEvent::AUTOTUNE_COMPLETE);
                        LOG_INFO(TAG, "Auto-tuning complete! Kp=%.2f, Ki=%.4f, Kd=%.2f",
                                results.Kp, results.Ki, results.Kd);

                        // Apply new parameters via PIDControlModule
                        PIDControlModule* pidController = SRP::getPIDControl();
                        if (pidController) {
                            pidController->setParameters(results.Kp, results.Ki, results.Kd);

                            // Save to persistent storage in FRAM
                            CriticalDataStorage::savePIDTuning(0, results.Kp, results.Ki, results.Kd,
                                                              -100.0f, 100.0f, true);
                            LOG_INFO(TAG, "PID tuning saved to FRAM");
                        }

                        // Publish results via MQTT
                        char buffer[SystemConstants::Buffers::MEDIUM_JSON_BUFFER_SIZE];
                        int written = snprintf(buffer, sizeof(buffer),
                                "{\"state\":\"complete\",\"kp\":%.2f,\"ki\":%.4f,\"kd\":%.2f,"
                                "\"ku\":%.2f,\"tu\":%.1f}",
                                results.Kp, results.Ki, results.Kd,
                                results.ultimateGain, results.ultimatePeriod);
                        // M1: Check for buffer overflow
                        if (written < 0 || static_cast<size_t>(written) >= sizeof(buffer)) {
                            LOG_WARN(TAG, "Autotune complete JSON truncated");
                        }
                        MQTTTask::publish("status/boiler/pid_autotune/results", buffer, 0, true);
                    } else {
                        // Results out of acceptable range - don't apply
                        xEventGroupSetBits(SRP::getHeatingEventGroup(), SystemEvents::HeatingEvent::AUTOTUNE_FAILED);
                        LOG_ERROR(TAG,
                            "Auto-tuning results rejected: %s (Kp=%.2f, Ki=%.4f, Kd=%.2f, Ku=%.2f, Tu=%.1f)",
                            validationError, results.Kp, results.Ki, results.Kd,
                            results.ultimateGain, results.ultimatePeriod);

                        char buffer[SystemConstants::Buffers::MEDIUM_JSON_BUFFER_SIZE];
                        int written = snprintf(buffer, sizeof(buffer),
                            "{\"state\":\"failed\",\"reason\":\"validation\",\"error\":\"%s\","
                            "\"kp\":%.2f,\"ki\":%.4f,\"kd\":%.2f,\"ku\":%.2f,\"tu\":%.1f}",
                            validationError, results.Kp, results.Ki, results.Kd,
                            results.ultimateGain, results.ultimatePeriod);
                        // M1: Check for buffer overflow
                        if (written < 0 || static_cast<size_t>(written) >= sizeof(buffer)) {
                            LOG_WARN(TAG, "Autotune failed JSON truncated");
                        }
                        MQTTTask::publish("status/boiler/pid_autotune/results", buffer, 0, true);
                    }
                } else {
                    LOG_ERROR(TAG, "Auto-tuning failed - invalid results");
                    xEventGroupSetBits(SRP::getHeatingEventGroup(), SystemEvents::HeatingEvent::AUTOTUNE_FAILED);
                    MQTTTask::publish("status/boiler/pid_autotune/results",
                        "{\"state\":\"failed\",\"reason\":\"invalid_results\"}", 0, true);
                }
            }
            
            // Check for failure conditions
            if (self->autoTuner->getState() == PIDAutoTuner::TuningState::FAILED) {
                // Thread-safe state update (Round 13 Issue #3)
                {
                    MutexGuard guard(self->taskStateMutex_, pdMS_TO_TICKS(100));
                    if (guard.hasLock()) {
                        self->isAutoTuning = false;
                    }
                }
                xEventGroupClearBits(SRP::getHeatingEventGroup(), SystemEvents::HeatingEvent::AUTOTUNE_RUNNING);
                xEventGroupSetBits(SRP::getHeatingEventGroup(), SystemEvents::HeatingEvent::AUTOTUNE_FAILED);
                LOG_ERROR(TAG, "Auto-tuning failed: %s",
                         self->autoTuner->getStatusMessage());
            }
            
            // Publish progress periodically with detailed information
            // THREAD-SAFE: Only accessed by HeatingControlTask (single task)
            static uint32_t lastProgressPublish = 0;
            uint32_t now = millis();
            if (now - lastProgressPublish > SystemConstants::PID::PROGRESS_PUBLISH_INTERVAL_MS) {
                lastProgressPublish = now;
                uint8_t progress = self->autoTuner->getProgress();
                uint8_t cycles = self->autoTuner->getCycleCount();
                uint8_t minCycles = self->autoTuner->getMinCycles();
                float elapsedTime = self->autoTuner->getElapsedTime();
                float maxTime = self->autoTuner->getMaxTuningTime();
                char progressBuffer[SystemConstants::Buffers::MEDIUM_JSON_BUFFER_SIZE];
                int written = snprintf(progressBuffer, sizeof(progressBuffer),
                        "{\"progress\":%d,\"state\":\"running\",\"cycles\":%d,\"minCycles\":%d,"
                        "\"elapsedSec\":%.0f,\"maxSec\":%.0f}",
                        progress, cycles, minCycles, elapsedTime, maxTime);
                // M1: Check for buffer overflow
                if (written < 0 || static_cast<size_t>(written) >= sizeof(progressBuffer)) {
                    LOG_WARN(TAG, "Autotune progress JSON truncated");
                }
                MQTTTask::publish("status/boiler/pid_autotune/progress", progressBuffer, 0, false);
            }

            (void)SRP::getTaskManager().feedWatchdog();
            vTaskDelay(effectivePidInterval);
            continue;  // Skip normal PID control during auto-tuning
        }

        // Check if any burner request is active
        EventBits_t currentRequest = SRP::getBurnerRequestEventBits();

        // Track previous state for PID reset logic
        // THREAD-SAFE: Only accessed by HeatingControlTask (single task)
        static bool wasRequestActive = false;
        static Temperature_t lastTargetTemp = 0;
        const Temperature_t SIGNIFICANT_SETPOINT_CHANGE = tempFromWhole(5);  // 5°C threshold

        bool isRequestActive = (currentRequest & SystemEvents::BurnerRequest::ANY_REQUEST) != 0;

        if (!isRequestActive) {
            // No burner request active - mark state and wait
            wasRequestActive = false;
            (void)SRP::getTaskManager().feedWatchdog();
            vTaskDelay(effectivePidInterval);
            continue;
        }

        // Extract target temperature for comparison
        Temperature_t currentTargetTemp = (Temperature_t)SystemEvents::BurnerRequest::decode_temperature(currentRequest);

        // Reset PID integral when:
        // 1. Burner transitions from OFF to ON (prevents integral windup carryover)
        // 2. Setpoint changes significantly (prevents overshoot from stale integral)
        if (self->fixedPointPID != nullptr) {
            bool shouldReset = false;
            const char* resetReason = nullptr;

            if (!wasRequestActive && isRequestActive) {
                shouldReset = true;
                resetReason = "burner activated";
            } else if (wasRequestActive && lastTargetTemp != 0) {
                Temperature_t tempDiff = tempAbs(tempSub(currentTargetTemp, lastTargetTemp));
                if (tempDiff > SIGNIFICANT_SETPOINT_CHANGE) {
                    shouldReset = true;
                    resetReason = "setpoint changed >5C";
                }
            }

            if (shouldReset) {
                self->fixedPointPID->reset();
                LOG_INFO(TAG, "PID integral reset: %s", resetReason);
            }
        }

        // Update tracking state
        wasRequestActive = isRequestActive;
        lastTargetTemp = currentTargetTemp;

        // Get current boiler output temperature with staleness validation
        Temperature_t boilerOutputTemp = 0;
        bool sensorDataValid = false;

        // Use StateManager for staleness and validity checks
        if (StateManager::isSensorStale(StateManager::SensorChannel::BOILER_OUTPUT)) {
            LOG_ERROR(TAG, "Sensor data too old: %lu ms",
                      StateManager::getSensorAge(StateManager::SensorChannel::BOILER_OUTPUT));
        } else if (!StateManager::isSensorValid(StateManager::SensorChannel::BOILER_OUTPUT)) {
            LOG_WARN(TAG, "Boiler output temp sensor invalid");
        } else {
            SharedSensorReadings readings = StateManager::getSensorReadingsCopy();
            boilerOutputTemp = readings.boilerTempOutput;
            sensorDataValid = true;
        }

        if (!sensorDataValid) {
            ErrorHandler::logError(TAG, SystemError::SENSOR_INVALID_DATA,
                                  "Cannot proceed with PID - sensor data unavailable or stale");
            // Reset PID integral to prevent windup during sensor outage
            if (self->fixedPointPID != nullptr) {
                self->fixedPointPID->reset();
                LOG_INFO(TAG, "PID integral reset: sensor data invalid");
            }
            (void)SRP::getTaskManager().feedWatchdog();
            vTaskDelay(effectivePidInterval);
            continue;
        }

        // Skip if no valid target temperature (using currentTargetTemp from earlier extraction)
        if (currentTargetTemp < SystemConstants::Temperature::MIN_VALID_TARGET_TEMP_C) {
            (void)SRP::getTaskManager().feedWatchdog();
            vTaskDelay(effectivePidInterval);
            continue;
        }

        // Use fixed-point PID controller to calculate adjustment
        Temperature_t adjustmentFixed = 0;
        float adjustment = 0.0f;
        
        if (self->fixedPointPID != nullptr) {
            // Convert float PID parameters to fixed-point
            // Round 13 Issue #2: Use cached gains with mutex protection
            PIDValue_t kp, ki, kd;
            if (currentRequest & SystemEvents::BurnerRequest::HEATING) {
                float localKp, localKi, localKd;
                {
                    MutexGuard guard(self->pidGainsMutex_, pdMS_TO_TICKS(100));
                    if (guard.hasLock()) {
                        localKp = self->pidKp;
                        localKi = self->pidKi;
                        localKd = self->pidKd;
                    } else {
                        // Use defaults if mutex fails
                        localKp = 1.0f;
                        localKi = 0.05f;
                        localKd = 0.2f;
                    }
                }
                HeatingControlModuleFixedPoint::convertPIDParameters(
                    localKp, localKi, localKd,
                    kp, ki, kd
                );
            } else {
                // Water heating - use simpler on/off control (no PID)
                HeatingControlModuleFixedPoint::convertPIDParameters(
                    1.0f, 0.0f, 0.0f,
                    kp, ki, kd
                );
            }
            
            // Calculate time delta in milliseconds
            // THREAD-SAFE: Only accessed by HeatingControlTask (single task)
            // C5: Tick subtraction handles wraparound via unsigned modular arithmetic
            static TickType_t lastPIDTime = 0;
            TickType_t currentTime = xTaskGetTickCount();
            TickType_t tickDelta = currentTime - lastPIDTime;  // Handles wraparound correctly
            uint32_t dtMs = pdTICKS_TO_MS(tickDelta);

            // C5: Clamp to reasonable max (10 seconds) to handle edge cases
            constexpr uint32_t MAX_DT_MS = 10000;
            if (dtMs == 0) {
                dtMs = SystemConstants::PID::DEFAULT_TIME_DELTA_MS;
            } else if (dtMs > MAX_DT_MS) {
                LOG_WARN(TAG, "PID time delta clamped: %lu ms -> %lu ms", dtMs, MAX_DT_MS);
                dtMs = MAX_DT_MS;
            }
            lastPIDTime = currentTime;
            
            // Calculate adjustment using fixed-point PID
            adjustmentFixed = self->fixedPointPID->calculatePIDAdjustment(
                currentTargetTemp,
                boilerOutputTemp,
                kp, ki, kd,
                dtMs
            );

            // Convert fixed-point adjustment to float for compatibility
            adjustment = tempToFloat(adjustmentFixed);

            char targetBuf[SystemConstants::Buffers::TEMP_FORMAT_BUFFER_SIZE],
                 currentBuf[SystemConstants::Buffers::TEMP_FORMAT_BUFFER_SIZE],
                 adjustBuf[SystemConstants::Buffers::TEMP_FORMAT_BUFFER_SIZE];
            formatTemp(targetBuf, sizeof(targetBuf), currentTargetTemp);
            formatTemp(currentBuf, sizeof(currentBuf), boilerOutputTemp);
            formatTemp(adjustBuf, sizeof(adjustBuf), adjustmentFixed);
            LOG_DEBUG(TAG, "Fixed-point PID - Target: %s, Current: %s, Adjustment: %s (%.2f°C)", 
                     targetBuf, currentBuf, adjustBuf, adjustment);
        } else {
            ErrorHandler::logError(TAG, SystemError::NOT_INITIALIZED, "Fixed-point PID controller not available");
            (void)SRP::getTaskManager().feedWatchdog();
            vTaskDelay(effectivePidInterval);
            continue;
        }

        // Convert PID adjustment to power level
        uint32_t adjustmentLevel = PIDControlModule::determineAdjustmentLevel(adjustment);
        
        // Apply PID deadband to reduce minor fluctuations
        // THREAD-SAFE: Only accessed by HeatingControlTask (single task)
        static float lastAdjustment = 0.0f;
        if (!BurnerAntiFlapping::isSignificantPIDChange(lastAdjustment, adjustment)) {
            // Change is within deadband, keep current state
            (void)SRP::getTaskManager().feedWatchdog();
            vTaskDelay(effectivePidInterval);
            continue;
        }
        lastAdjustment = adjustment;
        
        // Round 13 Issue #1: Use atomic update to prevent race condition
        // Previous code used non-atomic read-modify-write which could race with WaterHeatingPIDTask
        EventBits_t powerBitsToSet = 0;
        EventBits_t clearBits = SystemEvents::BurnerRequest::POWER_BITS;

        if (adjustmentLevel >= SystemConstants::PID::MIN_ADJUSTMENT_LEVEL_FOR_ON) {
            // Burner should be ON - set appropriate power level
            if (adjustmentLevel >= SystemConstants::PID::MIN_ADJUSTMENT_LEVEL_FOR_HIGH) {
                powerBitsToSet = SystemEvents::BurnerRequest::POWER_HIGH;
                LOG_DEBUG(TAG, "PID level %lu -> HIGH power", adjustmentLevel);
            } else {
                powerBitsToSet = SystemEvents::BurnerRequest::POWER_LOW;
                LOG_DEBUG(TAG, "PID level %lu -> LOW power", adjustmentLevel);
            }

            // Use atomic update to prevent race condition with WaterHeatingPIDTask
            BurnerRequestManager::atomicUpdateBits(powerBitsToSet, clearBits);
        } else {
            // PID says no heating needed - clear power bits atomically
            LOG_DEBUG(TAG, "PID level %lu -> Burner OFF (below threshold)", adjustmentLevel);
            BurnerRequestManager::atomicUpdateBits(0, clearBits);
        }

        // Save PID state periodically (every 5 minutes)
        // THREAD-SAFE: Only accessed by HeatingControlTask (single task)
        static TickType_t lastSaveTime = 0;
        TickType_t currentTickTime = xTaskGetTickCount();
        if (currentTickTime - lastSaveTime > pdMS_TO_TICKS(5 * 60 * 1000)) {
            if (self->fixedPointPID && self->fixedPointPID->saveState(PID_CONTROLLER_SPACE_HEATING)) {
                LOG_DEBUG(TAG, "PID state saved to FRAM");
                lastSaveTime = currentTickTime;
            }
        }

        // Feed watchdog before delay
        (void)SRP::getTaskManager().feedWatchdog();
        vTaskDelay(effectivePidInterval);
    }
}
