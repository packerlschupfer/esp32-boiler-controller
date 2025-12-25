// src/modules/tasks/MonitoringTaskEventDriven.cpp
// Event-driven version of the monitoring task

#include "MonitoringTask.h"
#include "modules/tasks/EventDrivenPatterns.h"
#include "config/SystemConstants.h"
#include "config/ProjectConfig.h"
#include "LoggingMacros.h"

#include <Watchdog.h>
#include <freertos/timers.h>
#include <TaskManager.h>
#include "core/SystemResourceProvider.h"
#include "events/SystemEventsGenerated.h"
#include <EthernetManager.h>
#include <esp_log.h>
#include "shared/SharedSensorReadings.h"
#include "shared/SharedRelayReadings.h"
#include "shared/Temperature.h"
#include "config/SystemSettings.h"
#include "utils/ErrorLogFRAM.h"
#include "utils/ErrorHandler.h"
#include <algorithm>
#include <cstring>

static const char* TAG = "Monitoring";

// Timer handles for different monitoring intervals
static TimerHandle_t healthCheckTimer = nullptr;
static TimerHandle_t detailedMonitorTimer = nullptr;

// Use timer periods from SystemConstants
using namespace SystemConstants::Timing;

// Event bits for monitoring
enum MonitoringEventBits : uint32_t {
    MONITOR_EVENT_HEALTH_CHECK = (1 << 0),
    MONITOR_EVENT_DETAILED = (1 << 1),
    MONITOR_EVENT_ON_DEMAND = (1 << 2),
    MONITOR_EVENT_CRITICAL = (1 << 3)
};

// Event aggregator for monitoring events - initialized on first use
// Protected by atomic flag to prevent race with timer callbacks
static EventAggregator* monitoringEvents = nullptr;
static volatile bool monitoringEventsReady = false;  // Atomic flag for safe timer access

// Static member definitions
TaskHandle_t MonitoringTask::taskHandle = nullptr;

// Forward declarations of monitoring functions
static void logNetworkStatus();
static void logSensorStatus();
static void logRelayStatus();
static void logCompactStatus();
static void logAllTasks();
static void dumpErrorLog(size_t maxErrors = 10);

/**
 * @brief Timer callback for health checks
 */
static void healthCheckTimerCallback(TimerHandle_t xTimer) {
    // Use atomic flag to safely check if event aggregator is ready
    // This prevents race condition during task initialization
    if (!monitoringEventsReady) {
        return;  // Silently skip - task not ready yet
    }

    // Double-check pointer after flag (belt and suspenders)
    if (monitoringEvents != nullptr && monitoringEvents->getHandle() != nullptr) {
        monitoringEvents->setEvent(MONITOR_EVENT_HEALTH_CHECK);
    }
}

/**
 * @brief Timer callback for detailed monitoring
 */
static void detailedMonitorTimerCallback(TimerHandle_t xTimer) {
    // Use atomic flag to safely check if event aggregator is ready
    if (!monitoringEventsReady) {
        return;  // Silently skip - task not ready yet
    }

    // Double-check pointer after flag
    if (monitoringEvents != nullptr && monitoringEvents->getHandle() != nullptr) {
        monitoringEvents->setEvent(MONITOR_EVENT_DETAILED);
    }
}

/**
 * @brief Event-driven monitoring task main function
 */
