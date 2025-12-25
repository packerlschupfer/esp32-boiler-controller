// src/utils/CriticalDataStorage.h
#pragma once

#include <Arduino.h>
#include <RuntimeStorage.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "core/SystemResourceProvider.h"
#include "config/ProjectConfig.h"
#include "utils/Utils.h"
#include "shared/SharedRelayReadings.h"
#include "shared/SharedSensorReadings.h"
#include "events/SystemEventsGenerated.h"
#include "LoggingMacros.h"

/**
 * @brief Persistent storage for critical system data
 * 
 * Uses reserved FRAM space to store:
 * - Emergency shutdown state
 * - PID tuning parameters
 * - System runtime counters
 * - Critical error history
 */
class CriticalDataStorage {
public:
    // Storage addresses in reserved FRAM space (0x4C20 - 0x7FFF)
    static constexpr uint16_t ADDR_CRITICAL_BASE = 0x4C20;
    static constexpr uint16_t ADDR_EMERGENCY_STATE = ADDR_CRITICAL_BASE;        // 64 bytes
    static constexpr uint16_t ADDR_PID_TUNING = ADDR_CRITICAL_BASE + 0x40;     // 128 bytes
    static constexpr uint16_t ADDR_RUNTIME_DATA = ADDR_CRITICAL_BASE + 0xC0;    // 64 bytes
    static constexpr uint16_t ADDR_LOG_INDICES = ADDR_CRITICAL_BASE + 0xF0;    // 16 bytes - log position indices
    static constexpr uint16_t ADDR_ERROR_CIRCULAR = ADDR_CRITICAL_BASE + 0x100; // 4KB circular buffer
    static constexpr uint16_t ADDR_SAFETY_LOG = ADDR_CRITICAL_BASE + 0x1100;    // 1KB safety events
    
    // Data structures
    struct EmergencyState {
        uint32_t magic;           // 0xDEADBEEF when valid
        uint32_t timestamp;       // When emergency occurred
        uint8_t reason;           // Emergency reason code
        uint8_t activeRelays;     // Relay state before shutdown
        float lastBoilerTemp;     // Last known boiler temp
        float lastPressure;       // Last known pressure
        bool wasHeating;          // Was heating active
        bool wasWaterActive;      // Was water heating active
        uint32_t errorCode;       // Associated error code
        uint32_t crc;            // CRC32 of data
    } __attribute__((packed));
    
    struct PIDTuningData {
        uint32_t magic;           // 0xPID00001 when valid
        struct {
            float kp, ki, kd;
            float outputMin, outputMax;
            uint32_t lastTuned;   // Timestamp of last tuning
            bool isAutoTuned;
        } controllers[4];         // Support up to 4 PID controllers
        uint32_t crc;
    } __attribute__((packed));
    
    struct RuntimeCounters {
        uint32_t magic;           // 0xRUN00001 when valid
        uint32_t totalRuntime;    // Total system runtime in seconds
        uint32_t burnerRuntime;   // Burner runtime in seconds
        uint32_t heatingCycles;   // Number of heating cycles
        uint32_t waterCycles;     // Number of water heating cycles
        uint32_t emergencyStops;  // Number of emergency stops
        uint32_t lastBootTime;    // Last boot timestamp
        uint32_t crc;
    } __attribute__((packed));
    
    struct ErrorLogEntry {
        uint32_t timestamp;
        uint16_t errorCode;
        uint8_t severity;         // 0=Info, 1=Warning, 2=Error, 3=Critical
        uint8_t source;           // Module that generated error
        float value1, value2;     // Associated values
    } __attribute__((packed));
    
    struct SafetyEvent {
        uint32_t timestamp;
        uint8_t eventType;        // Safety event type
        uint8_t action;           // Action taken
        uint16_t data;            // Event-specific data
    } __attribute__((packed));

    // Log position indices - stored in FRAM to avoid scanning on boot
    struct LogIndices {
        uint32_t magic;           // 0x4C4F4749 ("LOGI") when valid
        uint16_t errorLogIndex;   // Next write position for error log
        uint16_t safetyLogIndex;  // Next write position for safety log
        uint32_t crc;             // CRC32 for validation
    } __attribute__((packed));
    
private:
    static rtstorage::RuntimeStorage* storage_;
    static EmergencyState cachedEmergency_;
    static PIDTuningData cachedPID_;
    static RuntimeCounters cachedCounters_;
    static uint16_t errorLogIndex_;
    static uint16_t safetyLogIndex_;
    static bool initialized_;

