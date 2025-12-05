// src/modules/control/WheaterControlModule.cpp
#include "modules/control/WheaterControlModule.h"
#include "modules/control/PIDControlModule.h"
#include "modules/control/BurnerAntiFlapping.h"
#include "modules/control/BurnerRequestManager.h"
#include "events/SystemEventsGenerated.h"
#include "config/SystemConstants.h"
#include "config/ProjectConfig.h"
#include "utils/ErrorHandler.h"
#include "utils/MutexGuard.h"
#include "LoggingMacros.h"
#include <TaskManager.h>

WheaterControlModule::WheaterControlModule() {
    // Mutexes and PID controller will be created in initialize()
}

WheaterControlModule::~WheaterControlModule() {
    stopWaterHeatingPIDTask();

    if (fixedPointPID != nullptr) {
        delete fixedPointPID;
        fixedPointPID = nullptr;
    }

    // Cleanup mutexes
    if (taskStateMutex_ != nullptr) {
        vSemaphoreDelete(taskStateMutex_);
        taskStateMutex_ = nullptr;
    }
    if (pidGainsMutex_ != nullptr) {
        vSemaphoreDelete(pidGainsMutex_);
        pidGainsMutex_ = nullptr;
    }
    if (pidStateMutex_ != nullptr) {
        vSemaphoreDelete(pidStateMutex_);
        pidStateMutex_ = nullptr;
    }
}

void WheaterControlModule::initialize() {
    // Create thread safety mutexes (Round 12 Issue #1, #2, #7)
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
    if (pidStateMutex_ == nullptr) {
        pidStateMutex_ = xSemaphoreCreateMutex();
        if (!pidStateMutex_) {
            LOG_ERROR(TAG, "Failed to create PID state mutex");
        }
    }

    // Create fixed-point PID controller
    if (fixedPointPID == nullptr) {
        fixedPointPID = new PIDControlModuleFixedPoint();
        LOG_INFO(TAG, "Water heating PID controller initialized");
    }

    // Load PID gains from settings
    SystemSettings& settings = SRP::getSystemSettings();
    pidKp = settings.wHeaterKp;
    pidKi = settings.wHeaterKi;
    pidKd = settings.wHeaterKd;
    LOG_INFO(TAG, "Loaded water PID gains: Kp=%.2f, Ki=%.2f, Kd=%.2f", pidKp, pidKi, pidKd);
}

Temperature_t WheaterControlModule::calculateWaterHeatingTargetTemp(
    const SharedSensorReadings& readings,
    const SystemSettings& settings) const {

    // Water heating target = tank temperature + charge delta
    // Boiler needs to be 5-10°C hotter than tank to charge it effectively

    Temperature_t chargeDelta = tempFromFloat(settings.wHeaterConfTempChargeDelta);
    Temperature_t targetTemp = tempAdd(readings.waterHeaterTempTank, chargeDelta);

    char tankBuf[16], deltaBuf[16], targetBuf[16];
    formatTemp(tankBuf, sizeof(tankBuf), readings.waterHeaterTempTank);
    formatTemp(deltaBuf, sizeof(deltaBuf), chargeDelta);
    formatTemp(targetBuf, sizeof(targetBuf), targetTemp);
    LOG_DEBUG(TAG, "Water target: tank %s + delta %s = %s", tankBuf, deltaBuf, targetBuf);

    // Apply safety limits (from SystemConstants)
    const Temperature_t MIN_TARGET = SystemConstants::WaterHeating::MIN_TARGET_TEMP;
    const Temperature_t MAX_TARGET = SystemConstants::WaterHeating::MAX_TARGET_TEMP;

    if (targetTemp < MIN_TARGET) {
        targetTemp = MIN_TARGET;
    } else if (targetTemp > MAX_TARGET) {
        targetTemp = MAX_TARGET;
    }

    return targetTemp;
}