void MonitoringTaskEventDriven(void* pvParameters) {
    LOG_INFO(TAG, "Task started @ %lu ms", millis());
    
    // Register with watchdog with simple approach
    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::enabled(
        false,   // not critical - won't reset system
        SystemConstants::System::WDT_MONITORING_MS
    );

    bool registered = SRP::getTaskManager().registerCurrentTaskWithWatchdog("Monitoring", wdtConfig);
    if (registered) {
        LOG_INFO(TAG, "WDT registered %lums", SystemConstants::System::WDT_MONITORING_MS);
        (void)SRP::getTaskManager().feedWatchdog();
    } else {
        LOG_WARN(TAG, "WDT registration failed - continuing anyway");
    }
    
    // H1: Initialize event aggregator - clean up any existing one first (handles task restart)
    // NOTE: Uses heap allocation because EventAggregator calls xEventGroupCreate() in constructor,
    // which requires FreeRTOS scheduler to be running. Static allocation would fail at boot.
    // Memory is intentionally never freed during normal operation (~100 bytes, allocated once).
    monitoringEventsReady = false;  // Mark as not ready during initialization
    if (monitoringEvents != nullptr) {
        LOG_WARN(TAG, "Cleaning up existing EventAggregator (task restart?)");
        delete monitoringEvents;
        monitoringEvents = nullptr;
    }

    LOG_DEBUG(TAG, "Creating new EventAggregator...");
    monitoringEvents = new EventAggregator();
    if (monitoringEvents == nullptr) {
        LOG_ERROR(TAG, "Failed to allocate memory for EventAggregator");
        vTaskDelete(NULL);
        return;
    }
    if (monitoringEvents->getHandle() == nullptr) {
        LOG_ERROR(TAG, "EventAggregator created but handle is NULL");
        delete monitoringEvents;
        monitoringEvents = nullptr;
        vTaskDelete(NULL);
        return;
    }
    LOG_DEBUG(TAG, "Event aggregator created successfully with handle: %p",
              monitoringEvents->getHandle());
    
    // Feed watchdog before waiting for sensors (even if registration failed)
    (void)SRP::getTaskManager().feedWatchdog();
    
    // Skip waiting for sensors - monitoring can work without them initially
    LOG_INFO(TAG, "Skipping sensor wait to avoid blocking at %lu ms", millis());
    
    // Just check if sensors are already ready without waiting
    EventBits_t sensorBits = xEventGroupGetBits(SRP::getSensorEventGroup());
    if (sensorBits & SystemEvents::SensorUpdate::FIRST_READ_COMPLETE) {
        LOG_INFO(TAG, "Sensors already ready at %lu ms", millis());
    } else {
        LOG_INFO(TAG, "Sensors not ready yet at %lu ms - will monitor anyway", millis());
    }
    
    // Feed watchdog
    (void)SRP::getTaskManager().feedWatchdog();

    LOG_INFO(TAG, "Starting event-driven monitoring at %lu ms", millis());

    // Create timers for periodic monitoring
    healthCheckTimer = xTimerCreate(
        "HealthCheck",
        pdMS_TO_TICKS(HEALTH_CHECK_INTERVAL_MS),
        pdTRUE,  // Auto-reload
        nullptr,
        healthCheckTimerCallback
    );

    detailedMonitorTimer = xTimerCreate(
        "DetailedMon",
        pdMS_TO_TICKS(DETAILED_MONITOR_INTERVAL_MS),
        pdTRUE,  // Auto-reload
        nullptr,
        detailedMonitorTimerCallback
    );

    if (!healthCheckTimer || !detailedMonitorTimer) {
        LOG_ERROR(TAG, "Failed to create timers");
        // Cleanup any timer that was created
        if (healthCheckTimer) {
            xTimerDelete(healthCheckTimer, 0);
            healthCheckTimer = nullptr;
        }
        if (detailedMonitorTimer) {
            xTimerDelete(detailedMonitorTimer, 0);
            detailedMonitorTimer = nullptr;
        }
        // Cleanup event aggregator
        if (monitoringEvents) {
            delete monitoringEvents;
            monitoringEvents = nullptr;
        }
        vTaskDelete(NULL);
        return;
    }

    // Start timers
    if (xTimerStart(healthCheckTimer, pdMS_TO_TICKS(100)) != pdPASS ||
        xTimerStart(detailedMonitorTimer, pdMS_TO_TICKS(100)) != pdPASS) {
        LOG_ERROR(TAG, "Failed to start timers");
        // Cleanup timers
        xTimerDelete(healthCheckTimer, 0);
        xTimerDelete(detailedMonitorTimer, 0);
        healthCheckTimer = nullptr;
        detailedMonitorTimer = nullptr;
        // Cleanup event aggregator
        if (monitoringEvents) {
            delete monitoringEvents;
            monitoringEvents = nullptr;
        }
        vTaskDelete(NULL);
        return;
    }

    LOG_INFO(TAG, "Timers started - Health: %ums, Detailed: %ums",
             HEALTH_CHECK_INTERVAL_MS, DETAILED_MONITOR_INTERVAL_MS);

    // Mark event aggregator as ready - timer callbacks can now safely access it
    monitoringEventsReady = true;

    // Feed watchdog before entering main loop
    (void)SRP::getTaskManager().feedWatchdog();

    LOG_DEBUG(TAG, "Initialization complete, entering main loop...");

    // Main event loop
    EventGroupHandle_t eventGroup = monitoringEvents->getHandle();
    if (!eventGroup) {
        LOG_ERROR(TAG, "Failed to get event group handle!");
        vTaskDelete(NULL);
        return;
    }
    
    LOG_DEBUG(TAG, "Got event group handle: %p", eventGroup);
    
    const EventBits_t ALL_EVENTS = MONITOR_EVENT_HEALTH_CHECK | MONITOR_EVENT_DETAILED | 
                                   MONITOR_EVENT_ON_DEMAND | MONITOR_EVENT_CRITICAL;

    uint32_t loopCount = 0;
    
    LOG_DEBUG(TAG, "About to enter main loop...");
    
    // Feed watchdog once more before entering loop
    (void)SRP::getTaskManager().feedWatchdog();

    while (true) {
        // Feed watchdog FIRST thing in the loop
        (void)SRP::getTaskManager().feedWatchdog();
        
        // Use ESP_LOG directly for critical messages to bypass custom logger
        if (loopCount < 10) {
            LOG_DEBUG(TAG, "Loop %u @ %lu ms", loopCount, millis());
        }
        
        // Wait for any monitoring event with shorter timeout
        EventBits_t events = xEventGroupWaitBits(
            eventGroup,
            ALL_EVENTS,
            pdTRUE,   // Clear bits on exit
            pdFALSE,  // Wait for any bit
            pdMS_TO_TICKS(500)  // 500ms timeout for even more frequent watchdog feeding
        );
        
        // Feed watchdog immediately after wait returns
        (void)SRP::getTaskManager().feedWatchdog();
        
        // Only log watchdog feed occasionally to reduce log pressure
        if (loopCount % 20 == 0) {
            LOG_VERBOSE(TAG, "WDT fed @ %lu ms", millis());
        }

        // Debug logging after wait
        if (events != 0 && loopCount < 10) {
            LOG_VERBOSE(TAG, "Events: 0x%08X", events);
        }
        
        // Increment loop counter
        loopCount++;

        if (events == 0) {
            // Timeout - just feed watchdog and continue
            continue;
        }

        // Handle health check event
        if (events & MONITOR_EVENT_HEALTH_CHECK) {
            // Quick health check - only critical metrics
            uint32_t freeHeap = ESP.getFreeHeap();
            uint32_t minFreeHeap = ESP.getMinFreeHeap();
            
            // H14: Use standardized heap thresholds - warn at 2x warning threshold for early notice
            constexpr uint32_t EARLY_WARNING = SystemConstants::System::MIN_FREE_HEAP_WARNING * 2;
            if (freeHeap < EARLY_WARNING || minFreeHeap < SystemConstants::System::MIN_FREE_HEAP_WARNING) {
                LOG_WARN(TAG, "Low heap! Free: %u, Min: %u",
                         freeHeap, minFreeHeap);
            }
            
            // Log health check completion every 10th time (50 seconds)
            static uint32_t healthCheckCount = 0;
            if (++healthCheckCount % 10 == 0) {
                LOG_VERBOSE(TAG, "Health check #%u - heap: %u", 
                         healthCheckCount, freeHeap);
            }
            
            // Yield to allow other tasks to run
            taskYIELD();
        }

        // Handle detailed monitoring event
        if (events & MONITOR_EVENT_DETAILED) {
            LOG_DEBUG(TAG, "=== DETAILED MONITOR REPORT ===");

            // Compact status report with yields between sections to prevent starving other tasks
            // This is important because logAllTasks() iterates through all 32 tasks
            logCompactStatus();
            taskYIELD();

            logAllTasks();
            (void)SRP::getTaskManager().feedWatchdog();  // Feed after heavy task iteration
            taskYIELD();

            logNetworkStatus();
            logSensorStatus();
            logRelayStatus();

            // Feed watchdog immediately after report
            (void)SRP::getTaskManager().feedWatchdog();
        }

        // Handle on-demand report
        if (events & MONITOR_EVENT_ON_DEMAND) {
            LOG_INFO(TAG, "On-demand report requested");
            // Trigger both health check and detailed monitoring
            if (monitoringEvents != nullptr) {
                monitoringEvents->setEvent(MONITOR_EVENT_HEALTH_CHECK | MONITOR_EVENT_DETAILED);
            }
        }

        // Handle critical alerts
        if (events & MONITOR_EVENT_CRITICAL) {
            LOG_ERROR(TAG, "!!! CRITICAL ALERT !!!");
            // Dump error log for diagnostics
            dumpErrorLog(10);
            // Force immediate detailed monitoring
            if (monitoringEvents != nullptr) {
                monitoringEvents->setEvent(MONITOR_EVENT_DETAILED);
            }
            
            // Could also trigger emergency actions here if needed
        }
    }

    // Cleanup (should never reach here)
    monitoringEventsReady = false;  // Prevent timer callbacks from accessing during cleanup
    if (healthCheckTimer) {
        xTimerStop(healthCheckTimer, 0);
        xTimerDelete(healthCheckTimer, 0);
        healthCheckTimer = nullptr;
    }
    if (detailedMonitorTimer) {
        xTimerStop(detailedMonitorTimer, 0);
        xTimerDelete(detailedMonitorTimer, 0);
        detailedMonitorTimer = nullptr;
    }
    if (monitoringEvents != nullptr) {
        delete monitoringEvents;
        monitoringEvents = nullptr;
    }
    vTaskDelete(NULL);
}

