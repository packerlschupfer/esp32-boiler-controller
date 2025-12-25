// include/core/StateManager.h
#ifndef STATE_MANAGER_H
#define STATE_MANAGER_H

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "shared/Temperature.h"
#include "shared/Pressure.h"
#include "shared/SharedSensorReadings.h"  // Required for SensorReadingsWithAge struct
#include "config/SystemConstants.h"

// Forward declarations
struct SharedRelayReadings;
struct SystemSettings;

/**
 * @brief Atomic sensor readings with staleness metadata
 * Used to prevent TOCTOU race between checking staleness and reading data
 */
struct SensorReadingsWithAge {
    SharedSensorReadings readings;
    uint32_t ageMs;       // Age of sensor data in milliseconds
    bool isStale;         // True if age > SENSOR_STALE_THRESHOLD_MS
    bool mutexAcquired;   // True if mutex was successfully acquired
};

/**
 * @file StateManager.h
 * @brief Unified state management for the boiler controller
 *
 * StateManager provides a single source of truth for system state,
 * eliminating the dual-truth problem between event bits and settings.
 *
 * Key features:
 * - Atomic enable state updates (event bits + settings synced)
 * - Centralized staleness detection for sensors
 * - Safety precondition validation
 * - Thread-safe access to shared state
 *
 * Usage:
 *   StateManager::setBoilerEnabled(true);  // Updates both event bits AND settings
 *   if (StateManager::canStartBurner()) { ... }
 */
class StateManager {
public:
    // Sensor channels for staleness detection
    enum class SensorChannel {
        BOILER_OUTPUT,
        BOILER_RETURN,
        WATER_TANK,
        WATER_TANK_TOP,   // Optional - enable via ENABLE_SENSOR_WATER_TANK_TOP
        WATER_RETURN,     // Optional - enable via ENABLE_SENSOR_WATER_RETURN
        HEATING_RETURN,   // Optional - enable via ENABLE_SENSOR_HEATING_RETURN
        OUTSIDE_TEMP,
        INSIDE_TEMP,
        PRESSURE
    };

    /**
     * @brief Initialize the StateManager
     * Must be called after SharedResourceManager is initialized
     */
    static void initialize();

    /**
     * @brief Cleanup StateManager resources
     * Round 20 Issue #11: Deletes mutex to prevent resource leak
     */
    static void cleanup();

    // ========== Enable State Management ==========
    // These methods atomically update BOTH event bits AND SystemSettings

    /**
     * @brief Set boiler master enable state
     * @param enabled True to enable, false to disable
     * @param persist If true, mark settings as dirty for NVS save
     */
    static void setBoilerEnabled(bool enabled, bool persist = true);

    /**
     * @brief Set space heating enable state
     */
    static void setHeatingEnabled(bool enabled, bool persist = true);

    /**
     * @brief Set water heating enable state
     */
    static void setWaterEnabled(bool enabled, bool persist = true);

    /**
     * @brief Set water priority enable state
     */
    static void setWaterPriorityEnabled(bool enabled, bool persist = true);

    /**
     * @brief Set heating override off (summer mode)
     */
    static void setHeatingOverrideOff(bool blocked, bool persist = true);

    /**
     * @brief Set water override off (summer mode)
     */
    static void setWaterOverrideOff(bool blocked, bool persist = true);

    // Enable state getters (read from event bits for speed)
    static bool isBoilerEnabled();
    static bool isHeatingEnabled();
    static bool isWaterEnabled();
    static bool isWaterPriorityEnabled();
    static bool isHeatingOverrideOff();
    static bool isWaterOverrideOff();

    // ========== Sensor Staleness Detection ==========

    /**
     * @brief Check if a sensor reading is stale
     * @param channel The sensor channel to check
     * @param maxAgeMs Maximum age in milliseconds (default 15s)
     * @return true if data is older than maxAgeMs
     */
    static bool isSensorStale(SensorChannel channel,
                              uint32_t maxAgeMs = SystemConstants::Safety::SENSOR_STALE_THRESHOLD_MS);

    /**
     * @brief Check if all critical sensors are valid (not stale, valid readings)
     * Critical sensors: boiler output, boiler return, pressure
     */
    static bool areAllCriticalSensorsValid();

    /**
     * @brief Check if a specific sensor has a valid reading
     */
    static bool isSensorValid(SensorChannel channel);

    /**
     * @brief Get age of sensor data in milliseconds
     */
    static uint32_t getSensorAge(SensorChannel channel);

    // ========== Safety Preconditions ==========

    /**
     * @brief Check if burner can be started safely
     * Validates: sensors valid, not stale, pressure OK, temps OK, enabled
     */
    static bool canStartBurner();

    /**
     * @brief Check if heating can be enabled
     * Validates: boiler enabled, heating not blocked, sensors valid
     */
    static bool canEnableHeating();

    /**
     * @brief Check if water heating can be enabled
     */
    static bool canEnableWaterHeating();

    /**
     * @brief Check if system is in emergency stop
     */
    static bool isEmergencyStop();

    /**
     * @brief Check if system is in degraded mode
     */
    static bool isDegradedMode();

    // ========== Convenience Accessors ==========
    // These provide safe access with mutex protection

    /**
     * @brief Get a copy of current sensor readings (thread-safe)
     */
    static SharedSensorReadings getSensorReadingsCopy();

    /**
     * @brief Get sensor readings with staleness info atomically
     * Prevents TOCTOU race between checking staleness and reading data.
     * Use this instead of separate isSensorStale() + getSensorReadingsCopy() calls.
     *
     * @param maxAgeMs Maximum acceptable age before considered stale
     * @return SensorReadingsWithAge containing readings, age, and staleness flag
     */
    static SensorReadingsWithAge getSensorReadingsAtomic(
        uint32_t maxAgeMs = SystemConstants::Safety::SENSOR_STALE_THRESHOLD_MS);

    /**
     * @brief Get a copy of current relay readings (thread-safe)
     */
    static SharedRelayReadings getRelayReadingsCopy();

    // ========== Settings Dirty Flag ==========
    // For NVS persistence coordination

    /**
     * @brief Check if settings have been modified since last save
     */
    static bool areSettingsDirty();

    /**
     * @brief Clear the dirty flag (called after NVS save)
     */
    static void clearSettingsDirty();

    /**
     * @brief Mark settings as dirty (triggers save on next check)
     */
    static void markSettingsDirty();

    /**
     * @brief Sync enable states from SystemSettings to event bits
     * Call after loading settings from NVS
     */
    static void syncEnableStatesToEventBits();

private:
    static bool initialized_;
    static bool settingsDirty_;
    static SemaphoreHandle_t dirtyFlagMutex_;  // Protects settingsDirty_ access

    // Internal helpers
    static void syncEventBitsToSettings();
};

// Convenience macro
#define SM StateManager

#endif // STATE_MANAGER_H