    // Round 20 Issue #2: Mutex to protect I2C bus access during FRAM operations
    static SemaphoreHandle_t framMutex_;

    // CRC calculation
    static uint32_t calculateCRC32(const void* data, size_t length);

    // Low-level FRAM access (protected by framMutex_)
    static bool writeToFRAM(uint16_t address, const void* data, size_t size);
    static bool readFromFRAM(uint16_t address, void* data, size_t size);
    
public:
    /**
     * @brief Initialize critical data storage
     */
    static bool begin() {
        storage_ = SRP::getRuntimeStorage();
        if (!storage_ || !storage_->isConnected()) {
            LOG_ERROR("CriticalData", "FRAM not available for critical data storage");
            return false;
        }

        // Round 20 Issue #2: Create mutex for I2C bus protection
        if (!framMutex_) {
            framMutex_ = xSemaphoreCreateMutex();
            if (!framMutex_) {
                LOG_ERROR("CriticalData", "Failed to create FRAM mutex");
                return false;
            }
        }

        // Load cached data
        loadEmergencyState();
        loadPIDTuning();
        loadRuntimeCounters();

        // Load log indices directly from FRAM (fast path - no scanning)
        if (!loadLogIndices()) {
            // Indices not found or corrupted - scan to rebuild (only on first boot or after corruption)
            LOG_WARN("CriticalData", "Log indices not found, scanning buffers...");
            findErrorLogPosition();
            findSafetyLogPosition();
            saveLogIndices();  // Persist for next boot
        }

        initialized_ = true;
        LOG_INFO("CriticalData", "Critical data storage initialized");
        return true;
    }
    
    // Emergency State Management
    // Round 21: Enhanced with aggressive retry for critical saves
    static constexpr uint8_t EMERGENCY_SAVE_MAX_RETRIES = 5;
    static constexpr uint32_t EMERGENCY_SAVE_RETRY_DELAY_MS = 10;

    static bool saveEmergencyState(uint8_t reason, uint32_t errorCode) {
        if (!initialized_) {
            LOG_ERROR("CriticalData", "saveEmergencyState called but storage not initialized");
            return false;
        }

        EmergencyState state;
        state.magic = 0xDEADBEEF;
        state.timestamp = millis();
        state.reason = reason;
        state.errorCode = errorCode;

        // Capture current system state
        auto& readings = SRP::getSensorReadings();
        state.lastBoilerTemp = readings.boilerTempOutput;
        state.lastPressure = readings.systemPressure;

        // Get relay states
        auto& relays = SRP::getRelayReadings();
        state.activeRelays = 0;
        if (relays.relayHeatingPump) state.activeRelays |= (1 << 0);
        if (relays.relayWaterPump) state.activeRelays |= (1 << 1);
        if (relays.relayBurnerEnable) state.activeRelays |= (1 << 2);
        if (relays.relayPowerBoost) state.activeRelays |= (1 << 3);
        if (relays.relayWaterMode) state.activeRelays |= (1 << 4);
        if (relays.relayValve) state.activeRelays |= (1 << 5);
        if (relays.relaySpare) state.activeRelays |= (1 << 6);

        // Heating state
        EventBits_t requestBits = xEventGroupGetBits(SRP::getBurnerRequestEventGroup());
        state.wasHeating = (requestBits & SystemEvents::BurnerRequest::HEATING) != 0;
        state.wasWaterActive = (requestBits & SystemEvents::BurnerRequest::WATER) != 0;

        // Calculate CRC
        state.crc = calculateCRC32(&state, sizeof(state) - sizeof(uint32_t));

        // Round 21: Aggressive retry for emergency saves - this data is critical
        for (uint8_t attempt = 0; attempt < EMERGENCY_SAVE_MAX_RETRIES; attempt++) {
            if (writeToFRAM(ADDR_EMERGENCY_STATE, &state, sizeof(state))) {
                cachedEmergency_ = state;
                if (attempt > 0) {
                    LOG_WARN("CriticalData", "Emergency state saved after %d retries: reason=%d, error=0x%08X",
                             attempt, reason, errorCode);
                } else {
                    LOG_WARN("CriticalData", "Emergency state saved: reason=%d, error=0x%08X",
                             reason, errorCode);
                }
                return true;
            }

            // Brief delay before retry - yield to other tasks
            if (attempt < EMERGENCY_SAVE_MAX_RETRIES - 1) {
                LOG_WARN("CriticalData", "Emergency save attempt %d failed, retrying...", attempt + 1);
                vTaskDelay(pdMS_TO_TICKS(EMERGENCY_SAVE_RETRY_DELAY_MS * (attempt + 1)));  // Increasing backoff
            }
        }

        // All retries failed - critical error
        LOG_ERROR("CriticalData", "CRITICAL: Emergency state save FAILED after %d attempts! reason=%d, error=0x%08X",
                  EMERGENCY_SAVE_MAX_RETRIES, reason, errorCode);
        return false;
    }
    