/**
 * @brief Request an immediate monitoring report
 */
void requestMonitoringReport() {
    if (monitoringEventsReady && monitoringEvents != nullptr) {
        monitoringEvents->setEvent(MONITOR_EVENT_ON_DEMAND);
    }
}

/**
 * @brief Trigger a critical alert
 */
void triggerCriticalAlert() {
    if (monitoringEventsReady && monitoringEvents != nullptr) {
        monitoringEvents->setEvent(MONITOR_EVENT_CRITICAL);
    }
}

/**
 * @brief Change monitoring intervals dynamically
 */
bool setMonitoringIntervals(uint32_t healthCheckMs, uint32_t detailedMs) {
    bool success = true;
    
    if (healthCheckTimer && healthCheckMs >= 1000) {
        success &= (xTimerChangePeriod(healthCheckTimer, pdMS_TO_TICKS(healthCheckMs), 
                                       pdMS_TO_TICKS(100)) == pdPASS);
    }
    
    if (detailedMonitorTimer && detailedMs >= 10000) {
        success &= (xTimerChangePeriod(detailedMonitorTimer, pdMS_TO_TICKS(detailedMs), 
                                       pdMS_TO_TICKS(100)) == pdPASS);
    }
    
    return success;
}

// MonitoringTask static method implementations
bool MonitoringTask::init() {
    // Event aggregator will be initialized when task starts
    return true;
}

