// src/modules/control/PumpControlModule.h
// Unified pump control module for heating and water pumps
#ifndef PUMP_CONTROL_MODULE_H
#define PUMP_CONTROL_MODULE_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "events/SystemEventsGenerated.h"
#include <RuntimeStorage.h>

// Generic pump state
enum class PumpState { Off, On, Error };

/**
 * @brief Configuration for a pump control instance
 *
 * Allows the same control logic to be used for both heating and water pumps
 * by parameterizing the event bits and counters.
 */
struct PumpConfig {
    // Event bit to check if this pump's mode is active
    EventBits_t modeActiveBit;        // e.g., HEATING_ON or WATER_ON

    // Event bit to set when pump is on (state indicator)
    EventBits_t pumpOnStateBit;       // e.g., HEATING_PUMP_ON or WATER_PUMP_ON

    // Relay request bits
    EventBits_t relayOnRequestBit;    // e.g., RelayRequest::HEATING_PUMP_ON
    EventBits_t relayOffRequestBit;   // e.g., RelayRequest::HEATING_PUMP_OFF

    // FRAM counter for pump starts
    rtstorage::CounterType startCounterId;

    // Task identification
    const char* taskName;             // e.g., "HeatingPump" or "WaterPump"
    const char* logTag;               // e.g., "HeatingPumpCtrl" or "WaterPumpCtrl"
};

/**
 * @brief Unified pump control module
 *
 * Manages pump state based on system mode and boiler enable state.
 * Motor protection (30s minimum interval) is enforced BOTH here AND at relay layer
 * for defense-in-depth.
 */
class PumpControlModule {
public:
    // Motor protection constant (30s minimum between state changes)
    static constexpr uint32_t MOTOR_PROTECTION_INTERVAL_MS = 30000;

    /**
     * @brief Generic pump control task
     * @param pvParameters Pointer to PumpConfig struct (must remain valid)
     */
    static void PumpControlTask(void* pvParameters);

    // Pre-configured task entry points for FreeRTOS
    static void HeatingPumpTask(void* pvParameters);
    static void WaterPumpTask(void* pvParameters);

    // State accessors
    static PumpState getHeatingPumpState();
    static PumpState getWaterPumpState();

    /**
     * @brief Check if pump state change is allowed (motor protection)
     * @param isHeatingPump true for heating pump, false for water pump
     * @return true if 30s has elapsed since last state change
     */
    static bool canChangeState(bool isHeatingPump);

private:
    static PumpState heatingPumpState_;
    static PumpState waterPumpState_;

    // Timestamps for motor protection (defense-in-depth with relay layer)
    static uint32_t heatingPumpLastChangeTime_;
    static uint32_t waterPumpLastChangeTime_;

    // Mutex for thread-safe state access (prevents torn reads/writes)
    static SemaphoreHandle_t stateMutex_;
    static bool mutexInitialized_;

    // Static configurations
    static const PumpConfig heatingPumpConfig_;
    static const PumpConfig waterPumpConfig_;

    // Internal helper to ensure mutex is initialized
    static void initMutexIfNeeded();
};

#endif // PUMP_CONTROL_MODULE_H
