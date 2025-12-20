// src/modules/mqtt/MQTTCommandHandlers.cpp
/**
 * @file MQTTCommandHandlers.cpp
 * @brief MQTT command handler implementations
 *
 * Extracted from MQTTTask.cpp to reduce file size and improve maintainability.
 */

#include "MQTTCommandHandlers.h"

#include "config/SystemConstants.h"
#include "config/SafetyConfig.h"
#include "config/SystemSettingsStruct.h"
#include "events/SystemEventsGenerated.h"
#include "shared/Temperature.h"
#include "utils/ErrorHandler.h"
#include "utils/ErrorLogFRAM.h"
#include "utils/CriticalDataStorage.h"
#include "core/SystemResourceProvider.h"
#include "core/StateManager.h"
#include <nvs_flash.h>  // For nvs_flash_erase()
#include "MQTTTopics.h"
#include "utils/MemoryPool.h"
#include "utils/PooledString.h"
#include "modules/tasks/MQTTTask.h"
#include "modules/tasks/TimerSchedulerTask.h"
#include "modules/control/BurnerStateMachine.h"  // H10: For resetLockout()
#include "modules/tasks/BoilerTempControlTask.h"  // For getBoilerTempController()
#include "modules/control/BoilerTempController.h" // For BoilerTempController class
#include <RuntimeStorage.h>
#include <ArduinoJson.h>
#include <esp_log.h>
#include <cstring>
#include <cerrno>  // M2: For errno in strtoul validation
#include <cstdlib> // M2: For strtoul
#include <ctype.h>


static const char* TAG_CMD = "MQTTCmd";

// External function declarations (defined in other compilation units)
extern void triggerCriticalAlert();  // Defined in MonitoringTask.cpp

// Command deduplication to prevent double-execution on QoS retries
namespace {
    struct RecentCommand {
        uint32_t hash;
        uint32_t timestamp;
    };
    // Increased from 8 to 16 to handle rapid command bursts better
    static constexpr size_t DEDUP_CACHE_SIZE = 16;
    static constexpr uint32_t DEDUP_WINDOW_MS = 5000;  // 5 second dedup window
    static RecentCommand recentCommands[DEDUP_CACHE_SIZE] = {};
    static size_t recentCommandIndex = 0;
    // M5: Mutex for thread-safe dedup cache access (MQTT callbacks may come from different contexts)
    static portMUX_TYPE dedupSpinlock = portMUX_INITIALIZER_UNLOCKED;

    // Simple hash function for command deduplication
    inline uint32_t hashCommand(const char* topic, const char* payload) {
        uint32_t hash = 5381;
        if (topic) {
            while (*topic) {
                hash = ((hash << 5) + hash) ^ *topic++;
            }
        }
        hash ^= 0x1F1F1F1F;  // Separator
        if (payload) {
            while (*payload) {
                hash = ((hash << 5) + hash) ^ *payload++;
            }
        }
        return hash;
    }

    // Clean up stale entries from dedup cache (caller must hold spinlock)
    void cleanupStaleEntriesLocked() {
        uint32_t now = millis();
        for (size_t i = 0; i < DEDUP_CACHE_SIZE; i++) {
            if (recentCommands[i].hash != 0) {
                uint32_t elapsed = now - recentCommands[i].timestamp;
                if (elapsed >= DEDUP_WINDOW_MS) {
                    recentCommands[i].hash = 0;  // Mark as free
                    recentCommands[i].timestamp = 0;
                }
            }
        }
    }

    // Check if command is duplicate (seen within dedup window)
    // M5: Thread-safe with spinlock
    bool isDuplicateCommand(uint32_t hash) {
        uint32_t now = millis();
        bool isDuplicate = false;

        portENTER_CRITICAL(&dedupSpinlock);
        // Clean up stale entries first to free space
        cleanupStaleEntriesLocked();

        for (size_t i = 0; i < DEDUP_CACHE_SIZE; i++) {
            if (recentCommands[i].hash == hash) {
                uint32_t elapsed = now - recentCommands[i].timestamp;
                if (elapsed < DEDUP_WINDOW_MS) {
                    isDuplicate = true;
                    break;
                }
            }
        }
        portEXIT_CRITICAL(&dedupSpinlock);
        return isDuplicate;
    }

    // Record command for deduplication
    // M5: Thread-safe with spinlock
    void recordCommand(uint32_t hash) {
        portENTER_CRITICAL(&dedupSpinlock);
        // Try to find a free slot first
        for (size_t i = 0; i < DEDUP_CACHE_SIZE; i++) {
            if (recentCommands[i].hash == 0) {
                recentCommands[i].hash = hash;
                recentCommands[i].timestamp = millis();
                portEXIT_CRITICAL(&dedupSpinlock);
                return;
            }
        }
        // No free slot, use circular index
        recentCommands[recentCommandIndex].hash = hash;
        recentCommands[recentCommandIndex].timestamp = millis();
        recentCommandIndex = (recentCommandIndex + 1) % DEDUP_CACHE_SIZE;
        portEXIT_CRITICAL(&dedupSpinlock);
    }
}