bool MonitoringTask::start() {
    if (taskHandle != nullptr) {
        return false; // Already running
    }
    
    // Use TaskManager for proper watchdog integration
    // Task will manually register watchdog from its own context
    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::disabled();
    
    bool result = SRP::getTaskManager().startTaskPinned(
        taskFunction,
        "Monitoring",
        STACK_SIZE_MONITORING_TASK,  // Stack size from config
        nullptr,
        PRIORITY_MONITORING_TASK,     // Priority from config
        0,      // Core 0
        wdtConfig
    );
    
    if (result) {
        // Get the task handle after creation
        taskHandle = SRP::getTaskManager().getTaskHandleByName("Monitoring");
    }
    
    return result;
}

void MonitoringTask::stop() {
    if (taskHandle != nullptr) {
        vTaskDelete(taskHandle);
        taskHandle = nullptr;
    }
}

bool MonitoringTask::isRunning() {
    return taskHandle != nullptr;
}

TaskHandle_t MonitoringTask::getTaskHandle() {
    return taskHandle;
}

void MonitoringTask::taskFunction(void* pvParameters) {
    // This is the actual task implementation - it's the MonitoringTaskEventDriven function
    MonitoringTaskEventDriven(pvParameters);
}

// Static helper functions for monitoring

static void logNetworkStatus() {
    // Compact network status
    [[maybe_unused]] bool isConnected = EthernetManager::isConnected();
    LOG_DEBUG(TAG, "ETH: %s", isConnected ? "UP" : "DOWN");
}