    static bool loadEmergencyState() {
        EmergencyState state;
        if (!readFromFRAM(ADDR_EMERGENCY_STATE, &state, sizeof(state))) {
            return false;
        }
        
        if (state.magic != 0xDEADBEEF) {
            return false;  // No valid emergency state
        }
        
        uint32_t crc = calculateCRC32(&state, sizeof(state) - sizeof(uint32_t));
        if (crc != state.crc) {
            LOG_WARN("CriticalData", "Emergency state CRC mismatch");
            return false;
        }
        
        cachedEmergency_ = state;
        return true;
    }
    
    static bool hasEmergencyState() {
        return cachedEmergency_.magic == 0xDEADBEEF;
    }
    
    static EmergencyState getEmergencyState() {
        return cachedEmergency_;
    }
    
    static bool clearEmergencyState() {
        EmergencyState empty = {0};
        if (writeToFRAM(ADDR_EMERGENCY_STATE, &empty, sizeof(empty))) {
            cachedEmergency_ = empty;
            LOG_INFO("CriticalData", "Emergency state cleared");
            return true;
        }
        return false;
    }
    
    // PID Tuning Management
    static bool savePIDTuning(uint8_t controllerId, float kp, float ki, float kd,
                             float outputMin, float outputMax, bool isAutoTuned) {
        if (!initialized_ || controllerId >= 4) return false;
        
        // Update cached data
        cachedPID_.magic = 0x50494401;  // "PID\x01"
        cachedPID_.controllers[controllerId].kp = kp;
        cachedPID_.controllers[controllerId].ki = ki;
        cachedPID_.controllers[controllerId].kd = kd;
        cachedPID_.controllers[controllerId].outputMin = outputMin;
        cachedPID_.controllers[controllerId].outputMax = outputMax;
        cachedPID_.controllers[controllerId].lastTuned = millis();
        cachedPID_.controllers[controllerId].isAutoTuned = isAutoTuned;
        
        // Calculate CRC
        cachedPID_.crc = calculateCRC32(&cachedPID_, sizeof(cachedPID_) - sizeof(uint32_t));
        
        // Write to FRAM
        if (writeToFRAM(ADDR_PID_TUNING, &cachedPID_, sizeof(cachedPID_))) {
            LOG_INFO("CriticalData", "PID[%d] tuning saved: Kp=%.3f Ki=%.3f Kd=%.3f", 
                     controllerId, kp, ki, kd);
            return true;
        }
        return false;
    }
    
    static bool loadPIDTuning() {
        PIDTuningData data;
        if (!readFromFRAM(ADDR_PID_TUNING, &data, sizeof(data))) {
            return false;
        }
        
        if (data.magic != 0x50494401) {
            // Initialize with defaults
            cachedPID_.magic = 0x50494401;
            for (int i = 0; i < 4; i++) {
                cachedPID_.controllers[i].kp = 2.0f;
                cachedPID_.controllers[i].ki = 0.1f;
                cachedPID_.controllers[i].kd = 0.5f;
                cachedPID_.controllers[i].outputMin = -100.0f;
                cachedPID_.controllers[i].outputMax = 100.0f;
                cachedPID_.controllers[i].isAutoTuned = false;
            }
            return false;
        }
        
        uint32_t crc = calculateCRC32(&data, sizeof(data) - sizeof(uint32_t));
        if (crc != data.crc) {
            LOG_WARN("CriticalData", "PID tuning CRC mismatch");
            return false;
        }
        
        cachedPID_ = data;
        return true;
    }
    