void WheaterControlModule::startWaterHeatingPIDTask() {
    // Thread-safe check and start (Round 12 Issue #1)
    {
        MutexGuard guard(taskStateMutex_, pdMS_TO_TICKS(100));
        if (!guard.hasLock()) {
            LOG_ERROR(TAG, "Failed to acquire task state mutex for start");
            return;
        }

        if (pidTaskRunning) {
            LOG_DEBUG(TAG, "Water heating PID task already running");
            return;
        }

        pidTaskStopRequested = false;
    }

    // Reset PID to start fresh
    resetPID();

    // Use TaskManager for task creation
    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::disabled();

    bool result = SRP::getTaskManager().startTaskPinned(
        WaterHeatingPIDTask,
        "WaterHeatingPID",
        STACK_SIZE_PID_CONTROL_TASK,
        this,
        5,     // Priority (same as SpaceHeatingPID)
        1,     // Core 1
        wdtConfig
    );

    // Update state with mutex protection
    {
        MutexGuard guard(taskStateMutex_, pdMS_TO_TICKS(100));
        if (guard.hasLock()) {
            if (result) {
                pidTaskRunning = true;
                LOG_INFO(TAG, "Water heating PID task started");
            } else {
                LOG_ERROR(TAG, "Failed to create water heating PID task");
            }
        }
    }
}