static void logSensorStatus() {
    // Rate limit: only log every 10th call to reduce CPU overhead
    static uint8_t logCounter = 0;
    if (++logCounter < 10) {
        return;
    }
    logCounter = 0;

    // Get sensor readings safely
    if (SRP::takeSensorReadingsMutex(pdMS_TO_TICKS(100))) {
        [[maybe_unused]] SharedSensorReadings readings = SRP::getSensorReadings();

        // Batch all sensor data into fewer log lines for efficiency
        // Temperature_t is already in tenths of degrees
        LOG_DEBUG(TAG, "Sens BO:%d.%d BR:%d.%d WT:%d.%d O:%d.%d",
                 readings.isBoilerTempOutputValid ? readings.boilerTempOutput / 10 : -99,
                 readings.isBoilerTempOutputValid ? abs(readings.boilerTempOutput % 10) : 9,
                 readings.isBoilerTempReturnValid ? readings.boilerTempReturn / 10 : -99,
                 readings.isBoilerTempReturnValid ? abs(readings.boilerTempReturn % 10) : 9,
                 readings.isWaterHeaterTempTankValid ? readings.waterHeaterTempTank / 10 : -99,
                 readings.isWaterHeaterTempTankValid ? abs(readings.waterHeaterTempTank % 10) : 9,
                 readings.isOutsideTempValid ? readings.outsideTemp / 10 : -99,
                 readings.isOutsideTempValid ? abs(readings.outsideTemp % 10) : 9);

        LOG_DEBUG(TAG, "Env I:%d.%d",
                 readings.isInsideTempValid ? readings.insideTemp / 10 : -99,
                 readings.isInsideTempValid ? abs(readings.insideTemp % 10) : 9);

        // Optional sensors (enable via ENABLE_SENSOR_* flags)
#ifdef ENABLE_SENSOR_WATER_TANK_TOP
        LOG_DEBUG(TAG, "Opt WTT:%d.%d",
                 readings.isWaterTankTopTempValid ? readings.waterTankTopTemp / 10 : -99,
                 readings.isWaterTankTopTempValid ? abs(readings.waterTankTopTemp % 10) : 9);
#endif
#ifdef ENABLE_SENSOR_WATER_RETURN
        LOG_DEBUG(TAG, "Opt WR:%d.%d",
                 readings.isWaterHeaterTempReturnValid ? readings.waterHeaterTempReturn / 10 : -99,
                 readings.isWaterHeaterTempReturnValid ? abs(readings.waterHeaterTempReturn % 10) : 9);
#endif
#ifdef ENABLE_SENSOR_HEATING_RETURN
        LOG_DEBUG(TAG, "Opt HR:%d.%d",
                 readings.isHeatingTempReturnValid ? readings.heatingTempReturn / 10 : -99,
                 readings.isHeatingTempReturnValid ? abs(readings.heatingTempReturn % 10) : 9);
#endif

        SRP::giveSensorReadingsMutex();
    }
}

