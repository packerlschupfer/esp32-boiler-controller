// src/utils/ErrorContextSnapshot.h
#ifndef ERROR_CONTEXT_SNAPSHOT_H
#define ERROR_CONTEXT_SNAPSHOT_H

#include "shared/Temperature.h"
#include "shared/Pressure.h"
#include "utils/ErrorHandler.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <cstdint>

/**
 * @brief Complete system state snapshot captured on critical errors
 *
 * IMPROVEMENT 4 (simplified): Enhanced Error Context
 * Captures full system state when critical errors occur for better diagnostics.
 *
 * Context includes:
 * - Error identification (code, component, description)
 * - Task context (task name, priority)
 * - Memory state (free heap, minimum heap, largest block)
 * - Event group snapshots (system state, burner, sensors, relays)
 * - Sensor readings (temperatures, pressure)
 * - Relay states (desired vs actual)
 * - Burner state
 */
struct ErrorContextSnapshot {
    // Error identification
    SystemError errorCode;
    char component[16];
    char description[64];
    uint32_t timestamp;

    // Task context
    char taskName[16];
    uint8_t taskPriority;

    // Memory context
    uint32_t freeHeap;
    uint32_t minFreeHeap;
    uint32_t largestFreeBlock;

    // Event group snapshot (system state)
    uint32_t systemStateBits;
    uint32_t burnerRequestBits;
    uint32_t sensorEventBits;
    uint32_t relayEventBits;

    // Sensor snapshot (critical values only)
    Temperature_t boilerTempOutput;
    Temperature_t boilerTempReturn;
    Temperature_t waterHeaterTempTank;
    Pressure_t systemPressure;
    bool sensorsValid;

    // Relay snapshot
    uint8_t relayDesiredState;
    uint8_t relayActualState;
    uint8_t relayMismatchMask;

    // Burner state
    uint8_t burnerState;  // BurnerSMState cast to uint8_t
    bool burnerActive;
    bool heatingActive;
    bool waterActive;
};

/**
 * @brief Captures and publishes error context snapshots
 */
class ErrorContextCapture {
public:
    /**
     * @brief Capture complete system snapshot at error time
     * @param errorCode Error code that occurred
     * @param component Component tag where error occurred
     * @param description Human-readable error description
     * @return Complete snapshot of system state
     */
    static ErrorContextSnapshot captureSnapshot(
        SystemError errorCode,
        const char* component,
        const char* description
    );

    /**
     * @brief Publish error context to MQTT for remote monitoring
     * @param snapshot System snapshot to publish
     */
    static void publishErrorContext(const ErrorContextSnapshot& snapshot);

private:
    /**
     * @brief Check if this error type is critical (requires snapshot)
     * @param error Error code to check
     * @return True if critical error requiring snapshot
     */
    static bool isCriticalError(SystemError error);
};

#endif // ERROR_CONTEXT_SNAPSHOT_H
