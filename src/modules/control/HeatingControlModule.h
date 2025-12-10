// src/modules/control/HeatingControlModule.h
#ifndef HEATING_CONTROL_MODULE_H
#define HEATING_CONTROL_MODULE_H

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include "shared/SharedSensorReadings.h"
#include "shared/Temperature.h"
#include "config/SystemSettings.h"
#include "utils/Utils.h"
#include "PIDControlModule.h"
#include "modules/control/PIDControlModuleFixedPoint.h"
#include "PIDAutoTuner.h"
#include "core/SystemResourceProvider.h"

// Note: Access these through SystemResourceProvider (SRP):
// - SRP::getSystemSettings() for SystemSettings
// - SRP::getPIDControl() for PIDControlModule

class HeatingControlModule {
public:
    // Constructor
    HeatingControlModule(EventGroupHandle_t systemEventGroup, SemaphoreHandle_t sensorMutex);

    // Destructor
    ~HeatingControlModule();

    // Initialize the heating control module
    void initialize();

    // Calculate the target temperature for space heating based on shared sensor readings and system settings
    Temperature_t calculateSpaceHeatingTargetTemp(const SharedSensorReadings& readings, const SystemSettings& settings) const;

    // Check if heating conditions are met based on temperature and hysteresis
    bool checkHeatingConditions(const SharedSensorReadings& readings, Temperature_t targetTemperature, Temperature_t hysteresis) const;

    // Start and stop heating
    void startHeating();
    void stopHeating();

    // Reset PID controller state (clears integral accumulator)
    void resetPID();

    // Start the Space Heating PID Task
    void startSpaceHeatingPIDTask();

    /**
     * @brief Stop the Space Heating PID Task (C1: destructor safety)
     * Must be called before destruction to safely stop the task
     */
    void stopSpaceHeatingPIDTask();

    /**
     * @brief Check if PID task is running (thread-safe)
     */
    bool isPIDTaskRunning() const;

    /**
     * @brief Set PID gains for space heating (thread-safe)
     * @param kp Proportional gain
     * @param ki Integral gain
     * @param kd Derivative gain
     * Round 13 Issue #2: Added for consistency with WheaterControlModule
     */
    void setPIDGains(float kp, float ki, float kd);

    /**
     * @brief Get current PID gains (thread-safe)
     * Round 13 Issue #2: Added for consistency with WheaterControlModule
     */
    void getPIDGains(float& kp, float& ki, float& kd) const;

private:
    // Internal Space Heating PID Task logic
    static void SpaceHeatingPIDTask(void* parameter);

    // Helper function to calculate the effective PID interval
    TickType_t calculateEffectivePIDInterval() const;

    // Thread safety mutexes (Round 13 Issue #2, #3)
    SemaphoreHandle_t taskStateMutex_ = nullptr;   // Protects isAutoTuning, pidTaskRunning, pidTaskStopRequested
    SemaphoreHandle_t pidGainsMutex_ = nullptr;    // Protects pidKp, pidKi, pidKd

    // Task management - protected by taskStateMutex_ (C1: destructor safety)
    TaskHandle_t pidTaskHandle = nullptr;
    bool pidTaskRunning = false;
    bool pidTaskStopRequested = false;

    // PID gains for space heating - protected by pidGainsMutex_
    // Round 13 Issue #2: Cache gains locally with mutex protection
    float pidKp = 1.0f;   // Proportional gain
    float pidKi = 0.05f;  // Integral gain
    float pidKd = 0.2f;   // Derivative gain

    // PID interval adjustment factor for space heating
    float pidFactorSpaceHeating = 1.0f;

    // Auto-tuning support - isAutoTuning protected by taskStateMutex_
    bool isAutoTuning = false;
    Temperature_t autoTuneSetpoint = 0;
    PIDAutoTuner* autoTuner = nullptr;

    // Fixed-point PID controller
    class PIDControlModuleFixedPoint* fixedPointPID = nullptr;

    // Auto-tuning methods
    bool startAutoTuning(Temperature_t setpoint);
    void stopAutoTuning();
    bool isAutoTuningActive() const;  // Removed inline impl - needs mutex
    PIDAutoTuner::TuningState getAutoTuningState() const;
    uint8_t getAutoTuningProgress() const;
    PIDAutoTuner::TuningResult getAutoTuningResults() const;

    // Note: Event groups and mutexes are now accessed via SystemResourceProvider
    // The constructor parameters are kept for backward compatibility

    // Logging tag
    static constexpr const char* TAG = "HeatingControl";
};

#endif // HEATING_CONTROL_MODULE_H