static void logRelayStatus() {
    // Rate limit: only log every 10th call (synced with sensor status)
    static uint8_t logCounter = 0;
    if (++logCounter < 10) {
        return;
    }
    logCounter = 0;

    // Get relay states safely
    if (SRP::takeRelayReadingsMutex(pdMS_TO_TICKS(100))) {
        [[maybe_unused]] SharedRelayReadings relays = SRP::getRelayReadings();

        // Compact relay status - single line with all states
        LOG_DEBUG(TAG, "Rly HP:%d WP:%d B:%d HP:%d WM:%d V:%d S:%d",
                 relays.relayHeatingPump,
                 relays.relayWaterPump,
                 relays.relayBurnerEnable,
                 relays.relayPowerBoost,
                 relays.relayWaterMode,
                 relays.relayValve,
                 relays.relaySpare);

        SRP::giveRelayReadingsMutex();
    }
}

static void dumpErrorLog(size_t maxErrors) {
    LOG_INFO(TAG, "=== ERROR LOG DUMP ===");
    
    // Get error statistics first
    ErrorLogFRAM::ErrorStats stats = ErrorLogFRAM::getStats();
    LOG_INFO(TAG, "Error Stats: Total=%lu, Critical=%lu, Unique=%u", 
             stats.totalErrors, stats.criticalErrors, stats.uniqueErrors);
    
    if (stats.lastErrorTime > 0) {
        // Convert timestamp to readable format
        uint32_t timeSinceLast = (millis() / 1000) - stats.lastErrorTime;
        uint32_t hours = timeSinceLast / 3600;
        uint32_t minutes = (timeSinceLast % 3600) / 60;
        uint32_t seconds = timeSinceLast % 60;
        LOG_INFO(TAG, "Last error: %02lu:%02lu:%02lu ago", hours, minutes, seconds);
    }
    
    // Get recent errors
    ErrorLogFRAM::ErrorEntry entry;
    size_t errorCount = ErrorLogFRAM::getErrorCount();
    size_t displayCount = std::min(errorCount, maxErrors);
    
    LOG_INFO(TAG, "Recent Errors (showing %zu of %zu):", displayCount, errorCount);
    
    for (size_t i = 0; i < displayCount; i++) {
        if (ErrorLogFRAM::getError(i, entry)) {
            // Convert error code to string
            const char* errorStr = ErrorHandler::errorToString(static_cast<SystemError>(entry.errorCode));
            
            // Calculate time ago
            uint32_t timeAgo = (millis() / 1000) - entry.timestamp;
            uint32_t minutesAgo = timeAgo / 60;
            uint32_t secondsAgo = timeAgo % 60;
            
            // Log the error entry
            if (entry.count > 1) {
                LOG_INFO(TAG, "[%zu] %s (code: %lu) x%u - %02lu:%02lu ago",
                         i, errorStr, entry.errorCode, entry.count, minutesAgo, secondsAgo);
            } else {
                LOG_INFO(TAG, "[%zu] %s (code: %lu) - %02lu:%02lu ago",
                         i, errorStr, entry.errorCode, minutesAgo, secondsAgo);
            }
            
            // Log message and context if available
            if (strlen(entry.message) > 0) {
                LOG_INFO(TAG, "    Msg: %s", entry.message);
            }
            if (strlen(entry.context) > 0) {
                LOG_INFO(TAG, "    Ctx: %s", entry.context);
            }
        }
    }
    
    // Get critical errors specifically
    LOG_INFO(TAG, "Critical Errors:");
    ErrorLogFRAM::ErrorEntry criticalErrors[5];
    size_t criticalCount = ErrorLogFRAM::getCriticalErrors(criticalErrors, 5);
    
    for (size_t i = 0; i < criticalCount; i++) {
        const char* errorStr = ErrorHandler::errorToString(static_cast<SystemError>(criticalErrors[i].errorCode));
        LOG_INFO(TAG, "  [CRIT] %s (code: %lu) - %s",
                 errorStr, criticalErrors[i].errorCode, 
                 criticalErrors[i].context[0] ? criticalErrors[i].context : "No context");
    }
    
    LOG_INFO(TAG, "=== END ERROR LOG ===");
}

