// src/diagnostics/MQTTDiagnostics.cpp
#include "MQTTDiagnostics.h"
#include "LoggingMacros.h"
#include "shared/SharedSensorReadings.h"
#include "shared/SharedRelayReadings.h"
#include "shared/SharedResources.h"
#include "shared/Temperature.h"  // For temperature conversions
#include "shared/Pressure.h"  // For pressure types
#include "modules/control/BurnerStateMachine.h"
#include "modules/control/BurnerSystemController.h"
#include "modules/control/PIDControlModule.h"
#include "utils/ErrorHandler.h"
#include "utils/MutexRetryHelper.h"
#include "RuntimeDiagnostics.h"
#include "SemaphoreGuard.h"
#include <esp_system.h>
#include <esp_timer.h>
#include "core/SystemResourceProvider.h"
// #include "monitoring/MemoryMonitor.h" // Removed - not needed
#include <math.h>  // For isnan, isinf
#include "utils/PooledString.h"
#include "core/QueueManager.h"
#include "config/ProjectConfig.h"
#include "config/SystemConstants.h"


const char* MQTTDiagnostics::TAG = "MQTTDiagnostics";
MQTTDiagnostics* MQTTDiagnostics::instance = nullptr;

// No external declarations needed - using SRP methods

MQTTDiagnostics::MQTTDiagnostics() 
    : enabled(false)
    , taskHandle(nullptr) {
    mutex = xSemaphoreCreateMutex();
}

MQTTDiagnostics::~MQTTDiagnostics() {
    if (taskHandle != nullptr) {
        vTaskDelete(taskHandle);
    }
    if (mutex != nullptr) {
        vSemaphoreDelete(mutex);
    }
}

MQTTDiagnostics* MQTTDiagnostics::getInstance() {
    if (instance == nullptr) {
        instance = new MQTTDiagnostics();
    }
    return instance;
}

void MQTTDiagnostics::cleanup() {
    if (instance != nullptr) {
        delete instance;
        instance = nullptr;
        LOG_INFO(TAG, "MQTTDiagnostics singleton cleaned up");
    }
}

bool MQTTDiagnostics::initialize(const std::string& baseTopic, 
                                 PublishCallback publishCallback,
                                 uint32_t taskStackSize,
                                 UBaseType_t taskPriority) {
    SemaphoreGuard guard(mutex, pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "Failed to acquire mutex for initialization");
        return false;
    }
    
    this->baseTopic = baseTopic;
    this->publishCallback = publishCallback;
    
    if (publishCallback == nullptr) {
        LOG_ERROR(TAG, "Invalid publish callback");
        return false;
    }
    
    // Create diagnostics task
    BaseType_t result = xTaskCreate(
        diagnosticsTask,
        "MQTTDiagnostics",
        taskStackSize,
        this,
        taskPriority,
        &taskHandle
    );
    
    if (result != pdPASS) {
        LOG_ERROR(TAG, "Failed to create diagnostics task");
        return false;
    }
    
    metrics.startTime = xTaskGetTickCount();
    enabled = true;
    
    LOG_INFO(TAG, "MQTT Diagnostics initialized with base topic: %s", baseTopic.c_str());
    return true;
}

void MQTTDiagnostics::forceUpdate() {
    if (taskHandle != nullptr) {
        xTaskNotifyGive(taskHandle);
    }
}

void MQTTDiagnostics::publishError(const char* component, const char* error, const char* details) {
    if (!enabled || !publishCallback) return;

    JsonDocument doc;  // ArduinoJson v7
    doc["timestamp"] = esp_timer_get_time() / 1000000; // Convert to seconds
    doc["component"] = component;
    doc["error"] = error;
    if (details) {
        doc["details"] = details;
    }
    doc["severity"] = "ERROR";

    publish("errors", doc, false); // Don't retain errors
}

void MQTTDiagnostics::publishMaintenanceAlert(const char* component, const char* alert, int severity) {
    if (!enabled || !publishCallback) return;

    JsonDocument doc;  // ArduinoJson v7
    doc["timestamp"] = esp_timer_get_time() / 1000000;
    doc["component"] = component;
    doc["alert"] = alert;
    doc["severity"] = severity;

    publish("maintenance/alerts", doc, true);
}

void MQTTDiagnostics::diagnosticsTask(void* pvParameters) {
    MQTTDiagnostics* self = static_cast<MQTTDiagnostics*>(pvParameters);
    self->runDiagnostics();
}

