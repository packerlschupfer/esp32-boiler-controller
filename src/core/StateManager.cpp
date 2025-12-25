// src/core/StateManager.cpp
#include "core/StateManager.h"
#include "core/SystemResourceProvider.h"
#include "events/SystemEventsGenerated.h"
#include "shared/SharedSensorReadings.h"
#include "shared/SharedRelayReadings.h"
#include "config/SystemSettings.h"
#include "config/ProjectConfig.h"
#include "utils/Utils.h"  // Round 16 Issue #1: Safe elapsed time calculation
#include "LoggingMacros.h"
#include <MutexGuard.h>

static constexpr const char* TAG = "StateManager";

// Static member definitions
bool StateManager::initialized_ = false;
bool StateManager::settingsDirty_ = false;
SemaphoreHandle_t StateManager::dirtyFlagMutex_ = nullptr;

void StateManager::initialize() {
    if (initialized_) {
        LOG_WARN(TAG, "Already initialized");
        return;
    }

    LOG_INFO(TAG, "Initializing StateManager...");

    // Create mutex for dirty flag protection
    dirtyFlagMutex_ = xSemaphoreCreateMutex();
    if (dirtyFlagMutex_ == nullptr) {
        LOG_ERROR(TAG, "Failed to create dirtyFlagMutex");
    }

    // Sync settings to event bits on startup
    syncEnableStatesToEventBits();

    initialized_ = true;
    LOG_INFO(TAG, "StateManager initialized");
}

// Round 20 Issue #11: Cleanup function to prevent resource leaks
void StateManager::cleanup() {
    if (!initialized_) {
        return;
    }

    LOG_INFO(TAG, "Cleaning up StateManager...");

    if (dirtyFlagMutex_ != nullptr) {
        vSemaphoreDelete(dirtyFlagMutex_);
        dirtyFlagMutex_ = nullptr;
    }

    initialized_ = false;
    settingsDirty_ = false;
    LOG_INFO(TAG, "StateManager cleaned up");
}

// ========== Enable State Management ==========

void StateManager::setBoilerEnabled(bool enabled, bool persist) {
    // Update settings struct
    {
        MutexGuard guard(SRP::getSystemSettingsMutex(), pdMS_TO_TICKS(100));
        if (guard.hasLock()) {
            SRP::getSystemSettings().boilerEnabled = enabled;
        }
    }

    // Update event bits (atomic, no mutex needed)
    if (enabled) {
        SRP::setSystemStateEventBits(SystemEvents::SystemState::BOILER_ENABLED);
    } else {
        SRP::clearSystemStateEventBits(SystemEvents::SystemState::BOILER_ENABLED);
    }

    if (persist) {
        markSettingsDirty();
    }

    LOG_INFO(TAG, "Boiler %s", enabled ? "ENABLED" : "DISABLED");
}

void StateManager::setHeatingEnabled(bool enabled, bool persist) {
    {
        MutexGuard guard(SRP::getSystemSettingsMutex(), pdMS_TO_TICKS(100));
        if (guard.hasLock()) {
            SRP::getSystemSettings().heatingEnabled = enabled;
        }
    }

    if (enabled) {
        SRP::setSystemStateEventBits(SystemEvents::SystemState::HEATING_ENABLED);
    } else {
        SRP::clearSystemStateEventBits(SystemEvents::SystemState::HEATING_ENABLED);
    }

    if (persist) {
        markSettingsDirty();
    }

    LOG_INFO(TAG, "Heating %s", enabled ? "ENABLED" : "DISABLED");
}

void StateManager::setWaterEnabled(bool enabled, bool persist) {
    {
        MutexGuard guard(SRP::getSystemSettingsMutex(), pdMS_TO_TICKS(100));
        if (guard.hasLock()) {
            SRP::getSystemSettings().waterEnabled = enabled;
        }
    }

    if (enabled) {
        SRP::setSystemStateEventBits(SystemEvents::SystemState::WATER_ENABLED);
    } else {
        SRP::clearSystemStateEventBits(SystemEvents::SystemState::WATER_ENABLED);

        // Round 17 Issue X: Notify HeatingControlTask immediately so it can claim burner
        // BEFORE WheaterControlTask clears its request. This ensures seamless handoff
        // from water→heating without burner shutdown.
        // Note: We do NOT clear water request here - WheaterControlTask will do that.
        // This allows BurnerControlTask to see continuous demand during transition.
        xEventGroupSetBits(SRP::getControlRequestsEventGroup(),
                          SystemEvents::ControlRequest::WATER_PRIORITY_RELEASED);
        LOG_DEBUG(TAG, "Water priority released - heating notified for potential handoff");
    }

    if (persist) {
        markSettingsDirty();
    }

    LOG_INFO(TAG, "Water heating %s", enabled ? "ENABLED" : "DISABLED");
}