void WheaterControlModule::stopWaterHeatingPIDTask() {
    // Thread-safe stop request (Round 12 Issue #1)
    bool wasRunning = false;
    {
        MutexGuard guard(taskStateMutex_, pdMS_TO_TICKS(100));
        if (!guard.hasLock()) {
            LOG_ERROR(TAG, "Failed to acquire task state mutex for stop");
            return;
        }

        if (!pidTaskRunning && pidTaskHandle == nullptr) {
            return;
        }

        wasRunning = pidTaskRunning;
        pidTaskStopRequested = true;
    }

    if (wasRunning) {
        LOG_INFO(TAG, "Stopping water heating PID task...");

        // Round 16 Issue A: Wait for task to acknowledge stop
        // Must wait longer than PID_INTERVAL_MS (2000ms) + margin since task may be
        // blocked in vTaskDelay. Using 2500ms (25 × 100ms) to ensure task has time
        // to wake up and check the stop flag.
        const int maxIterations = 25;  // 25 × 100ms = 2500ms max wait
        for (int i = 0; i < maxIterations; i++) {
            {
                MutexGuard guard(taskStateMutex_, pdMS_TO_TICKS(50));
                if (guard.hasLock() && !pidTaskRunning) {
                    LOG_INFO(TAG, "Water heating PID task stopped successfully after %d ms", i * 100);
                    return;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        // Force cleanup if task didn't stop gracefully
        MutexGuard guard(taskStateMutex_, pdMS_TO_TICKS(100));
        if (guard.hasLock() && pidTaskRunning) {
            LOG_WARN(TAG, "Water heating PID task did not stop gracefully after %d ms", maxIterations * 100);
            pidTaskRunning = false;
            pidTaskHandle = nullptr;
        }
    }
}

bool WheaterControlModule::isPIDTaskRunning() const {
    // Thread-safe read (Round 12 Issue #1)
    MutexGuard guard(taskStateMutex_, pdMS_TO_TICKS(50));
    if (guard.hasLock()) {
        return pidTaskRunning;
    }
    return false;  // Assume not running if mutex fails
}

void WheaterControlModule::resetPID() {
    // Thread-safe PID reset (Round 12 Issue #7)
    MutexGuard guard(pidStateMutex_, pdMS_TO_TICKS(100));
    if (guard.hasLock() && fixedPointPID != nullptr) {
        fixedPointPID->reset();
        LOG_DEBUG(TAG, "Water heating PID controller reset");
    }
}

void WheaterControlModule::setPIDGains(float kp, float ki, float kd) {
    // Thread-safe PID gains update (Round 12 Issue #2)
    MutexGuard guard(pidGainsMutex_, pdMS_TO_TICKS(100));
    if (guard.hasLock()) {
        pidKp = kp;
        pidKi = ki;
        pidKd = kd;
        LOG_INFO(TAG, "Water PID gains updated: Kp=%.2f, Ki=%.2f, Kd=%.2f", kp, ki, kd);
    } else {
        LOG_ERROR(TAG, "Failed to acquire mutex for PID gains update");
    }
}

void WheaterControlModule::getPIDGains(float& kp, float& ki, float& kd) const {
    // Thread-safe PID gains read (Round 12 Issue #2)
    MutexGuard guard(pidGainsMutex_, pdMS_TO_TICKS(100));
    if (guard.hasLock()) {
        kp = pidKp;
        ki = pidKi;
        kd = pidKd;
    } else {
        // Return defaults if mutex fails
        kp = 2.0f;
        ki = 0.1f;
        kd = 0.5f;
    }
}

void WheaterControlModule::WaterHeatingPIDTask(void* parameter) {
    auto* self = static_cast<WheaterControlModule*>(parameter);

    // PID interval - water heating responds faster than space heating (from SystemConstants)
    const uint32_t pidIntervalMs = SystemConstants::WaterHeating::PID_INTERVAL_MS;
    const TickType_t pidInterval = pdMS_TO_TICKS(pidIntervalMs);

    LOG_INFO(self->TAG, "Water heating PID task started, interval: %lu ms", pidIntervalMs);

    // Track time for derivative calculation
    TickType_t lastPIDTime = xTaskGetTickCount();
    float lastAdjustment = 0.0f;

    // Helper to check stop request with mutex
    auto shouldStop = [self]() -> bool {
        MutexGuard guard(self->taskStateMutex_, pdMS_TO_TICKS(50));
        return guard.hasLock() ? self->pidTaskStopRequested : true;
    };

    while (!shouldStop()) {
        // Check if water heating is still active
        EventBits_t systemBits = SRP::getSystemStateEventBits();
        if (!(systemBits & SystemEvents::SystemState::WATER_ON)) {
            // Water heating not active - wait and check again
            vTaskDelay(pdMS_TO_TICKS(SystemConstants::WaterHeating::WAIT_INTERVAL_MS));
            continue;
        }

        // Get current burner request to check if water is actually requesting
        EventBits_t requestBits = BurnerRequestManager::getCurrentRequests();
        if (!(requestBits & SystemEvents::BurnerRequest::WATER)) {
            // No water request active
            vTaskDelay(pdMS_TO_TICKS(SystemConstants::WaterHeating::WAIT_INTERVAL_MS));
            continue;
        }

        // Get target temperature from burner request
        Temperature_t targetTemp = BurnerRequestManager::getCurrentTargetTemp();

        // Get current boiler output temperature
        Temperature_t boilerOutputTemp = 0;
        bool sensorValid = false;

        if (SRP::takeSensorReadingsMutex(pdMS_TO_TICKS(100))) {
            SharedSensorReadings readings = SRP::getSensorReadings();
            if (readings.isBoilerTempOutputValid) {
                boilerOutputTemp = readings.boilerTempOutput;
                sensorValid = true;
            }
            SRP::giveSensorReadingsMutex();
        }

        if (!sensorValid) {
            // Round 12 Issue #6: Reset PID to prevent integral windup during sensor failure
            LOG_WARN(self->TAG, "Boiler output sensor unavailable - resetting PID to prevent windup");
            {
                MutexGuard guard(self->pidStateMutex_, pdMS_TO_TICKS(100));
                if (guard.hasLock() && self->fixedPointPID != nullptr) {
                    self->fixedPointPID->reset();
                }
            }
            vTaskDelay(pidInterval);
            continue;
        }

        // Calculate time delta for PID
        TickType_t currentTime = xTaskGetTickCount();
        uint32_t dtMs = pdTICKS_TO_MS(currentTime - lastPIDTime);
        if (dtMs == 0) dtMs = pidIntervalMs;
        lastPIDTime = currentTime;

        // Get PID gains with mutex protection (Round 12 Issue #2)
        float localKp, localKi, localKd;
        {
            MutexGuard guard(self->pidGainsMutex_, pdMS_TO_TICKS(100));
            if (guard.hasLock()) {
                localKp = self->pidKp;
                localKi = self->pidKi;
                localKd = self->pidKd;
            } else {
                // Use defaults if mutex fails
                localKp = 2.0f;
                localKi = 0.1f;
                localKd = 0.5f;
            }
        }

        // Calculate PID adjustment with mutex protection (Round 12 Issue #7)
        Temperature_t adjustmentFixed = 0;
        {
            MutexGuard guard(self->pidStateMutex_, pdMS_TO_TICKS(100));
            if (guard.hasLock() && self->fixedPointPID != nullptr) {
                adjustmentFixed = self->fixedPointPID->calculatePIDAdjustment(
                    targetTemp,
                    boilerOutputTemp,
                    localKp,
                    localKi,
                    localKd,
                    dtMs
                );
            }
        }

        float adjustment = tempToFloat(adjustmentFixed);

        // Log PID values
        char targetBuf[16], currentBuf[16], adjustBuf[16];
        formatTemp(targetBuf, sizeof(targetBuf), targetTemp);
        formatTemp(currentBuf, sizeof(currentBuf), boilerOutputTemp);
        formatTemp(adjustBuf, sizeof(adjustBuf), adjustmentFixed);
        LOG_DEBUG(self->TAG, "Water PID: target=%s, current=%s, adjustment=%s",
                  targetBuf, currentBuf, adjustBuf);

        // Check if change is significant enough to update
        if (!BurnerAntiFlapping::isSignificantPIDChange(lastAdjustment, adjustment)) {
            vTaskDelay(pidInterval);
            continue;
        }
        lastAdjustment = adjustment;

        // Convert adjustment to power level
        uint32_t adjustmentLevel = PIDControlModule::determineAdjustmentLevel(adjustment);

        // Update power bits - determine which bits to set
        EventBits_t powerBitsToSet = 0;
        if (adjustmentLevel >= SystemConstants::PID::MIN_ADJUSTMENT_LEVEL_FOR_ON) {
            if (adjustmentLevel >= SystemConstants::PID::MIN_ADJUSTMENT_LEVEL_FOR_HIGH) {
                powerBitsToSet = SystemEvents::BurnerRequest::POWER_HIGH;
                LOG_DEBUG(self->TAG, "Water PID: adjustment %lu -> FULL power", adjustmentLevel);
            } else {
                powerBitsToSet = SystemEvents::BurnerRequest::POWER_LOW;
                LOG_DEBUG(self->TAG, "Water PID: adjustment %lu -> HALF power", adjustmentLevel);
            }

            // Round 12 Issue #3: Use atomic update to prevent race condition
            // BurnerRequestManager provides mutex-protected atomic bit updates
            BurnerRequestManager::atomicUpdateBits(powerBitsToSet, SystemEvents::BurnerRequest::POWER_BITS);
        } else {
            // Adjustment too low - burner should be off (handled by WheaterControlTask)
            LOG_DEBUG(self->TAG, "Water PID: adjustment %lu -> below threshold", adjustmentLevel);
        }

        // Feed watchdog
        (void)SRP::getTaskManager().feedWatchdog();

        vTaskDelay(pidInterval);
    }

    // Task cleanup with mutex protection (Round 12 Issue #1)
    LOG_INFO(self->TAG, "Water heating PID task stopping");
    {
        MutexGuard guard(self->taskStateMutex_, pdMS_TO_TICKS(100));
        if (guard.hasLock()) {
            self->pidTaskRunning = false;
            self->pidTaskHandle = nullptr;
        }
    }
    vTaskDelete(nullptr);
}