void MQTTDiagnostics::runDiagnostics() {
    LOG_INFO(TAG, "Diagnostics task started");
    
    while (true) {
        if (enabled) {
            TickType_t loopStart = xTaskGetTickCount();
            
            // Publish various diagnostic information based on intervals
            publishHealthStatus();
            publishMemoryStatus();
            publishTaskStatus();
            publishSensorStatus();
            publishRelayStatus();
            publishNetworkStatus();
            publishPerformanceMetrics();
            publishPIDStatus();
            publishBurnerStatus();
            publishMaintenanceStatus();
            publishQueueStatus();
            
            // Update performance metrics
            TickType_t loopEnd = xTaskGetTickCount();
            uint32_t loopTime = (loopEnd - loopStart) * portTICK_PERIOD_MS;
            
            metrics.loopCount++;
            if (loopTime > metrics.maxLoopTime) {
                metrics.maxLoopTime = loopTime;
            }
            metrics.avgLoopTime = ((metrics.avgLoopTime * (metrics.loopCount - 1)) + loopTime) / metrics.loopCount;
        }
        
        // Wait for notification or timeout
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    }
}

void MQTTDiagnostics::publishHealthStatus() {
    if (!shouldPublish(lastPublish.health, intervals.health)) return;
    
    // TEMPORARILY DISABLED: Health monitor causing crash at 60 seconds
    // The generateHealthReport() function appears to be accessing task info
    // that causes a double exception
    /*
    HealthMonitor* healthMonitor = SRP::getHealthMonitor();
    if (healthMonitor == nullptr) {
        // Fallback if health monitor not initialized
        StaticJsonDocument<256> doc;
        doc["status"] = "unknown";
        doc["error"] = "HealthMonitor not initialized";
        doc["uptime"] = (xTaskGetTickCount() * portTICK_PERIOD_MS) / 1000;
        publish("health", doc, true);
        return;
    }
    
    std::string healthJson = healthMonitor->generateHealthReport();
    publish("health", healthJson.c_str(), true);
    */
    
    // Simple health status with abbreviated keys
    JsonDocument doc;  // ArduinoJson v7
    doc["s"] = "ok";  // status
    doc["u"] = (xTaskGetTickCount() * portTICK_PERIOD_MS) / 1000;  // uptime
    doc["h"] = ESP.getFreeHeap();  // heap
    doc["mh"] = ESP.getMinFreeHeap();  // min heap
    publish("health", doc, true);
}

void MQTTDiagnostics::publishMemoryStatus() {
    if (!shouldPublish(lastPublish.memory, intervals.memory)) return;

    JsonDocument doc;  // ArduinoJson v7
    collectMemoryInfo(doc);
    publish("memory", doc, true);
}

void MQTTDiagnostics::publishTaskStatus() {
    if (!shouldPublish(lastPublish.tasks, intervals.tasks)) return;

    JsonDocument doc;  // ArduinoJson v7
    collectTaskInfo(doc);
    publish("tasks", doc, true);
}

void MQTTDiagnostics::publishSensorStatus() {
    if (!shouldPublish(lastPublish.sensors, intervals.sensors)) return;

    JsonDocument doc;  // ArduinoJson v7
    collectSensorInfo(doc);
    publish("sensors", doc, true);
}

void MQTTDiagnostics::publishRelayStatus() {
    if (!shouldPublish(lastPublish.relays, intervals.relays)) return;

    JsonDocument doc;  // ArduinoJson v7
    collectRelayInfo(doc);
    publish("relays", doc, true);
}

void MQTTDiagnostics::publishNetworkStatus() {
    if (!shouldPublish(lastPublish.network, intervals.network)) return;

    JsonDocument doc;  // ArduinoJson v7
    collectNetworkInfo(doc);
    publish("network", doc, true);
}

void MQTTDiagnostics::publishPerformanceMetrics() {
    if (!shouldPublish(lastPublish.performance, intervals.performance)) return;

    JsonDocument doc;  // ArduinoJson v7
    doc["uptime_seconds"] = (xTaskGetTickCount() - metrics.startTime) * portTICK_PERIOD_MS / 1000;
    doc["diagnostics"]["loops"] = metrics.loopCount;
    doc["diagnostics"]["avg_loop_ms"] = metrics.avgLoopTime;
    doc["diagnostics"]["max_loop_ms"] = metrics.maxLoopTime;
    doc["diagnostics"]["publishes"] = metrics.publishCount;
    doc["diagnostics"]["publish_failures"] = metrics.publishFailures;

    publish("performance", doc, true);
}