void StateManager::setWaterPriorityEnabled(bool enabled, bool persist) {
    {
        MutexGuard guard(SRP::getSystemSettingsMutex(), pdMS_TO_TICKS(100));
        if (guard.hasLock()) {
            SRP::getSystemSettings().wheaterPriorityEnabled = enabled;
        }
    }

    if (enabled) {
        SRP::setSystemStateEventBits(SystemEvents::SystemState::WATER_PRIORITY);
    } else {
        SRP::clearSystemStateEventBits(SystemEvents::SystemState::WATER_PRIORITY);

        // Notify heating task that water priority has been released
        // This triggers immediate re-evaluation for seamless transition
        xEventGroupSetBits(SRP::getControlRequestsEventGroup(),
                          SystemEvents::ControlRequest::WATER_PRIORITY_RELEASED);
        LOG_DEBUG(TAG, "Water priority released - heating notified for potential takeover");
    }

    if (persist) {
        markSettingsDirty();
    }

    LOG_INFO(TAG, "Water priority %s", enabled ? "ENABLED" : "DISABLED");
}

void StateManager::setHeatingOverrideOff(bool blocked, bool persist) {
    {
        MutexGuard guard(SRP::getSystemSettingsMutex(), pdMS_TO_TICKS(100));
        if (guard.hasLock()) {
            SRP::getSystemSettings().heatingOverrideOff = blocked;
        }
    }

    // No corresponding event bit for override - it's settings-only
    if (persist) {
        markSettingsDirty();
    }

    LOG_INFO(TAG, "Heating override %s", blocked ? "BLOCKED" : "CLEARED");
}

void StateManager::setWaterOverrideOff(bool blocked, bool persist) {
    {
        MutexGuard guard(SRP::getSystemSettingsMutex(), pdMS_TO_TICKS(100));
        if (guard.hasLock()) {
            SRP::getSystemSettings().waterOverrideOff = blocked;
        }
    }

    if (persist) {
        markSettingsDirty();
    }

    LOG_INFO(TAG, "Water override %s", blocked ? "BLOCKED" : "CLEARED");
}

// Enable state getters - read from event bits (atomic, fast)
bool StateManager::isBoilerEnabled() {
    return (SRP::getSystemStateEventBits() & SystemEvents::SystemState::BOILER_ENABLED) != 0;
}

bool StateManager::isHeatingEnabled() {
    return (SRP::getSystemStateEventBits() & SystemEvents::SystemState::HEATING_ENABLED) != 0;
}

bool StateManager::isWaterEnabled() {
    return (SRP::getSystemStateEventBits() & SystemEvents::SystemState::WATER_ENABLED) != 0;
}

bool StateManager::isWaterPriorityEnabled() {
    return (SRP::getSystemStateEventBits() & SystemEvents::SystemState::WATER_PRIORITY) != 0;
}

bool StateManager::isHeatingOverrideOff() {
    MutexGuard guard(SRP::getSystemSettingsMutex(), pdMS_TO_TICKS(50));
    if (guard.hasLock()) {
        return SRP::getSystemSettings().heatingOverrideOff;
    }
    return false;  // Safe default
}

bool StateManager::isWaterOverrideOff() {
    MutexGuard guard(SRP::getSystemSettingsMutex(), pdMS_TO_TICKS(50));
    if (guard.hasLock()) {
        return SRP::getSystemSettings().waterOverrideOff;
    }
    return false;
}

// ========== Sensor Staleness Detection ==========

uint32_t StateManager::getSensorAge(SensorChannel channel) {
    uint32_t timestamp = 0;

    MutexGuard guard(SRP::getSensorReadingsMutex(), pdMS_TO_TICKS(50));
    if (guard.hasLock()) {
        const SharedSensorReadings& readings = SRP::getSensorReadings();

        if (channel == SensorChannel::PRESSURE) {
            timestamp = readings.lastPressureUpdateTimestamp;
        } else {
            timestamp = readings.lastUpdateTimestamp;
        }
    }

    if (timestamp == 0) {
        // M21: Return large value for never-updated sensors
        // Using 0x7FFFFFFF instead of UINT32_MAX to prevent overflow in arithmetic
        // This still compares as "stale" for any reasonable maxAgeMs threshold
        return 0x7FFFFFFF;
    }

    // Round 16 Issue #1: Use Utils::elapsedMs() for safe wraparound handling
    // This is safety-critical - sensor staleness detection affects burner operation
    return Utils::elapsedMs(timestamp);
}

bool StateManager::isSensorStale(SensorChannel channel, uint32_t maxAgeMs) {
    return getSensorAge(channel) > maxAgeMs;
}

