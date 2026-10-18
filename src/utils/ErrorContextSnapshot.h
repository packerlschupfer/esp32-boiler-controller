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
 * @brief Captures error context snapshots on critical errors and publishes them
 *
 * Wired in 2026-09-15 (was never called): ErrorHandler::logError() calls
 * recordCriticalError(), which fills a static slot in the calling task (no stack copy, no
 * heap, rate-limited); MQTTTask calls publishPending(), which formats compact JSON
 * (ErrorContextFormat.h, fits the 320-byte payload) and publishes boiler/error/context.
 */
class ErrorContextCapture {
public:
    /**
     * @brief Record a snapshot if the error is critical (any task context)
     *
     * At most one snapshot per MIN_INTERVAL_MS; a snapshot not yet published is kept.
     */
    static void recordCriticalError(SystemError errorCode, const char* component, const char* description);

    /**
     * @brief Publish a recorded snapshot (MQTT task only)
     */
    static void publishPending();

    /**
     * @brief Check if this error type is critical (requires snapshot)
     */
    static bool isCriticalError(SystemError error);

    static constexpr uint32_t MIN_INTERVAL_MS = 30000;

private:
    // Fill the snapshot in place (no by-value copy on the calling task's stack)
    static void captureInto(ErrorContextSnapshot& snapshot, SystemError errorCode,
                            const char* component, const char* description);
};

#endif // ERROR_CONTEXT_SNAPSHOT_H