void MQTTDiagnostics::publishPIDStatus() {
    if (!shouldPublish(lastPublish.pid, intervals.pid)) return;

    // Get PID controller instance (would need to be made accessible)
    // For now, create placeholder
    JsonDocument doc;  // ArduinoJson v7
    doc["enabled"] = true;
    doc["setpoint"] = 70.0;
    doc["current_temp"] = 65.5;
    doc["output"] = 45.0;
    doc["parameters"]["Kp"] = 2.0;
    doc["parameters"]["Ki"] = 0.1;
    doc["parameters"]["Kd"] = 0.5;
    doc["auto_tuning"]["active"] = false;
    doc["auto_tuning"]["progress"] = 0;

    publish("pid", doc, true);
}

void MQTTDiagnostics::publishBurnerStatus() {
    if (!shouldPublish(lastPublish.burner, intervals.burner)) return;

    JsonDocument doc;  // ArduinoJson v7
    
    // Get current burner state
    BurnerSMState state = BurnerStateMachine::getCurrentState();
    const char* stateStr = "UNKNOWN";
    
    switch (state) {
        case BurnerSMState::IDLE: stateStr = "IDLE"; break;
        case BurnerSMState::PRE_PURGE: stateStr = "PRE_PURGE"; break;
        case BurnerSMState::IGNITION: stateStr = "IGNITION"; break;
        case BurnerSMState::RUNNING_LOW: stateStr = "RUNNING_LOW"; break;
        case BurnerSMState::RUNNING_HIGH: stateStr = "RUNNING_HIGH"; break;
        case BurnerSMState::MODE_SWITCHING: stateStr = "MODE_SWITCHING"; break;
        case BurnerSMState::POST_PURGE: stateStr = "POST_PURGE"; break;
        case BurnerSMState::LOCKOUT: stateStr = "LOCKOUT"; break;
        case BurnerSMState::ERROR: stateStr = "ERROR"; break;
    }
    
    doc["state"] = stateStr;
    doc["state_numeric"] = (int)state;
    
    // Add burner controller status via BurnerSystemController
    BurnerSystemController* controller = SRP::getBurnerSystemController();
    if (controller) {
        doc["active"] = controller->isActive();
        doc["power_level"] = static_cast<int>(controller->getCurrentPowerLevel());
    } else {
        doc["active"] = false;
        doc["power_level"] = 0;
    }
    
    publish("burner", doc, true);
}

void MQTTDiagnostics::publishMaintenanceStatus() {
    if (!shouldPublish(lastPublish.maintenance, intervals.maintenance)) return;

    JsonDocument doc;  // ArduinoJson v7

    // Calculate runtime hours
    uint32_t uptimeHours = ((xTaskGetTickCount() - metrics.startTime) * portTICK_PERIOD_MS) / (1000 * 3600);
    doc["runtime_hours"] = uptimeHours;

    // Predictive maintenance alerts based on runtime
    JsonArray alerts = doc["alerts"].to<JsonArray>();

    if (uptimeHours > 2000) {  // 2000 hours
        JsonObject alert = alerts.add<JsonObject>();
        alert["component"] = "burner";
        alert["message"] = "Burner maintenance recommended";
        alert["severity"] = 2;
    }

    if (uptimeHours > 4000) {  // 4000 hours
        JsonObject alert = alerts.add<JsonObject>();
        alert["component"] = "sensors";
        alert["message"] = "Sensor calibration recommended";
        alert["severity"] = 1;
    }

    publish("maintenance", doc, true);
}

bool MQTTDiagnostics::shouldPublish(TickType_t& lastTime, uint32_t interval) {
    TickType_t now = xTaskGetTickCount();
    if ((now - lastTime) * portTICK_PERIOD_MS >= interval) {
        lastTime = now;
        return true;
    }
    return false;
}

bool MQTTDiagnostics::publish(const char* subtopic, const JsonDocument& doc, bool retain) {
    if (!enabled || !publishCallback) return false;
    
    // Use direct pool access for JSON buffer
    auto* buffer = MemoryPools::jsonBufferPool.allocate();
    if (!buffer) {
        LOG_ERROR(TAG, "Failed to allocate buffer for JSON payload");
        return false;
    }
    serializeJson(doc, buffer->data, sizeof(buffer->data));
    bool result = publish(subtopic, buffer->data, retain);
    MemoryPools::jsonBufferPool.deallocate(buffer);
    return result;
}

bool MQTTDiagnostics::publish(const char* subtopic, const char* payload, bool retain) {
    if (!enabled || !publishCallback) return false;
    
    auto topic = MemoryPools::getString();
    if (!topic) {
        LOG_ERROR(TAG, "Failed to allocate buffer for topic");
        return false;
    }
    topic.printf("%s/diagnostics/%s", baseTopic.c_str(), subtopic);
    
    bool result = publishCallback(topic.c_str(), payload, 1, retain);
    
    metrics.publishCount++;
    if (!result) {
        metrics.publishFailures++;
        LOG_WARN(TAG, "Failed to publish to %s", topic.c_str());
    }
    
    return result;
}