    static bool getPIDTuning(uint8_t controllerId, float& kp, float& ki, float& kd) {
        if (controllerId >= 4) return false;
        
        kp = cachedPID_.controllers[controllerId].kp;
        ki = cachedPID_.controllers[controllerId].ki;
        kd = cachedPID_.controllers[controllerId].kd;
        return cachedPID_.magic == 0x50494401;
    }
    
    // Runtime Counters Management
    static bool incrementRuntimeCounter(uint32_t deltaSeconds, bool burnerActive) {
        if (!initialized_) return false;
        
        cachedCounters_.magic = 0x52554E01;  // "RUN\x01"
        cachedCounters_.totalRuntime += deltaSeconds;
        if (burnerActive) {
            cachedCounters_.burnerRuntime += deltaSeconds;
        }
        
        // Save periodically (every minute)
        // Use Utils::elapsedMs for overflow-safe timing
        static uint32_t lastSave = 0;
        if (Utils::elapsedMs(lastSave) > 60000) {
            saveRuntimeCounters();
            lastSave = millis();
        }
        return true;
    }
    
    static bool incrementCycleCounter(bool isHeating) {
        if (!initialized_) return false;
        
        if (isHeating) {
            cachedCounters_.heatingCycles++;
        } else {
            cachedCounters_.waterCycles++;
        }
        return true;
    }
    
    static bool saveRuntimeCounters() {
        cachedCounters_.crc = calculateCRC32(&cachedCounters_, 
                                            sizeof(cachedCounters_) - sizeof(uint32_t));
        return writeToFRAM(ADDR_RUNTIME_DATA, &cachedCounters_, sizeof(cachedCounters_));
    }
    
    static bool loadRuntimeCounters() {
        RuntimeCounters data;
        if (!readFromFRAM(ADDR_RUNTIME_DATA, &data, sizeof(data))) {
            return false;
        }
        
        if (data.magic != 0x52554E01) {
            // Initialize new counters
            cachedCounters_.magic = 0x52554E01;
            cachedCounters_.totalRuntime = 0;
            cachedCounters_.burnerRuntime = 0;
            cachedCounters_.heatingCycles = 0;
            cachedCounters_.waterCycles = 0;
            cachedCounters_.emergencyStops = 0;
            cachedCounters_.lastBootTime = millis();
            return false;
        }
        
        uint32_t crc = calculateCRC32(&data, sizeof(data) - sizeof(uint32_t));
        if (crc != data.crc) {
            LOG_WARN("CriticalData", "Runtime counters CRC mismatch");
            return false;
        }
        
        cachedCounters_ = data;
        cachedCounters_.lastBootTime = millis();  // Update boot time
        return true;
    }
    
    static RuntimeCounters getRuntimeCounters() {
        return cachedCounters_;
    }
    
    // Error Log Management (Circular Buffer)
    static bool logError(uint16_t errorCode, uint8_t severity, uint8_t source,
                         float value1 = 0, float value2 = 0) {
        if (!initialized_) {
            // Don't LOG_ERROR here to avoid recursion - logError might call itself
            return false;
        }

        ErrorLogEntry entry;
        entry.timestamp = millis();
        entry.errorCode = errorCode;
        entry.severity = severity;
        entry.source = source;
        entry.value1 = value1;
        entry.value2 = value2;

        // Calculate position in circular buffer
        uint16_t address = ADDR_ERROR_CIRCULAR +
                          (errorLogIndex_ * sizeof(ErrorLogEntry));

        if (writeToFRAM(address, &entry, sizeof(entry))) {
            errorLogIndex_ = (errorLogIndex_ + 1) % (4096 / sizeof(ErrorLogEntry));
            saveLogIndices();  // Persist index for fast boot
            return true;
        }
        return false;
    }

