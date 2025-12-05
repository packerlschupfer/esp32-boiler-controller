// src/modules/control/WheaterControlModule.h
#ifndef WHEATER_CONTROL_MODULE_H
#define WHEATER_CONTROL_MODULE_H

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include "shared/SharedSensorReadings.h"
#include "shared/Temperature.h"
#include "config/SystemSettings.h"
#include "modules/control/PIDControlModuleFixedPoint.h"
#include "core/SystemResourceProvider.h"

/**
 * @brief Water heating control module with PID-based power modulation
 *
 * Similar to HeatingControlModule but for water heating:
 * - Target: boiler output = water return + charge delta
 * - PID modulates between HALF/FULL power to maintain target
 * - Goal: minimize burner on/off cycles by continuous operation at setpoint
 */
class WheaterControlModule {
public:
    // Constructor
    WheaterControlModule();

    // Destructor
    ~WheaterControlModule();

    // Initialize the water heating control module
    void initialize();

    /**
     * @brief Calculate boiler target for water heating
     * @param readings Current sensor readings
     * @param settings System settings (contains charge delta)
     * @return Target boiler output temperature (return + delta)
     */
    Temperature_t calculateWaterHeatingTargetTemp(const SharedSensorReadings& readings,
                                                   const SystemSettings& settings) const;

    /**
     * @brief Start the Water Heating PID Task
     * Call when water heating becomes active
     */
    void startWaterHeatingPIDTask();

    /**
     * @brief Stop the Water Heating PID Task
     * Call when water heating is deactivated
     */
    void stopWaterHeatingPIDTask();

    /**
     * @brief Check if PID task is running (thread-safe)
     */
    bool isPIDTaskRunning() const;

    /**
     * @brief Reset PID controller state (clears integral accumulator)
     */
    void resetPID();

    /**
     * @brief Set PID gains for water heating
     * @param kp Proportional gain
     * @param ki Integral gain
     * @param kd Derivative gain
     */
    void setPIDGains(float kp, float ki, float kd);

    /**
     * @brief Get current PID gains
     */
    void getPIDGains(float& kp, float& ki, float& kd) const;

private:
    // Internal Water Heating PID Task logic
    static void WaterHeatingPIDTask(void* parameter);

    // Thread safety mutexes (Round 12 Issue #1, #2, #7)
    SemaphoreHandle_t taskStateMutex_ = nullptr;   // Protects pidTaskRunning, pidTaskStopRequested
    SemaphoreHandle_t pidGainsMutex_ = nullptr;    // Protects pidKp, pidKi, pidKd
    SemaphoreHandle_t pidStateMutex_ = nullptr;    // Protects fixedPointPID access

    // Fixed-point PID controller
    PIDControlModuleFixedPoint* fixedPointPID = nullptr;

    // PID gains for water heating (different tuning than space heating)
    // Protected by pidGainsMutex_
    float pidKp = 2.0f;   // Proportional gain
    float pidKi = 0.1f;   // Integral gain
    float pidKd = 0.5f;   // Derivative gain

    // PID interval adjustment factor
    float pidFactorWaterHeating = 1.0f;

    // Task management - protected by taskStateMutex_
    TaskHandle_t pidTaskHandle = nullptr;
    bool pidTaskRunning = false;      // Removed volatile - use mutex instead
    bool pidTaskStopRequested = false; // Removed volatile - use mutex instead

    // Logging tag
    static constexpr const char* TAG = "WheaterControl";
};

#endif // WHEATER_CONTROL_MODULE_H