// getFullTopic method removed - using snprintf directly instead

void MQTTDiagnostics::collectTaskInfo(JsonDocument& doc) {
    // Get task information using FreeRTOS APIs
    UBaseType_t taskCount = uxTaskGetNumberOfTasks();
    doc["task_count"] = taskCount;

    JsonArray tasks = doc["tasks"].to<JsonArray>();

    // Allocate array for task status
    const UBaseType_t maxTasks = 40;  // Reasonable maximum for ESP32
    TaskStatus_t* taskStatusArray = (TaskStatus_t*)pvPortMalloc(maxTasks * sizeof(TaskStatus_t));

    if (taskStatusArray != nullptr) {
        uint32_t totalRunTime;
        UBaseType_t actualTaskCount = uxTaskGetSystemState(taskStatusArray, maxTasks, &totalRunTime);

        // Add info for each task (limit to first 10 for MQTT message size)
        UBaseType_t tasksToReport = (actualTaskCount < 10) ? actualTaskCount : 10;

        for (UBaseType_t i = 0; i < tasksToReport; i++) {
            JsonObject taskObj = tasks.add<JsonObject>();
            taskObj["name"] = taskStatusArray[i].pcTaskName;
            taskObj["priority"] = taskStatusArray[i].uxCurrentPriority;
            taskObj["state"] = taskStatusArray[i].eCurrentState;
            taskObj["stack_hwm"] = taskStatusArray[i].usStackHighWaterMark;
            taskObj["number"] = taskStatusArray[i].xTaskNumber;
            
            // Calculate CPU usage percentage if runtime stats are enabled
            // Use 64-bit arithmetic to prevent overflow (ulRunTimeCounter can exceed 42M)
            if (totalRunTime > 0) {
                uint32_t taskRunTimePercent = static_cast<uint32_t>(
                    (static_cast<uint64_t>(taskStatusArray[i].ulRunTimeCounter) * 100ULL) / totalRunTime
                );
                taskObj["cpu_percent"] = taskRunTimePercent;
            }
        }
        
        // Add summary for critical tasks
        for (UBaseType_t i = 0; i < actualTaskCount; i++) {
            // Check for low stack conditions
            if (taskStatusArray[i].usStackHighWaterMark < 200) {
                char warnMsg[64];
                snprintf(warnMsg, sizeof(warnMsg), "Task '%s' low stack: %lu", 
                        taskStatusArray[i].pcTaskName, 
                        (unsigned long)taskStatusArray[i].usStackHighWaterMark);
                LOG_WARN("MQTTDiag", "%s", warnMsg);
            }
        }
        
        vPortFree(taskStatusArray);
    } else {
        // Fallback if memory allocation fails
        LOG_WARN("MQTTDiag", "Failed to allocate memory for task status array");
    }
}

void MQTTDiagnostics::collectMemoryInfo(JsonDocument& doc) {
    // Simple memory stats without MemoryMonitor
    size_t totalHeap = ESP.getHeapSize();
    size_t freeHeap = ESP.getFreeHeap();
    size_t minFreeHeap = ESP.getMinFreeHeap();
    size_t maxAllocHeap = ESP.getMaxAllocHeap();
    
    doc["heap"]["total"] = totalHeap;
    doc["heap"]["free"] = freeHeap;
    doc["heap"]["minimum"] = minFreeHeap;
    doc["heap"]["largest_block"] = maxAllocHeap;
    
    // Simple fragmentation calculation
    float fragmentation = 0.0f;
    if (maxAllocHeap > 0 && freeHeap > 0) {
        fragmentation = 100.0f * (1.0f - (float)maxAllocHeap / (float)freeHeap);
    }
    doc["heap"]["fragmentation_percent"] = fragmentation;
    
    // Simple health status based on free heap using centralized thresholds
    const char* statusStr = "OK";
    if (freeHeap < SystemConstants::System::MIN_FREE_HEAP_CRITICAL) {
        statusStr = "CRITICAL";
    } else if (freeHeap < SystemConstants::System::MIN_FREE_HEAP_WARNING) {
        statusStr = "WARNING";
    }
    doc["heap"]["health_status"] = statusStr;
    
    // Note: Removed memory trend and task stack usage tracking
    // as MemoryMonitor is no longer available
}

// Helper function to format float with proper precision
static void formatFloatDiag(char* buffer, size_t bufferSize, float value, int decimals) {
    if (isnan(value) || isinf(value)) {
        snprintf(buffer, bufferSize, "0.0");
    } else {
        snprintf(buffer, bufferSize, "%.*f", decimals, value);
    }
}