bool StateManager::isSensorValid(SensorChannel channel) {
    MutexGuard guard(SRP::getSensorReadingsMutex(), pdMS_TO_TICKS(50));
    if (!guard.hasLock()) {
        return false;
    }

    const SharedSensorReadings& readings = SRP::getSensorReadings();

    switch (channel) {
        case SensorChannel::BOILER_OUTPUT:   return readings.isBoilerTempOutputValid;
        case SensorChannel::BOILER_RETURN:   return readings.isBoilerTempReturnValid;
        case SensorChannel::WATER_TANK:      return readings.isWaterHeaterTempTankValid;
        case SensorChannel::OUTSIDE_TEMP:    return readings.isOutsideTempValid;
        case SensorChannel::INSIDE_TEMP:     return readings.isInsideTempValid;
        case SensorChannel::PRESSURE:        return readings.isSystemPressureValid;

        // Optional sensors (enable via ENABLE_SENSOR_* flags)
#ifdef ENABLE_SENSOR_WATER_TANK_TOP
        case SensorChannel::WATER_TANK_TOP:  return readings.isWaterTankTopTempValid;
#endif
#ifdef ENABLE_SENSOR_WATER_RETURN
        case SensorChannel::WATER_RETURN:    return readings.isWaterHeaterTempReturnValid;
#endif
#ifdef ENABLE_SENSOR_HEATING_RETURN
        case SensorChannel::HEATING_RETURN:  return readings.isHeatingTempReturnValid;
#endif
        default:                             return false;
    }
}

bool StateManager::areAllCriticalSensorsValid() {
    // Critical sensors for burner operation
    if (!isSensorValid(SensorChannel::BOILER_OUTPUT)) return false;
    if (!isSensorValid(SensorChannel::BOILER_RETURN)) return false;
    if (!isSensorValid(SensorChannel::PRESSURE)) return false;

    // Check staleness
    uint32_t maxAge = SystemConstants::Safety::SENSOR_STALE_THRESHOLD_MS;
    if (isSensorStale(SensorChannel::BOILER_OUTPUT, maxAge)) return false;
    if (isSensorStale(SensorChannel::BOILER_RETURN, maxAge)) return false;
    if (isSensorStale(SensorChannel::PRESSURE, maxAge)) return false;

    return true;
}

// ========== Safety Preconditions ==========

bool StateManager::canStartBurner() {
    // 1. System must be enabled
    if (!isBoilerEnabled()) {
        return false;
    }

    // 2. Not in emergency stop or degraded mode
    if (isEmergencyStop() || isDegradedMode()) {
        return false;
    }

    // 3. Critical sensors must be valid and not stale
    if (!areAllCriticalSensorsValid()) {
        return false;
    }

    // 4. Pressure must be within safe range
    MutexGuard guard(SRP::getSensorReadingsMutex(), pdMS_TO_TICKS(50));
    if (guard.hasLock()) {
        const SharedSensorReadings& readings = SRP::getSensorReadings();
        if (readings.isSystemPressureValid) {
            if (readings.systemPressure < SystemConstants::Safety::Pressure::ALARM_MIN ||
                readings.systemPressure > SystemConstants::Safety::Pressure::ALARM_MAX) {
                return false;
            }
        }
    }

    return true;
}

bool StateManager::canEnableHeating() {
    if (!isBoilerEnabled()) return false;
    if (!isHeatingEnabled()) return false;
    if (isHeatingOverrideOff()) return false;
    if (isEmergencyStop()) return false;
    return true;
}

bool StateManager::canEnableWaterHeating() {
    if (!isBoilerEnabled()) return false;
    if (!isWaterEnabled()) return false;
    if (isWaterOverrideOff()) return false;
    if (isEmergencyStop()) return false;
    return true;
}

bool StateManager::isEmergencyStop() {
    return (SRP::getSystemStateEventBits() & SystemEvents::SystemState::EMERGENCY_STOP) != 0;
}

bool StateManager::isDegradedMode() {
    return (SRP::getSystemStateEventBits() & SystemEvents::SystemState::DEGRADED_MODE) != 0;
}

// ========== Convenience Accessors ==========

SharedSensorReadings StateManager::getSensorReadingsCopy() {
    MutexGuard guard(SRP::getSensorReadingsMutex(), pdMS_TO_TICKS(100));
    if (guard.hasLock()) {
        return SRP::getSensorReadings();  // Returns copy
    }
    return SharedSensorReadings{};  // Return default/empty if mutex failed
}

SensorReadingsWithAge StateManager::getSensorReadingsAtomic(uint32_t maxAgeMs) {
    SensorReadingsWithAge result{};
    result.mutexAcquired = false;
    result.ageMs = UINT32_MAX;
    result.isStale = true;

    MutexGuard guard(SRP::getSensorReadingsMutex(), pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        return result;  // Return with mutexAcquired = false
    }

    result.mutexAcquired = true;
    result.readings = SRP::getSensorReadings();

    // Calculate age from the same mutex acquisition
    // Round 16 Issue #1: Use Utils::elapsedMs() for safe wraparound handling
    uint32_t timestamp = result.readings.lastUpdateTimestamp;
    if (timestamp == 0) {
        result.ageMs = UINT32_MAX;
    } else {
        result.ageMs = Utils::elapsedMs(timestamp);
    }

    result.isStale = (result.ageMs > maxAgeMs);
    return result;
}