    // Safety Event Logging
    static bool logSafetyEvent(uint8_t eventType, uint8_t action, uint16_t data) {
        if (!initialized_) {
            LOG_ERROR("CriticalData", "logSafetyEvent called but storage not initialized");
            return false;
        }

        SafetyEvent event;
        event.timestamp = millis();
        event.eventType = eventType;
        event.action = action;
        event.data = data;

        uint16_t address = ADDR_SAFETY_LOG +
                          (safetyLogIndex_ * sizeof(SafetyEvent));

        if (writeToFRAM(address, &event, sizeof(event))) {
            safetyLogIndex_ = (safetyLogIndex_ + 1) % (1024 / sizeof(SafetyEvent));
            saveLogIndices();  // Persist index for fast boot
            LOG_INFO("CriticalData", "Safety event logged: type=%d, action=%d",
                     eventType, action);
            return true;
        }
        return false;
    }
    
private:
    static void findErrorLogPosition() {
        // Find the next write position in circular buffer
        // Optimized: Read entries in batches to reduce I2C overhead
        constexpr uint16_t ENTRIES_PER_READ = 8;  // Read 8 entries at a time (128 bytes)
        constexpr uint16_t TOTAL_ENTRIES = 4096 / sizeof(ErrorLogEntry);
        ErrorLogEntry batch[ENTRIES_PER_READ];
        errorLogIndex_ = 0;
        uint32_t latestTime = 0;

        for (uint16_t i = 0; i < TOTAL_ENTRIES; i += ENTRIES_PER_READ) {
            uint16_t addr = ADDR_ERROR_CIRCULAR + (i * sizeof(ErrorLogEntry));
            uint16_t entriesToRead = min((uint16_t)ENTRIES_PER_READ, (uint16_t)(TOTAL_ENTRIES - i));

            if (readFromFRAM(addr, batch, entriesToRead * sizeof(ErrorLogEntry))) {
                for (uint16_t j = 0; j < entriesToRead; j++) {
                    if (batch[j].timestamp > latestTime && batch[j].timestamp != 0xFFFFFFFF) {
                        latestTime = batch[j].timestamp;
                        errorLogIndex_ = ((i + j + 1) % TOTAL_ENTRIES);
                    }
                }
            }
        }
    }
    
    static void findSafetyLogPosition() {
        // Optimized: Read entries in batches to reduce I2C overhead
        constexpr uint16_t ENTRIES_PER_READ = 16;  // Read 16 entries at a time (128 bytes)
        constexpr uint16_t TOTAL_ENTRIES = 1024 / sizeof(SafetyEvent);
        SafetyEvent batch[ENTRIES_PER_READ];
        safetyLogIndex_ = 0;
        uint32_t latestTime = 0;

        for (uint16_t i = 0; i < TOTAL_ENTRIES; i += ENTRIES_PER_READ) {
            uint16_t addr = ADDR_SAFETY_LOG + (i * sizeof(SafetyEvent));
            uint16_t entriesToRead = min((uint16_t)ENTRIES_PER_READ, (uint16_t)(TOTAL_ENTRIES - i));

            if (readFromFRAM(addr, batch, entriesToRead * sizeof(SafetyEvent))) {
                for (uint16_t j = 0; j < entriesToRead; j++) {
                    if (batch[j].timestamp > latestTime && batch[j].timestamp != 0xFFFFFFFF) {
                        latestTime = batch[j].timestamp;
                        safetyLogIndex_ = ((i + j + 1) % TOTAL_ENTRIES);
                    }
                }
            }
        }
    }

    // Fast path: Load log indices directly from FRAM (12 bytes vs scanning 5KB)
    static bool loadLogIndices() {
        LogIndices indices;
        if (!readFromFRAM(ADDR_LOG_INDICES, &indices, sizeof(indices))) {
            return false;
        }

        // Verify magic number
        if (indices.magic != 0x4C4F4749) {  // "LOGI"
            return false;
        }

        // Verify CRC
        uint32_t crc = calculateCRC32(&indices, sizeof(indices) - sizeof(uint32_t));
        if (crc != indices.crc) {
            return false;
        }

        // Validate indices are within bounds
        constexpr uint16_t MAX_ERROR_ENTRIES = 4096 / sizeof(ErrorLogEntry);
        constexpr uint16_t MAX_SAFETY_ENTRIES = 1024 / sizeof(SafetyEvent);

        if (indices.errorLogIndex >= MAX_ERROR_ENTRIES ||
            indices.safetyLogIndex >= MAX_SAFETY_ENTRIES) {
            return false;
        }

        errorLogIndex_ = indices.errorLogIndex;
        safetyLogIndex_ = indices.safetyLogIndex;
        return true;
    }

    // Persist log indices to FRAM for fast boot
    static void saveLogIndices() {
        LogIndices indices;
        indices.magic = 0x4C4F4749;  // "LOGI"
        indices.errorLogIndex = errorLogIndex_;
        indices.safetyLogIndex = safetyLogIndex_;
        indices.crc = calculateCRC32(&indices, sizeof(indices) - sizeof(uint32_t));

        (void)writeToFRAM(ADDR_LOG_INDICES, &indices, sizeof(indices));
    }
};