// src/modules/mqtt/MQTTCommandHandlers.cpp
/**
 * @file MQTTCommandHandlers.cpp
 * @brief MQTT command handler implementations
 *
 * Extracted from MQTTTask.cpp to reduce file size and improve maintainability.
 */

#include "MQTTCommandHandlers.h"

#include "config/SystemConstants.h"
#include "config/SystemSettingsStruct.h"
#include "events/SystemEventsGenerated.h"
#include "shared/Temperature.h"
#include "utils/ErrorHandler.h"
#include "utils/ErrorLogFRAM.h"
#include "utils/CriticalDataStorage.h"
#include "core/SystemResourceProvider.h"
#include "core/StateManager.h"
#include "MQTTTopics.h"
#include "utils/MemoryPool.h"
#include "utils/PooledString.h"
#include "modules/tasks/MQTTTask.h"
#include "modules/tasks/TimerSchedulerTask.h"
#include <RuntimeStorage.h>
#include <ArduinoJson.h>
#include <esp_log.h>
#include <cstring>
#include <ctype.h>


static const char* TAG = "MQTT";
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

    // Clean up stale entries from dedup cache
    void cleanupStaleEntries() {
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
    bool isDuplicateCommand(uint32_t hash) {
        uint32_t now = millis();
        // Clean up stale entries first to free space
        cleanupStaleEntries();

        for (size_t i = 0; i < DEDUP_CACHE_SIZE; i++) {
            if (recentCommands[i].hash == hash) {
                uint32_t elapsed = now - recentCommands[i].timestamp;
                if (elapsed < DEDUP_WINDOW_MS) {
                    return true;  // Duplicate within window
                }
            }
        }
        return false;
    }

    // Record command for deduplication
    void recordCommand(uint32_t hash) {
        // Try to find a free slot first
        for (size_t i = 0; i < DEDUP_CACHE_SIZE; i++) {
            if (recentCommands[i].hash == 0) {
                recentCommands[i].hash = hash;
                recentCommands[i].timestamp = millis();
                return;
            }
        }
        // No free slot, use circular index
        recentCommands[recentCommandIndex].hash = hash;
        recentCommands[recentCommandIndex].timestamp = millis();
        recentCommandIndex = (recentCommandIndex + 1) % DEDUP_CACHE_SIZE;
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
                SystemSettings& settings = SRP::getSystemSettings();
                settings.targetTemperatureInside = tempFromFloat(tempValue);
                LOG_INFO(TAG_CMD, "Remote command: Set room target to %.1f°C", tempValue);
                char response[32];
                snprintf(response, sizeof(response), "target:%.1f", tempValue);
                MQTTTask::publish(MQTT_STATUS_HEATING, response, 0, true, MQTTPriority::PRIORITY_HIGH);
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
        SystemSettings& settings = SRP::getSystemSettings();
        settings.targetTemperatureInside = tempFromFloat(tempValue);
        LOG_INFO(TAG_CMD, "Remote command: Set room target to %.1f°C", tempValue);
        char response[32];
        snprintf(response, sizeof(response), "%.1f", tempValue);
        MQTTTask::publish(MQTT_STATUS_HEATING "/target", response, 0, true, MQTTPriority::PRIORITY_HIGH);
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
    if (strcmp(payload, "start") == 0) {
        SRP::setControlRequestsEventBits(SystemEvents::ControlRequest::PID_AUTOTUNE);
        LOG_INFO(TAG_CMD, "Remote command: Start PID auto-tuning");
        MQTTTask::publish(MQTT_STATUS_PID_AUTOTUNE, "starting", 0, true, MQTTPriority::PRIORITY_HIGH);
    } else if (strcmp(payload, "stop") == 0) {
        SRP::setControlRequestsEventBits(SystemEvents::ControlRequest::PID_AUTOTUNE_STOP);
        LOG_INFO(TAG_CMD, "Remote command: Stop PID auto-tuning");
        MQTTTask::publish(MQTT_STATUS_PID_AUTOTUNE, "stopping", 0, true, MQTTPriority::PRIORITY_HIGH);
    } else if (strcmp(payload, "status") == 0) {
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
        LOG_INFO(TAG_CMD, "PID auto-tuning status: %s", status);
    } else if (strcmp(payload, "params") == 0) {
        const SystemSettings& settings = SRP::getSystemSettings();
        // Use getLogBuffer() which provides 256 bytes, sufficient for PID params JSON (~180 bytes)
        auto buffer = MemoryPools::getLogBuffer();
        if (buffer) {
            int written = snprintf(buffer.data(), buffer.size(),
                "{\"spaceHeating\":{\"kp\":%.3f,\"ki\":%.3f,\"kd\":%.3f},"
                "\"waterHeater\":{\"kp\":%.3f,\"ki\":%.3f,\"kd\":%.3f},"
                "\"autotune\":{\"amplitude\":%.1f,\"hysteresis\":%.1f,\"method\":%ld}}",
                settings.spaceHeatingKp, settings.spaceHeatingKi, settings.spaceHeatingKd,
                settings.wHeaterKp, settings.wHeaterKi, settings.wHeaterKd,
                settings.autotuneRelayAmplitude, settings.autotuneHysteresis, (long)settings.autotuneMethod);
            // Check for buffer overflow (snprintf returns required size, not written size)
            if (written < 0 || static_cast<size_t>(written) >= buffer.size()) {
                LOG_ERROR(TAG_CMD, "PID params JSON truncated! needed=%d, available=%d", written, buffer.size());
                MQTTTask::publish(MQTT_STATUS_PID_PARAMS, "{\"error\":\"buffer_overflow\"}", 0, true, MQTTPriority::PRIORITY_HIGH);
            } else {
                MQTTTask::publish(MQTT_STATUS_PID_PARAMS, buffer.c_str(), 0, true, MQTTPriority::PRIORITY_HIGH);
                LOG_INFO(TAG_CMD, "Published PID parameters (%d bytes)", written);
            }
        } else {
            LOG_ERROR(TAG_CMD, "Failed to allocate buffer for PID params");
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
    else if (strcmp(payload, "format") == 0) {
        LOG_WARN(TAG_CMD, "FRAM format requested via MQTT");
        if (storage->format()) {
            MQTTTask::publish(MQTT_STATUS_FRAM_FORMATTED, "ok", 0, true, MQTTPriority::PRIORITY_HIGH);
            LOG_INFO(TAG_CMD, "FRAM formatted successfully");
        } else {
            MQTTTask::publish(MQTT_STATUS_FRAM_ERROR, "format_failed", 0, false, MQTTPriority::PRIORITY_HIGH);
            LOG_ERROR(TAG_CMD, "Failed to format FRAM");
        }
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
    else {
        LOG_WARN(TAG_CMD, "Unknown control command: %s", command);
        MQTTTask::publish(MQTT_STATUS_ERROR, "unknown_command", 0, false, MQTTPriority::PRIORITY_HIGH);
    }
}

} // namespace MQTTCommandHandlers