static void logCompactStatus() {
    // Calculate uptime
    [[maybe_unused]] uint32_t uptimeMs = millis();
    [[maybe_unused]] uint32_t days = uptimeMs / 86400000;
    [[maybe_unused]] uint32_t hours = (uptimeMs % 86400000) / 3600000;
    [[maybe_unused]] uint32_t minutes = (uptimeMs % 3600000) / 60000;
    [[maybe_unused]] uint32_t seconds = (uptimeMs % 60000) / 1000;

    LOG_DEBUG(TAG, "Up %dd %02d:%02d:%02d", days, hours, minutes, seconds);

    // Get task count
    [[maybe_unused]] UBaseType_t taskCount = uxTaskGetNumberOfTasks();
    LOG_DEBUG(TAG, "Tasks: %d", taskCount);
}

static void logAllTasks() {
    // Get number of tasks
    UBaseType_t taskCount = uxTaskGetNumberOfTasks();
    LOG_DEBUG(TAG, "=== TASKS (%u) ===", taskCount);
    
    // Allocate array for task status
    TaskStatus_t* taskStatusArray = (TaskStatus_t*)pvPortMalloc(taskCount * sizeof(TaskStatus_t));
    if (taskStatusArray == nullptr) {
        LOG_ERROR(TAG, "Alloc fail");
        return;
    }
    
    // Get task status
    uint32_t totalRunTime = 0;
    taskCount = uxTaskGetSystemState(taskStatusArray, taskCount, &totalRunTime);
    
    // Count issues
    int lowStackCount = 0;
    int blockedCount = 0;
    int suspendedCount = 0;
    
    // Log header
    LOG_DEBUG(TAG, "Name          St Pri Stack Core");
    
    // Display each task (no sorting - keep creation order)
    // Yield periodically to prevent starving other tasks during this long iteration
    for (UBaseType_t i = 0; i < taskCount; i++) {
        [[maybe_unused]] const char* stateStr;
        switch (taskStatusArray[i].eCurrentState) {
            case eRunning: stateStr = "RUN"; break;
            case eReady: stateStr = "RDY"; break;
            case eBlocked:
                stateStr = "BLK";
                blockedCount++;
                break;
            case eSuspended:
                stateStr = "SUS";
                suspendedCount++;
                break;
            case eDeleted: stateStr = "DEL"; break;
            default: stateStr = "?"; break;
        }

        // Get stack free in bytes
        size_t stackFreeBytes = taskStatusArray[i].usStackHighWaterMark * sizeof(StackType_t);

        // Get core ID
        int coreId = xTaskGetCoreID(taskStatusArray[i].xHandle);

        // Format core ID
        char coreStr[16];  // Increased size to handle any int value safely
        if (coreId == tskNO_AFFINITY) {
            snprintf(coreStr, sizeof(coreStr), "%s", "ANY");
        } else {
            snprintf(coreStr, sizeof(coreStr), "%d", coreId);
        }

        // Check for low stack
        [[maybe_unused]] const char* warning = "";
        if (stackFreeBytes < 512) {
            warning = " !LOW!";
            lowStackCount++;
        }

        // Truncate task name to 12 chars
        char shortName[13];
        strncpy(shortName, taskStatusArray[i].pcTaskName, 12);
        shortName[12] = '\0';

        LOG_DEBUG(TAG, "%-12s %3s %2d %5u %3s%s",
                 shortName,
                 stateStr,
                 taskStatusArray[i].uxCurrentPriority,
                 stackFreeBytes,
                 coreStr,
                 warning);

        // Yield every 4 tasks to let other tasks run (prevents starving lower priority tasks)
        if ((i + 1) % 4 == 0) {
            taskYIELD();
        }
    }
    
    LOG_DEBUG(TAG, "=== END ===");
    
    // Log issues summary
    LOG_DEBUG(TAG, "Issues: L%d B%d S%d", 
             lowStackCount, blockedCount, suspendedCount);
    
    // Free memory
    vPortFree(taskStatusArray);
}