namespace MQTTCommandHandlers {

void handleSystemCommand(const char* payload) {
    if (strcmp(payload, "on") == 0 || strcmp(payload, "enable") == 0 || strcmp(payload, "1") == 0) {
        StateManager::setBoilerEnabled(true);  // Atomic: updates event bits + settings
        LOG_INFO(TAG_CMD, "Remote command: Enable boiler system");
        MQTTTask::publish(MQTT_STATUS_SYSTEM, "enabled", 0, true, MQTTPriority::PRIORITY_HIGH);
    } else if (strcmp(payload, "off") == 0 || strcmp(payload, "disable") == 0 || strcmp(payload, "0") == 0) {
        StateManager::setBoilerEnabled(false);
        LOG_INFO(TAG_CMD, "Remote command: Disable boiler system");
        MQTTTask::publish(MQTT_STATUS_SYSTEM, "disabled", 0, true, MQTTPriority::PRIORITY_HIGH);
    } else if (strcmp(payload, "reboot") == 0 || strcmp(payload, "restart") == 0 || strcmp(payload, "reset") == 0) {
        LOG_WARN(TAG_CMD, "Remote command: System reboot requested");
        MQTTTask::publish(MQTT_STATUS_SYSTEM, "rebooting", 0, true, MQTTPriority::PRIORITY_HIGH);

        // Round 21: Save critical state before reboot
        LOG_INFO(TAG_CMD, "Saving state before reboot...");
        CriticalDataStorage::saveRuntimeCounters();
        CriticalDataStorage::logSafetyEvent(
            0x02,  // Event type: System reboot
            0x01,  // Action: Remote requested
            0       // No additional data
        );

        vTaskDelay(pdMS_TO_TICKS(500));  // Give time for MQTT message and saves
        esp_restart();
    } else if (strcmp(payload, "nvs_erase") == 0) {
        LOG_WARN(TAG_CMD, "Remote command: NVS erase requested - ALL SETTINGS WILL BE LOST!");
        MQTTTask::publish(MQTT_STATUS_SYSTEM, "nvs_erasing", 0, true, MQTTPriority::PRIORITY_HIGH);

        // Erase NVS partition
        esp_err_t err = nvs_flash_erase();
        if (err == ESP_OK) {
            LOG_INFO(TAG_CMD, "NVS erased successfully - rebooting to restore defaults");
            MQTTTask::publish(MQTT_STATUS_SYSTEM, "nvs_erased_rebooting", 0, true, MQTTPriority::PRIORITY_HIGH);
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        } else {
            LOG_ERROR(TAG_CMD, "NVS erase failed: 0x%x", err);
            MQTTTask::publish(MQTT_STATUS_SYSTEM, "nvs_erase_failed", 0, true, MQTTPriority::PRIORITY_HIGH);
        }
    }
}

void handleHeatingCommand(const char* payload) {
    if (strcmp(payload, "on") == 0 || strcmp(payload, "enable") == 0 || strcmp(payload, "1") == 0) {
        StateManager::setHeatingEnabled(true);  // Atomic: updates event bits + settings
        LOG_INFO(TAG_CMD, "Remote command: Enable heating");
        MQTTTask::publish(MQTT_STATUS_HEATING, "enabled", 0, true, MQTTPriority::PRIORITY_HIGH);
    } else if (strcmp(payload, "off") == 0 || strcmp(payload, "disable") == 0 || strcmp(payload, "0") == 0) {
        StateManager::setHeatingEnabled(false);
        LOG_INFO(TAG_CMD, "Remote command: Disable heating");
        MQTTTask::publish(MQTT_STATUS_HEATING, "disabled", 0, true, MQTTPriority::PRIORITY_HIGH);
    } else if (strcmp(payload, "override_on") == 0) {
        // Clear the OFF override flag (allow heating)
        StateManager::setHeatingOverrideOff(false);
        SRP::setControlRequestsEventBits(SystemEvents::ControlRequest::HEATING_ON_OVERRIDE);
        SRP::clearControlRequestsEventBits(SystemEvents::ControlRequest::HEATING_OFF_OVERRIDE);
        LOG_INFO(TAG_CMD, "Remote command: Override heating ON (clearing summer mode)");
        MQTTTask::publish(MQTT_STATUS_HEATING, "override_on", 0, true, MQTTPriority::PRIORITY_HIGH);
    } else if (strcmp(payload, "override_off") == 0) {
        // Set the OFF override flag (summer mode - block heating)
        StateManager::setHeatingOverrideOff(true);
        SRP::setControlRequestsEventBits(SystemEvents::ControlRequest::HEATING_OFF_OVERRIDE);
        SRP::clearControlRequestsEventBits(SystemEvents::ControlRequest::HEATING_ON_OVERRIDE);
        LOG_INFO(TAG_CMD, "Remote command: Override heating OFF (summer mode enabled)");
        MQTTTask::publish(MQTT_STATUS_HEATING, "override_off", 0, true, MQTTPriority::PRIORITY_HIGH);
    } else if (strcmp(payload, "normal") == 0) {
        // Clear all override flags (return to normal operation)
        StateManager::setHeatingOverrideOff(false);
        SRP::clearControlRequestsEventBits(SystemEvents::ControlRequest::HEATING_OFF_OVERRIDE |
                                           SystemEvents::ControlRequest::HEATING_ON_OVERRIDE);
        LOG_INFO(TAG_CMD, "Remote command: Heating normal mode (overrides cleared)");
        MQTTTask::publish(MQTT_STATUS_HEATING, "normal", 0, true, MQTTPriority::PRIORITY_HIGH);
    } else {
        // Try to parse as temperature value for room target
        char* endptr = nullptr;
        float tempValue = strtof(payload, &endptr);
        // Check if parsing succeeded (endptr moved past the number)
        if (endptr != payload && (*endptr == '\0' || isspace(*endptr))) {
            if (tempValue >= 15.0f && tempValue <= 30.0f) {
                // C4: Mutex protection for settings write
                if (SRP::takeSystemSettingsMutex(pdMS_TO_TICKS(100))) {
                    SystemSettings& settings = SRP::getSystemSettings();
                    settings.targetTemperatureInside = tempFromFloat(tempValue);
                    SRP::giveSystemSettingsMutex();
                    LOG_INFO(TAG_CMD, "Remote command: Set room target to %.1f°C", tempValue);
                    char response[32];
                    snprintf(response, sizeof(response), "target:%.1f", tempValue);
                    MQTTTask::publish(MQTT_STATUS_HEATING, response, 0, true, MQTTPriority::PRIORITY_HIGH);
                } else {
                    LOG_ERROR(TAG_CMD, "Failed to acquire settings mutex");
                }
            } else {
                LOG_WARN(TAG_CMD, "Invalid room target temp %.1f (must be 15-30°C)", tempValue);
            }
        }
        // If parsing failed, silently ignore (unknown command)
    }
}

void handleRoomTargetCommand(const char* payload) {
    char* endptr = nullptr;
    float tempValue = strtof(payload, &endptr);
    // Check if parsing succeeded (endptr moved past the number)
    if (endptr == payload || (*endptr != '\0' && !isspace(*endptr))) {
        LOG_WARN(TAG_CMD, "Invalid room target format: %s", payload);
        MQTTTask::publish(MQTT_STATUS_HEATING "/target", "error:invalid_format", 0, false, MQTTPriority::PRIORITY_HIGH);
        return;
    }
    if (tempValue >= 15.0f && tempValue <= 30.0f) {
        // C4: Mutex protection for settings write
        if (SRP::takeSystemSettingsMutex(pdMS_TO_TICKS(100))) {
            SystemSettings& settings = SRP::getSystemSettings();
            settings.targetTemperatureInside = tempFromFloat(tempValue);
            SRP::giveSystemSettingsMutex();
            LOG_INFO(TAG_CMD, "Remote command: Set room target to %.1f°C", tempValue);
            char response[32];
            snprintf(response, sizeof(response), "%.1f", tempValue);
            MQTTTask::publish(MQTT_STATUS_HEATING "/target", response, 0, true, MQTTPriority::PRIORITY_HIGH);
        } else {
            LOG_ERROR(TAG_CMD, "Failed to acquire settings mutex");
            MQTTTask::publish(MQTT_STATUS_HEATING "/target", "error:mutex_timeout", 0, false, MQTTPriority::PRIORITY_HIGH);
        }
    } else {
        LOG_WARN(TAG_CMD, "Invalid room target temp %.1f (must be 15-30°C)", tempValue);
        MQTTTask::publish(MQTT_STATUS_HEATING "/target", "error:invalid_range", 0, false, MQTTPriority::PRIORITY_HIGH);
    }
}

void handleWaterCommand(const char* payload) {
    if (strcmp(payload, "on") == 0 || strcmp(payload, "enable") == 0 || strcmp(payload, "1") == 0) {
        StateManager::setWaterEnabled(true);  // Atomic: updates event bits + settings
        LOG_INFO(TAG_CMD, "Remote command: Enable water heating");
        MQTTTask::publish(MQTT_STATUS_WATER, "enabled", 0, true, MQTTPriority::PRIORITY_HIGH);
    } else if (strcmp(payload, "off") == 0 || strcmp(payload, "disable") == 0 || strcmp(payload, "0") == 0) {
        StateManager::setWaterEnabled(false);
        LOG_INFO(TAG_CMD, "Remote command: Disable water heating");
        MQTTTask::publish(MQTT_STATUS_WATER, "disabled", 0, true, MQTTPriority::PRIORITY_HIGH);
    } else if (strcmp(payload, "override_on") == 0) {
        // Clear the OFF override flag (allow water heating)
        StateManager::setWaterOverrideOff(false);
        SRP::setControlRequestsEventBits(SystemEvents::ControlRequest::WATER_ON_OVERRIDE);
        SRP::clearControlRequestsEventBits(SystemEvents::ControlRequest::WATER_OFF_OVERRIDE);
        LOG_INFO(TAG_CMD, "Remote command: Override water heating ON (clearing block)");
        MQTTTask::publish(MQTT_STATUS_WATER, "override_on", 0, true, MQTTPriority::PRIORITY_HIGH);
    } else if (strcmp(payload, "override_off") == 0) {
        // Set the OFF override flag (block water heating)
        StateManager::setWaterOverrideOff(true);
        SRP::setControlRequestsEventBits(SystemEvents::ControlRequest::WATER_OFF_OVERRIDE);
        SRP::clearControlRequestsEventBits(SystemEvents::ControlRequest::WATER_ON_OVERRIDE);
        LOG_INFO(TAG_CMD, "Remote command: Override water heating OFF (blocked)");
        MQTTTask::publish(MQTT_STATUS_WATER, "override_off", 0, true, MQTTPriority::PRIORITY_HIGH);
    } else if (strcmp(payload, "normal") == 0) {
        // Clear all override flags (return to normal operation)
        StateManager::setWaterOverrideOff(false);
        SRP::clearControlRequestsEventBits(SystemEvents::ControlRequest::WATER_OFF_OVERRIDE |
                                           SystemEvents::ControlRequest::WATER_ON_OVERRIDE);
        LOG_INFO(TAG_CMD, "Remote command: Water heating normal mode (overrides cleared)");
        MQTTTask::publish(MQTT_STATUS_WATER, "normal", 0, true, MQTTPriority::PRIORITY_HIGH);
    } else if (strcmp(payload, "priority_on") == 0 || strcmp(payload, "priority_enable") == 0) {
        StateManager::setWaterPriorityEnabled(true);
        LOG_INFO(TAG_CMD, "Remote command: Enable water heating priority");
        MQTTTask::publish(MQTT_STATUS_WATER_PRIORITY, "enabled", 0, true, MQTTPriority::PRIORITY_HIGH);
    } else if (strcmp(payload, "priority_off") == 0 || strcmp(payload, "priority_disable") == 0) {
        StateManager::setWaterPriorityEnabled(false);
        LOG_INFO(TAG_CMD, "Remote command: Disable water heating priority");
        MQTTTask::publish(MQTT_STATUS_WATER_PRIORITY, "disabled", 0, true, MQTTPriority::PRIORITY_HIGH);
    }
}

void handlePIDAutotuneCommand(const char* payload) {
    // PID auto-tuning now available via BoilerTempController
    // Uses relay feedback method with FULL power oscillations

    if (strcmp(payload, "start") == 0) {
        // Start auto-tuning via event bit - BoilerTempControlTask will handle it
        SRP::setControlRequestsEventBits(SystemEvents::ControlRequest::PID_AUTOTUNE);
        LOG_INFO(TAG_CMD, "Remote command: Start boiler PID auto-tuning");
        MQTTTask::publish(MQTT_STATUS_PID_AUTOTUNE, "starting", 0, true, MQTTPriority::PRIORITY_HIGH);
    } else if (strcmp(payload, "stop") == 0) {
        SRP::setControlRequestsEventBits(SystemEvents::ControlRequest::PID_AUTOTUNE_STOP);
        LOG_INFO(TAG_CMD, "Remote command: Stop boiler PID auto-tuning");
        MQTTTask::publish(MQTT_STATUS_PID_AUTOTUNE, "stopping", 0, true, MQTTPriority::PRIORITY_HIGH);
    } else if (strcmp(payload, "status") == 0) {
        // Check auto-tuning status via event bits
        EventBits_t heatingBits = xEventGroupGetBits(SRP::getHeatingEventGroup());
        const char* status = "idle";
        if (heatingBits & SystemEvents::HeatingEvent::AUTOTUNE_RUNNING) {
            status = "running";
        } else if (heatingBits & SystemEvents::HeatingEvent::AUTOTUNE_COMPLETE) {
            status = "complete";
        } else if (heatingBits & SystemEvents::HeatingEvent::AUTOTUNE_FAILED) {
            status = "failed";
        }
        MQTTTask::publish(MQTT_STATUS_PID_AUTOTUNE, status, 0, true, MQTTPriority::PRIORITY_HIGH);
        LOG_INFO(TAG_CMD, "Boiler PID auto-tuning status: %s", status);
    } else if (strcmp(payload, "params") == 0) {
        // Report boiler PID parameters
        const SystemSettings& settings = SRP::getSystemSettings();
        auto buffer = MemoryPools::getLogBuffer();
        if (buffer) {
            int written = snprintf(buffer.data(), buffer.size(),
                "{\"boilerPID\":{\"kp\":%.3f,\"ki\":%.4f,\"kd\":%.3f},"
                "\"note\":\"Boiler temp PID - controls power level (OFF/HALF/FULL)\"}",
                settings.spaceHeatingKp, settings.spaceHeatingKi, settings.spaceHeatingKd);
            if (written >= 0 && static_cast<size_t>(written) < buffer.size()) {
                MQTTTask::publish(MQTT_STATUS_PID_PARAMS, buffer.c_str(), 0, true, MQTTPriority::PRIORITY_HIGH);
                LOG_INFO(TAG_CMD, "Published boiler PID parameters");
            }
        }
    } else if (strncmp(payload, "method:", 7) == 0) {
        // Set tuning method: method:zn_pi, method:zn_pid, method:tyreus, method:cohen, method:lambda
        const char* method = payload + 7;
        BoilerTempController* controller = getBoilerTempController();
        if (controller) {
            if (controller->setTuningMethod(method)) {
                LOG_INFO(TAG_CMD, "Remote command: Set tuning method to '%s'", method);
                char response[64];
                snprintf(response, sizeof(response), "{\"method\":\"%s\",\"status\":\"set\"}", method);
                MQTTTask::publish(MQTT_STATUS_PID_AUTOTUNE, response, 0, true, MQTTPriority::PRIORITY_HIGH);
            } else {
                LOG_WARN(TAG_CMD, "Invalid tuning method: %s", method);
                MQTTTask::publish(MQTT_STATUS_PID_AUTOTUNE,
                    "{\"error\":\"invalid_method\",\"valid\":[\"zn_pi\",\"zn_pid\",\"tyreus\",\"cohen\",\"lambda\"]}",
                    0, true, MQTTPriority::PRIORITY_HIGH);
            }
        } else {
            LOG_ERROR(TAG_CMD, "BoilerTempController not available");
            MQTTTask::publish(MQTT_STATUS_PID_AUTOTUNE, "{\"error\":\"controller_not_ready\"}",
                0, true, MQTTPriority::PRIORITY_HIGH);
        }
    }
}

void handleStatusCommand() {
    // This calls the static method in MQTTTask - would need to be refactored
    // For now, just signal that status was requested
    LOG_INFO(TAG_CMD, "Status request received");
    // The actual publishing is done by publishSystemState() in MQTTTask
}

void handleFRAMErrorsCommand(const char* payload) {
    rtstorage::RuntimeStorage* storage = SRP::getRuntimeStorage();
    if (!storage) {
        MQTTTask::publish(MQTT_STATUS_FRAM_ERRORS_ERROR, "not_available", 0, false, MQTTPriority::PRIORITY_HIGH);
        return;
    }

    if (strcmp(payload, "stats") == 0) {
        ErrorLogFRAM::ErrorStats stats = ErrorLogFRAM::getStats();

        auto buffer = MemoryPools::getTempBuffer();
        if (!buffer) {
            LOG_ERROR(TAG_CMD, "Failed to allocate buffer for FRAM error stats");
            return;
        }

        snprintf(buffer.data(), buffer.size(),
                 "{\"total\":%lu,\"critical\":%lu,\"last\":%lu,\"oldest\":%lu,\"unique\":%u}",
                 stats.totalErrors, stats.criticalErrors, stats.lastErrorTime,
                 stats.oldestErrorTime, stats.uniqueErrors);

        MQTTTask::publish(MQTT_STATUS_FRAM_ERRORS_STATS, buffer.c_str(), 0, false, MQTTPriority::PRIORITY_LOW);
        LOG_INFO(TAG_CMD, "Published FRAM error statistics");
    }
    else if (strcmp(payload, "clear") == 0) {
        ErrorLogFRAM::clear();
        MQTTTask::publish(MQTT_STATUS_FRAM_ERRORS_CLEARED, "ok", 0, true, MQTTPriority::PRIORITY_HIGH);
        LOG_INFO(TAG_CMD, "Cleared FRAM error log");
    }
    else {
        LOG_WARN(TAG_CMD, "Unknown FRAM error command: %s", payload);
        MQTTTask::publish(MQTT_STATUS_FRAM_ERRORS_ERROR, "unknown_command", 0, false, MQTTPriority::PRIORITY_HIGH);
    }
}

void handleFRAMCommand(const char* payload) {
    rtstorage::RuntimeStorage* storage = SRP::getRuntimeStorage();
    if (!storage) {
        MQTTTask::publish(MQTT_STATUS_FRAM_ERROR, "not_available", 0, false, MQTTPriority::PRIORITY_HIGH);
        return;
    }

    if (strcmp(payload, "status") == 0) {
        auto buffer = MemoryPools::getTempBuffer();
        if (!buffer) {
            LOG_ERROR(TAG_CMD, "Failed to allocate buffer for FRAM status");
            return;
        }

        bool connected = storage->isConnected();
        uint32_t size = storage->getSize();
        bool integrity = storage->verifyIntegrity();

        snprintf(buffer.data(), buffer.size(),
                 "{\"connected\":%s,\"size\":%lu,\"integrity\":%s}",
                 connected ? "true" : "false",
                 size,
                 integrity ? "true" : "false");

        MQTTTask::publish(MQTT_STATUS_FRAM_STATUS, buffer.c_str(), 0, false, MQTTPriority::PRIORITY_LOW);
        LOG_INFO(TAG_CMD, "Published FRAM status");
    }
    else if (strcmp(payload, "counters") == 0) {
        auto buffer = MemoryPools::getTempBuffer();
        if (!buffer) {
            LOG_ERROR(TAG_CMD, "Failed to allocate buffer for FRAM counters");
            return;
        }

        uint32_t b = storage->getCounter(rtstorage::COUNTER_BURNER_STARTS);
        uint32_t h = storage->getCounter(rtstorage::COUNTER_HEATING_PUMP_STARTS);
        uint32_t w = storage->getCounter(rtstorage::COUNTER_WATER_PUMP_STARTS);
        uint32_t e = storage->getCounter(rtstorage::COUNTER_ERROR_COUNT);

        snprintf(buffer.data(), buffer.size(),
                 "{\"b\":%lu,\"h\":%lu,\"w\":%lu,\"e\":%lu}",
                 b, h, w, e);

        MQTTTask::publish(MQTT_STATUS_FRAM_COUNTERS, buffer.c_str(), 0, false, MQTTPriority::PRIORITY_LOW);
        LOG_INFO(TAG_CMD, "Published FRAM counters");
    }
    else if (strcmp(payload, "runtime") == 0) {
        auto buffer = MemoryPools::getTempBuffer();
        if (!buffer) {
            LOG_ERROR(TAG_CMD, "Failed to allocate buffer for FRAM runtime");
            return;
        }

        uint32_t t = (uint32_t)storage->getRuntimeHours(rtstorage::RUNTIME_TOTAL);
        uint32_t h = (uint32_t)storage->getRuntimeHours(rtstorage::RUNTIME_HEATING);
        uint32_t w = (uint32_t)storage->getRuntimeHours(rtstorage::RUNTIME_WATER);
        uint32_t b = (uint32_t)storage->getRuntimeHours(rtstorage::RUNTIME_BURNER);

        snprintf(buffer.data(), buffer.size(),
                 "{\"t\":%lu,\"h\":%lu,\"w\":%lu,\"b\":%lu}",
                 t, h, w, b);

        MQTTTask::publish(MQTT_STATUS_FRAM_RUNTIME, buffer.c_str(), 0, false, MQTTPriority::PRIORITY_LOW);
        LOG_INFO(TAG_CMD, "Published FRAM runtime hours");
    }
    else if (strcmp(payload, "reset_counters") == 0) {
        (void)storage->setCounter(rtstorage::COUNTER_BURNER_STARTS, 0);
        (void)storage->setCounter(rtstorage::COUNTER_HEATING_PUMP_STARTS, 0);
        (void)storage->setCounter(rtstorage::COUNTER_WATER_PUMP_STARTS, 0);
        (void)storage->setCounter(rtstorage::COUNTER_ERROR_COUNT, 0);

        MQTTTask::publish(MQTT_STATUS_FRAM_COUNTERS_RESET, "ok", 0, true, MQTTPriority::PRIORITY_HIGH);
        LOG_INFO(TAG_CMD, "Reset FRAM counters");
    }
    else if (strcmp(payload, "format_confirm") == 0) {
        // H17: Require explicit confirmation to prevent accidental data loss
        LOG_WARN(TAG_CMD, "FRAM format CONFIRMED via MQTT - erasing all data!");
        if (storage->format()) {
            MQTTTask::publish(MQTT_STATUS_FRAM_FORMATTED, "ok", 0, true, MQTTPriority::PRIORITY_HIGH);
            LOG_INFO(TAG_CMD, "FRAM formatted successfully");
        } else {
            MQTTTask::publish(MQTT_STATUS_FRAM_ERROR, "format_failed", 0, false, MQTTPriority::PRIORITY_HIGH);
            LOG_ERROR(TAG_CMD, "Failed to format FRAM");
        }
    }
    else if (strcmp(payload, "format") == 0) {
        // H17: Warn user that confirmation is required
        LOG_WARN(TAG_CMD, "FRAM format requested - send 'format_confirm' to proceed");
        MQTTTask::publish(MQTT_STATUS_FRAM_ERROR, "use_format_confirm", 0, false, MQTTPriority::PRIORITY_HIGH);
    }
    else if (strcmp(payload, "save_pid") == 0) {
        SRP::setControlRequestsEventBits(SystemEvents::ControlRequest::SAVE_PARAMETERS);
        MQTTTask::publish(MQTT_STATUS_FRAM_PID_SAVE, "requested", 0, true, MQTTPriority::PRIORITY_HIGH);
        LOG_INFO(TAG_CMD, "Requested PID parameters save to FRAM");
    }
    else {
        LOG_WARN(TAG_CMD, "Unknown FRAM command: %s", payload);
        MQTTTask::publish(MQTT_STATUS_FRAM_ERROR, "unknown_command", 0, false, MQTTPriority::PRIORITY_HIGH);
    }
}

void handleErrorCommand(const char* topic, const char* payload) {
    // Validate inputs
    if (!topic) {
        LOG_ERROR(TAG_CMD, "Invalid error command: null topic");
        return;
    }

    LOG_INFO(TAG_CMD, "Error command on %s: %s", topic, payload ? payload : "(empty)");

    // Extract the command from the topic (after "errors/")
    const char* lastSlash = strrchr(topic, '/');
    if (!lastSlash) return;
    const char* command = lastSlash + 1;

    // Handle error log commands
    if (strcmp(command, "list") == 0) {
        // Get error count from payload (default 10)
        int count = 10;
        if (payload && strlen(payload) > 0) {
            char* endptr = nullptr;
            long parsed = strtol(payload, &endptr, 10);
            if (endptr != payload && *endptr == '\0' && parsed > 0 && parsed <= 50) {
                count = static_cast<int>(parsed);
            }
        }

        // Export errors as JSON
        auto buffer = MemoryPools::getString();
        if (!buffer) {
            LOG_ERROR(TAG_CMD, "Failed to allocate buffer for error list");
            return;
        }

        if (ErrorLogFRAM::exportToJson(buffer.data(), buffer.size(), count)) {
            MQTTTask::publish(MQTT_STATUS_ERRORS_LIST, buffer.c_str(), 0, false, MQTTPriority::PRIORITY_LOW);
            LOG_INFO(TAG_CMD, "Published error list with %d entries", count);
        } else {
            MQTTTask::publish(MQTT_STATUS_ERRORS_ERROR, "export_failed", 0, false, MQTTPriority::PRIORITY_HIGH);
        }
    }
    else if (strcmp(command, "clear") == 0) {
        ErrorLogFRAM::clear();
        MQTTTask::publish(MQTT_STATUS_ERRORS_CLEARED, "ok", 0, true, MQTTPriority::PRIORITY_HIGH);
        LOG_INFO(TAG_CMD, "Cleared error log");
    }
    else if (strcmp(command, "stats") == 0) {
        ErrorLogFRAM::ErrorStats stats = ErrorLogFRAM::getStats();

        auto buffer = MemoryPools::getTempBuffer();
        if (!buffer) {
            LOG_ERROR(TAG_CMD, "Failed to allocate buffer for error stats");
            return;
        }

        snprintf(buffer.data(), buffer.size(),
                 "{\"total\":%lu,\"critical\":%lu,\"last\":%lu,\"oldest\":%lu,\"unique\":%u}",
                 stats.totalErrors, stats.criticalErrors, stats.lastErrorTime,
                 stats.oldestErrorTime, stats.uniqueErrors);

        MQTTTask::publish(MQTT_STATUS_ERRORS_STATS, buffer.c_str(), 0, false, MQTTPriority::PRIORITY_LOW);
        LOG_INFO(TAG_CMD, "Published error statistics");
    }
    else if (strcmp(command, "critical") == 0) {
        // Get critical errors
        ErrorLogFRAM::ErrorEntry criticalErrors[5];
        size_t errCount = ErrorLogFRAM::getCriticalErrors(criticalErrors, 5);

        JsonDocument doc;  // ArduinoJson v7
        JsonArray errors = doc["critical"].to<JsonArray>();

        for (size_t i = 0; i < errCount; i++) {
            JsonObject error = errors.add<JsonObject>();
            error["time"] = criticalErrors[i].timestamp;
            error["code"] = criticalErrors[i].errorCode;
            error["msg"] = criticalErrors[i].message;
            if (criticalErrors[i].context[0] != '\0') {
                error["ctx"] = criticalErrors[i].context;
            }
        }

        auto buffer = MemoryPools::getString();
        if (!buffer) {
            LOG_ERROR(TAG_CMD, "Failed to allocate buffer for critical errors");
            return;
        }

        serializeJson(doc, buffer.data(), buffer.size());
        MQTTTask::publish(MQTT_STATUS_ERRORS_CRITICAL, buffer.c_str(), 0, false, MQTTPriority::PRIORITY_LOW);
        LOG_INFO(TAG_CMD, "Published %zu critical errors", errCount);
    }
    else if (strcmp(command, "dump") == 0) {
        // Trigger critical alert in monitoring task which will dump error log
        ::triggerCriticalAlert();
        MQTTTask::publish(MQTT_STATUS_ERRORS_DUMP, "triggered", 0, false, MQTTPriority::PRIORITY_HIGH);
        LOG_INFO(TAG_CMD, "Triggered error log dump via critical alert");
    }
    else {
        LOG_WARN(TAG_CMD, "Unknown error command: %s", command);
        MQTTTask::publish(MQTT_STATUS_ERRORS_ERROR, "unknown_command", 0, false, MQTTPriority::PRIORITY_HIGH);
    }
}

void handleSchedulerCommand(const char* topic, const char* payload) {
    LOG_INFO(TAG_CMD, "Scheduler command on %s: %s", topic, payload);

    const char* lastSlash = strrchr(topic, '/');
    if (!lastSlash) return;
    const char* command = lastSlash + 1;

    // Forward to TimerScheduler namespace for processing
    TimerScheduler::processMQTTCommand(String(command), String(payload));
}

/**
 * @brief Publish current safety configuration
 */
void publishSafetyConfig() {
    char json[128];
    int written = snprintf(json, sizeof(json),
        "{\"pump_prot\":%lu,\"sensor_stale\":%lu,\"post_purge\":%lu}",
        SafetyConfig::pumpProtectionMs,
        SafetyConfig::sensorStaleMs,
        SafetyConfig::postPurgeMs);

    // M1: Check for buffer overflow
    if (written < 0 || static_cast<size_t>(written) >= sizeof(json)) {
        LOG_WARN(TAG_CMD, "Safety config JSON truncated");
    }

    MQTTTask::publish(MQTT_STATUS_SAFETY_CONFIG, json, 0, true, MQTTPriority::PRIORITY_MEDIUM);
}

/**
 * @brief Handle safety configuration commands
 */
static void handleSafetyConfigCommand(const char* topic, const char* payload) {
    LOG_INFO(TAG_CMD, "Safety config command: %s = %s", topic, payload);

    // M2: Proper input validation using strtoul
    char* endptr = nullptr;
    errno = 0;
    unsigned long parsed = strtoul(payload, &endptr, 10);

    // Check for parse errors:
    // 1. No digits found (endptr == payload)
    // 2. Overflow (errno == ERANGE)
    // 3. Trailing non-whitespace characters
    // 4. Empty string
    if (endptr == payload || errno == ERANGE ||
        (endptr != nullptr && *endptr != '\0' && *endptr != ' ' && *endptr != '\n')) {
        LOG_WARN(TAG_CMD, "Invalid numeric value: %s", payload);
        MQTTTask::publish(MQTT_STATUS_ERROR, "invalid_numeric_value", 0, false, MQTTPriority::PRIORITY_HIGH);
        return;
    }

    uint32_t value = static_cast<uint32_t>(parsed);
    bool success = false;

    if (strstr(topic, "pump_protection_ms") != nullptr) {
        success = SafetyConfig::setPumpProtection(value);
    } else if (strstr(topic, "sensor_stale_ms") != nullptr) {
        success = SafetyConfig::setSensorStale(value);
    } else if (strstr(topic, "post_purge_ms") != nullptr) {
        success = SafetyConfig::setPostPurge(value);
    }
    // Return preheating (thermal shock mitigation) config
    else if (strstr(topic, "preheat_enabled") != nullptr) {
        SystemSettings& settings = SRP::getSystemSettings();
        settings.preheatEnabled = (value != 0);
        success = true;
        LOG_INFO(TAG_CMD, "Preheat enabled: %s", settings.preheatEnabled ? "true" : "false");
    } else if (strstr(topic, "preheat_off_multiplier") != nullptr) {
        if (value >= 1 && value <= 10) {
            SystemSettings& settings = SRP::getSystemSettings();
            settings.preheatOffMultiplier = static_cast<uint8_t>(value);
            success = true;
            LOG_INFO(TAG_CMD, "Preheat OFF multiplier: %u", settings.preheatOffMultiplier);
        }
    } else if (strstr(topic, "preheat_max_cycles") != nullptr) {
        if (value >= 1 && value <= 20) {
            SystemSettings& settings = SRP::getSystemSettings();
            settings.preheatMaxCycles = static_cast<uint8_t>(value);
            success = true;
            LOG_INFO(TAG_CMD, "Preheat max cycles: %u", settings.preheatMaxCycles);
        }
    } else if (strstr(topic, "preheat_timeout_ms") != nullptr) {
        if (value >= 60000 && value <= 1200000) {  // 1-20 minutes
            SystemSettings& settings = SRP::getSystemSettings();
            settings.preheatTimeoutMs = value;
            success = true;
            LOG_INFO(TAG_CMD, "Preheat timeout: %lu ms", settings.preheatTimeoutMs);
        }
    } else if (strstr(topic, "preheat_pump_min_ms") != nullptr) {
        if (value >= 1000 && value <= 30000) {  // 1-30 seconds
            SystemSettings& settings = SRP::getSystemSettings();
            settings.preheatPumpMinMs = static_cast<uint16_t>(value);
            success = true;
            LOG_INFO(TAG_CMD, "Preheat pump min change: %u ms", settings.preheatPumpMinMs);
        }
    } else if (strstr(topic, "preheat_safe_diff") != nullptr) {
        if (value >= 100 && value <= 300) {  // 10-30°C in tenths
            SystemSettings& settings = SRP::getSystemSettings();
            settings.preheatSafeDiff = static_cast<Temperature_t>(value);
            success = true;
            char tempBuf[16];
            formatTemp(tempBuf, sizeof(tempBuf), settings.preheatSafeDiff);
            LOG_INFO(TAG_CMD, "Preheat safe differential: %s°C", tempBuf);
        }
    }
    // Weather-compensated heating control
    else if (strstr(topic, "weather_control_enabled") != nullptr) {
        SystemSettings& settings = SRP::getSystemSettings();
        settings.useWeatherCompensatedControl = (value != 0);
        success = true;
        LOG_INFO(TAG_CMD, "Weather-compensated control: %s",
                settings.useWeatherCompensatedControl ? "ENABLED" : "DISABLED");
    } else if (strstr(topic, "outside_heating_threshold") != nullptr) {
        if (value >= 50 && value <= 200) {  // 5-20°C in tenths
            SystemSettings& settings = SRP::getSystemSettings();
            settings.outsideTempHeatingThreshold = static_cast<Temperature_t>(value);
            success = true;
            char tempBuf[16];
            formatTemp(tempBuf, sizeof(tempBuf), settings.outsideTempHeatingThreshold);
            LOG_INFO(TAG_CMD, "Outside heating threshold: %s°C", tempBuf);
        }
    } else if (strstr(topic, "room_overheat_margin") != nullptr) {
        if (value >= 10 && value <= 50) {  // 1-5°C in tenths
            SystemSettings& settings = SRP::getSystemSettings();
            settings.roomTempOverheatMargin = static_cast<Temperature_t>(value);
            success = true;
            char tempBuf[16];
            formatTemp(tempBuf, sizeof(tempBuf), settings.roomTempOverheatMargin);
            LOG_INFO(TAG_CMD, "Room overheat margin: %s°C", tempBuf);
        }
    } else if (strstr(topic, "room_curve_shift_factor") != nullptr) {
        // Accept value as float * 10 (e.g., 20 = 2.0)
        float factor = static_cast<float>(value) / 10.0f;
        if (factor >= 1.0f && factor <= 4.0f) {
            SystemSettings& settings = SRP::getSystemSettings();
            settings.roomTempCurveShiftFactor = factor;
            success = true;
            LOG_INFO(TAG_CMD, "Room curve shift factor: %.1f", settings.roomTempCurveShiftFactor);
        }
    } else {
        LOG_WARN(TAG_CMD, "Unknown safety config topic: %s", topic);
        return;
    }

    if (success) {
        SafetyConfig::saveToNVS();
        publishSafetyConfig();  // Confirm new values
        LOG_INFO(TAG_CMD, "Config updated successfully");
    } else {
        LOG_WARN(TAG_CMD, "Invalid config value: %s = %lu", topic, value);
        MQTTTask::publish(MQTT_STATUS_ERROR, "invalid_config_value", 0, false, MQTTPriority::PRIORITY_HIGH);
    }
}

void routeControlCommand(const char* topic, const char* payload) {
    if (!topic || !payload) {
        LOG_ERROR(TAG_CMD, "Invalid control command: null %s", !topic ? "topic" : "payload");
        return;
    }

    // Deduplication check - prevents double-execution on MQTT QoS retries
    uint32_t cmdHash = hashCommand(topic, payload);
    if (isDuplicateCommand(cmdHash)) {
        LOG_DEBUG(TAG_CMD, "Duplicate command ignored: %s", topic);
        return;
    }
    recordCommand(cmdHash);

    LOG_INFO(TAG_CMD, "Control command on %s: %s", topic, payload);

    // Extract the command from the topic (after "cmd/boiler/")
    const char* lastSlash = strrchr(topic, '/');
    if (!lastSlash) return;
    const char* command = lastSlash + 1;

    // Route to appropriate handler
    if (strcmp(command, "system") == 0) {
        handleSystemCommand(payload);
    }
    else if (strcmp(command, "heating") == 0) {
        handleHeatingCommand(payload);
    }
    else if (strcmp(command, "room_target") == 0) {
        handleRoomTargetCommand(payload);
    }
    else if (strcmp(command, "water") == 0 || strcmp(command, "wheater") == 0) {
        handleWaterCommand(payload);
    }
    else if (strcmp(command, "pid_autotune") == 0) {
        handlePIDAutotuneCommand(payload);
    }
    else if (strcmp(command, "status") == 0) {
        handleStatusCommand();
    }
    else if (strcmp(command, "fram_errors") == 0) {
        handleFRAMErrorsCommand(payload);
    }
    else if (strcmp(command, "fram") == 0) {
        handleFRAMCommand(payload);
    }
    else if (strcmp(command, "errors") == 0) {
        handleErrorCommand(topic, payload);
    }
    else if (strcmp(command, "burner_reset") == 0) {
        // H10: Remote lockout/error reset command
        if (strcmp(payload, "lockout") == 0 || strcmp(payload, "reset") == 0) {
            LOG_WARN(TAG_CMD, "Remote command: Reset burner lockout");
            BurnerStateMachine::resetLockout();
            MQTTTask::publish("status/boiler/burner", "lockout_reset", 0, true, MQTTPriority::PRIORITY_HIGH);
        } else {
            LOG_WARN(TAG_CMD, "Unknown burner_reset payload: %s (use 'lockout' or 'reset')", payload);
        }
    }
    else if (strstr(topic, "/config/") != nullptr) {
        handleSafetyConfigCommand(topic, payload);
    }
    else {
        LOG_WARN(TAG_CMD, "Unknown control command: %s", command);
        MQTTTask::publish(MQTT_STATUS_ERROR, "unknown_command", 0, false, MQTTPriority::PRIORITY_HIGH);
    }
}

} // namespace MQTTCommandHandlers