void MQTTDiagnostics::collectSensorInfo(JsonDocument& doc) {
    auto guard = MutexRetryHelper::acquireGuard(
        SRP::getSensorReadingsMutex(),
        "SensorReadings-Diagnostics"
    );
    if (guard) {
        JsonObject temps = doc["temperatures"].to<JsonObject>();
        auto floatBuffer = MemoryPools::getTempBuffer();
        if (!floatBuffer) {
            LOG_ERROR(TAG, "Failed to allocate temp buffer for sensor data");
            return;
        }

        formatFloatDiag(floatBuffer.data(), floatBuffer.size(), tempToFloat(SRP::getSensorReadings().boilerTempOutput), 1);
        temps["boiler_output"]["value"] = String(floatBuffer.c_str());
        temps["boiler_output"]["valid"] = SRP::getSensorReadings().isBoilerTempOutputValid;

        formatFloatDiag(floatBuffer.data(), floatBuffer.size(), tempToFloat(SRP::getSensorReadings().boilerTempReturn), 1);
        temps["boiler_return"]["value"] = String(floatBuffer.c_str());
        temps["boiler_return"]["valid"] = SRP::getSensorReadings().isBoilerTempReturnValid;

        formatFloatDiag(floatBuffer.data(), floatBuffer.size(), tempToFloat(SRP::getSensorReadings().waterHeaterTempTank), 1);
        temps["water_tank"]["value"] = String(floatBuffer.c_str());
        temps["water_tank"]["valid"] = SRP::getSensorReadings().isWaterHeaterTempTankValid;

        formatFloatDiag(floatBuffer.data(), floatBuffer.size(), tempToFloat(SRP::getSensorReadings().outsideTemp), 1);
        temps["outside"]["value"] = String(floatBuffer.c_str());
        temps["outside"]["valid"] = SRP::getSensorReadings().isOutsideTempValid;

        formatFloatDiag(floatBuffer.data(), floatBuffer.size(), tempToFloat(SRP::getSensorReadings().insideTemp), 1);
        temps["inside"]["value"] = String(floatBuffer.c_str());
        temps["inside"]["valid"] = SRP::getSensorReadings().isInsideTempValid;

        formatFloatDiag(floatBuffer.data(), floatBuffer.size(), SRP::getSensorReadings().insideHumidity, 1);
        temps["inside"]["humidity"] = String(floatBuffer.c_str());

        // Add system pressure
        JsonObject pressure = doc["pressure"].to<JsonObject>();
        Pressure_t p = SRP::getSensorReadings().systemPressure;
        char pressureStr[16];
        snprintf(pressureStr, sizeof(pressureStr), "%d.%02d", p / 100, abs(p % 100));
        pressure["value"] = String(pressureStr);
        pressure["unit"] = "BAR";
        pressure["valid"] = SRP::getSensorReadings().isSystemPressureValid;
    }
}

void MQTTDiagnostics::collectRelayInfo(JsonDocument& doc) {
    auto guard = MutexRetryHelper::acquireGuard(
        SRP::getRelayReadingsMutex(),
        "RelayReadings-Diagnostics"
    );
    if (guard) {
        JsonObject relays = doc["relays"].to<JsonObject>();

        // Map individual relay states
        relays["heating_pump"] = SRP::getRelayReadings().relayHeatingPump;
        relays["water_pump"] = SRP::getRelayReadings().relayWaterPump;
        relays["burner_enable"] = SRP::getRelayReadings().relayBurnerEnable;
        relays["water_mode"] = SRP::getRelayReadings().relayWaterMode;
        relays["power_boost"] = SRP::getRelayReadings().relayPowerBoost;
        relays["valve"] = SRP::getRelayReadings().relayValve;
        relays["spare"] = SRP::getRelayReadings().relaySpare;

        if (SRP::getRelayReadings().errorCode != 0) {
            doc["error_code"] = SRP::getRelayReadings().errorCode;
        }
    }
}

void MQTTDiagnostics::collectNetworkInfo(JsonDocument& doc) {
    // Placeholder - would collect actual network statistics
    doc["connected"] = true;
    doc["rssi"] = -65;
    doc["ip"] = "192.168.1.100";
}

void MQTTDiagnostics::publishQueueStatus() {
    if (!shouldPublish(lastPublish.queues, intervals.queues)) return;
    
    // Let QueueManager publish its own metrics
    QueueManager::getInstance().publishMetrics();
}

// BLE status publishing removed - no longer using BLE sensors