SharedRelayReadings StateManager::getRelayReadingsCopy() {
    MutexGuard guard(SRP::getRelayReadingsMutex(), pdMS_TO_TICKS(100));
    if (guard.hasLock()) {
        return SRP::getRelayReadings();
    }
    return SharedRelayReadings{};
}

// ========== Settings Dirty Flag ==========

bool StateManager::areSettingsDirty() {
    if (dirtyFlagMutex_ == nullptr) {
        return settingsDirty_;  // Fallback during early init
    }
    MutexGuard guard(dirtyFlagMutex_, pdMS_TO_TICKS(10));
    if (guard.hasLock()) {
        return settingsDirty_;
    }
    return false;  // Safe default if mutex unavailable
}

void StateManager::clearSettingsDirty() {
    if (dirtyFlagMutex_ == nullptr) {
        settingsDirty_ = false;
        return;
    }
    MutexGuard guard(dirtyFlagMutex_, pdMS_TO_TICKS(10));
    if (guard.hasLock()) {
        settingsDirty_ = false;
    }
}

void StateManager::markSettingsDirty() {
    if (dirtyFlagMutex_ == nullptr) {
        settingsDirty_ = true;
        return;
    }
    MutexGuard guard(dirtyFlagMutex_, pdMS_TO_TICKS(10));
    if (guard.hasLock()) {
        settingsDirty_ = true;
    }
}

// ========== Internal Helpers ==========

void StateManager::syncEnableStatesToEventBits() {
    LOG_INFO(TAG, "Syncing enable states from settings to event bits...");

    // Round 16 Issue #2: Capture all settings atomically before updating event bits
    // This prevents race condition where settings could change between reads
    bool boilerEnabled, heatingEnabled, waterEnabled, waterPriority;

    {
        MutexGuard guard(SRP::getSystemSettingsMutex(), pdMS_TO_TICKS(100));
        if (!guard.hasLock()) {
            LOG_ERROR(TAG, "Failed to acquire settings mutex for sync");
            return;
        }

        SystemSettings& settings = SRP::getSystemSettings();
        boilerEnabled = settings.boilerEnabled;
        heatingEnabled = settings.heatingEnabled;
        waterEnabled = settings.waterEnabled;
        waterPriority = settings.wheaterPriorityEnabled;
    }  // Release mutex before event group operations

    // Now update event bits atomically (no interleaved reads from settings)
    if (boilerEnabled) {
        SRP::setSystemStateEventBits(SystemEvents::SystemState::BOILER_ENABLED);
    } else {
        SRP::clearSystemStateEventBits(SystemEvents::SystemState::BOILER_ENABLED);
    }

    if (heatingEnabled) {
        SRP::setSystemStateEventBits(SystemEvents::SystemState::HEATING_ENABLED);
    } else {
        SRP::clearSystemStateEventBits(SystemEvents::SystemState::HEATING_ENABLED);
    }

    if (waterEnabled) {
        SRP::setSystemStateEventBits(SystemEvents::SystemState::WATER_ENABLED);
    } else {
        SRP::clearSystemStateEventBits(SystemEvents::SystemState::WATER_ENABLED);
    }

    if (waterPriority) {
        SRP::setSystemStateEventBits(SystemEvents::SystemState::WATER_PRIORITY);
    } else {
        SRP::clearSystemStateEventBits(SystemEvents::SystemState::WATER_PRIORITY);
    }

    LOG_INFO(TAG, "Synced: boiler=%d, heating=%d, water=%d, priority=%d",
             boilerEnabled, heatingEnabled, waterEnabled, waterPriority);
}

void StateManager::syncEventBitsToSettings() {
    MutexGuard guard(SRP::getSystemSettingsMutex(), pdMS_TO_TICKS(100));
    if (guard.hasLock()) {
        EventBits_t bits = SRP::getSystemStateEventBits();
        SystemSettings& settings = SRP::getSystemSettings();

        settings.boilerEnabled = (bits & SystemEvents::SystemState::BOILER_ENABLED) != 0;
        settings.heatingEnabled = (bits & SystemEvents::SystemState::HEATING_ENABLED) != 0;
        settings.waterEnabled = (bits & SystemEvents::SystemState::WATER_ENABLED) != 0;
        settings.wheaterPriorityEnabled = (bits & SystemEvents::SystemState::WATER_PRIORITY) != 0;
    }
}
