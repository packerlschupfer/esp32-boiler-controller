// src/modules/tasks/MonitoringTaskEventDriven.cpp
// Event-driven version of the monitoring task

#include "MonitoringTask.h"
#include "modules/tasks/EventDrivenPatterns.h"
#include "config/SystemConstants.h"
#include <esp_heap_caps.h>
#include "config/ProjectConfig.h"
#include <atomic>
#include "LoggingMacros.h"

#include <Watchdog.h>
#include <freertos/timers.h>
#include <TaskManager.h>
#include "core/SystemResourceProvider.h"
#include "events/SystemEventsGenerated.h"
#include <EthernetManager.h>
#include <esp_log.h>
#include "monitoring/HealthMonitor.h"  // wired up here; see the init below
#include "shared/SharedSensorReadings.h"
#include "shared/SharedRelayReadings.h"
#include "shared/Temperature.h"
#include "config/SystemSettings.h"
#include "utils/ErrorLogFRAM.h"
#include "utils/ErrorHandler.h"
#include "utils/OtaRollbackGuard.h"
#include "MQTTTask.h"
#include "MQTTTopics.h"
#include <ModbusErrorTracker.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <cstring>

static const char* TAG = "Monitoring";

// Timer handles for different monitoring intervals
// How often the stack scan runs, expressed in health checks (5 s each) - so 12 = 60 s.
// Deliberately not a new timer: the health check already wakes this task on a cadence that is
// both frequent enough to be useful and slow enough that a ~1.3 kB transient allocation and a
// 32-task enumeration are free in practice.
static constexpr uint32_t STACK_SCAN_EVERY_N_CHECKS = 12;

// Repeat interval for the persistent LOW STACK warning, so raising the scan rate 30x does not
// raise the log rate 30x. Matches the old detailed-report cadence.
static constexpr uint32_t LOW_STACK_WARN_REPEAT_MS = 1800000;  // 30 min

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
static void scanTaskStacks(bool verbose);
static void publishModbusErrorStats();
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

    LOG_INFO(TAG, "Timers started - Health: %lums, Detailed: %lums",
             HEALTH_CHECK_INTERVAL_MS, DETAILED_MONITOR_INTERVAL_MS);

    // Mark event aggregator as ready - timer callbacks can now safely access it
    monitoringEventsReady = true;

    // Wire up HealthMonitor. It had never been instantiated: gHealthMonitor stayed
    // nullptr, so SRP::getHealthMonitor() returned null to every caller and the whole
    // class - including every HEALTH_RECORD_ERROR site across the firmware - was a
    // silent no-op. Assign the global only AFTER initialize() succeeds, because
    // initialize() runs a metric pass of its own and a half-built instance must never
    // be reachable through getHealthMonitor().
    {
        HealthMonitor& hm = HealthMonitor::getInstance();
        const auto hmInit = hm.initialize();
        if (hmInit.isSuccess()) {
            gHealthMonitor = &hm;
            LOG_INFO(TAG, "HealthMonitor initialized and published");
        } else {
            LOG_ERROR(TAG, "HealthMonitor init failed - health tracking stays disabled");
        }
    }

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
            LOG_DEBUG(TAG, "Loop %lu @ %lu ms", loopCount, millis());
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

        // Confirm or roll back an OTA-updated image (no-op after serial flash)
        OtaRollbackGuard::check();

        if (events == 0) {
            // Timeout - just feed watchdog and continue
            continue;
        }

        // Handle health check event
        if (events & MONITOR_EVENT_HEALTH_CHECK) {
            // Drive HealthMonitor's metric pass. Nothing called this before, which is
            // why the DRAM gate in updateMemoryMetrics() never ran.
            if (HealthMonitor* hm = SRP::getHealthMonitor()) {
                hm->updateMetrics();
            }

            // Quick health check - only critical metrics
            // MALLOC_CAP_DEFAULT: ESP.getFreeHeap() is MALLOC_CAP_INTERNAL (counts IRAM,
            // ~58 kB here), so the old comparison against MIN_FREE_HEAP_WARNING * 2 could
            // never be true and this 5-second check never fired (2026-10-18 review).
            const size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
            const size_t freeDram = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);

            // Early notice at 2x the warning block size, as the old code intended.
            constexpr size_t EARLY_WARNING = SystemConstants::System::MIN_DRAM_BLOCK_WARNING * 2;
            if (largestBlock < EARLY_WARNING) {
                LOG_WARN(TAG, "Low DRAM! free %u B, largest block %u B",
                         static_cast<unsigned>(freeDram), static_cast<unsigned>(largestBlock));
            }
            
            // Task stack minimum for boiler/status/health, plus the low-stack warning.
            // First health check after boot scans immediately so the published figure is real
            // within ~5 s; after that every STACK_SCAN_EVERY_N_CHECKS.
            static uint32_t stackScanCounter = 0;
            if (stackScanCounter++ % STACK_SCAN_EVERY_N_CHECKS == 0) {
                scanTaskStacks(false);
                (void)SRP::getTaskManager().feedWatchdog();
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
            // This is important because scanTaskStacks() iterates through all 32 tasks
            logCompactStatus();
            taskYIELD();

            scanTaskStacks(true);  // verbose: the full per-task table
            (void)SRP::getTaskManager().feedWatchdog();  // Feed after heavy task iteration
            taskYIELD();

            logNetworkStatus();
            logSensorStatus();
            logRelayStatus();

            // Publish Modbus error statistics (IMPROVEMENT 5)
            // NOTE: Will publish zeros until device libraries integrate tracking calls
            publishModbusErrorStats();

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

    LOG_DEBUG(TAG, "Up %lud %02lu:%02lu:%02lu", days, hours, minutes, seconds);

    // Get task count
    [[maybe_unused]] UBaseType_t taskCount = uxTaskGetNumberOfTasks();
    LOG_DEBUG(TAG, "Tasks: %d", taskCount);
}

// Minimum stack free across the tasks WE control, from the last scanTaskStacks() pass, for
// boiler/status/health. 0 means "not measured yet", which now only covers the first ~5 s after
// boot: the scan is driven from the 5-second health check (see STACK_SCAN_EVERY_N_CHECKS),
// not from the 30-minute detailed report. It was driven from the detailed report for exactly
// one firmware (68c45fb), where the field read 0 for the first half hour of every boot - so a
// device in a reboot loop, the case you most want the number for, would never publish it.
//
// This exists because the health payload used to publish uxTaskGetStackHighWaterMark(NULL),
// which is the MQTT TASK'S OWN headroom under the name `stack_hwm` - a figure that reads like a
// system minimum and is not one. Noticed 2026-10-26 only because the published 1604 happened to
// match MQTTTask exactly in a per-task dump. Same family as heap_free being MALLOC_CAP_INTERNAL.
static std::atomic<uint16_t> s_minStackFree{0};

// DOUBLE-BUFFERED, with an atomic index naming the stable one. The publisher used to read
// this array directly while MonitoringTask memcpy'd into it from the other core, and the
// comment there conceded a torn name was possible and called it acceptable. It costs 13 bytes
// to not have that: the scan writes the slot the readers are NOT using, then publishes the
// index with release ordering, so a reader always sees one complete name. It can still be one
// pass old relative to the number, which is harmless and now visible via stack_min_age.
static char s_minStackTask[2][13] = {{0}, {0}};
static std::atomic<uint8_t> s_minStackSlot{0};

// Consecutive scans that could not run (gate refused or malloc failed). Published so a frozen
// stack_min is distinguishable from a fresh one.
static std::atomic<uint16_t> s_scanFailures{0};

uint16_t MonitoringTask_getMinTaskStackFree() {
    return s_minStackFree.load(std::memory_order_acquire);
}

const char* MonitoringTask_getMinTaskStackName() {
    return s_minStackTask[s_minStackSlot.load(std::memory_order_acquire)];
}

uint16_t MonitoringTask_getStackScanFailures() {
    return s_scanFailures.load(std::memory_order_relaxed);
}

/**
 * @brief Enumerate every live task, publish the stack minimum and warn on low stacks.
 *
 * @param verbose also print the per-task table (LOG_DEBUG, detailed report only)
 *
 * uxTaskGetSystemState() is the only SAFE way to enumerate here. The TaskManager registry
 * would need no allocation, but a dozen task bodies bail out with vTaskDelete(NULL) on an init
 * failure and the registry keeps the stale handle - reading a freed TCB to print a diagnostic
 * is not a trade worth making. The transient ~1.3 kB allocation is the price of that safety;
 * a static array of the same size would cost it permanently, and DRAM is the scarce resource
 * on this board, not cycles.
 */
static void scanTaskStacks(bool verbose) {
    // Get number of tasks
    UBaseType_t taskCount = uxTaskGetNumberOfTasks();
    if (verbose) {
        LOG_DEBUG(TAG, "=== TASKS (%u) ===", taskCount);
    }
    
    // GATE THE ALLOCATION, same rule as the on-demand tasks dump
    // (MQTTCommandHandlers.cpp): need the array PLUS a critical-block margin left over, so a
    // diagnostic cannot be the allocation that pushes a control path over. That dump has
    // gated this since 2026-10-16 because ordinary HA polling was once measured at 576 B
    // free; this copy was ungated, and ff98106 made it 30x more frequent (60 s instead of
    // 30 min) on a period that coincides with the 60 s health publish, which allocates too.
    const size_t needed = taskCount * sizeof(TaskStatus_t);
    const size_t required = needed + SystemConstants::System::MIN_DRAM_BLOCK_CRITICAL;
    const size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    if (largestBlock < required) {
        s_scanFailures.fetch_add(1, std::memory_order_relaxed);
        LOG_WARN(TAG, "Stack scan skipped - largest DRAM block %u B, need %u B",
                 static_cast<unsigned>(largestBlock), static_cast<unsigned>(required));
        return;
    }

    // Allocate array for task status
    TaskStatus_t* taskStatusArray = (TaskStatus_t*)pvPortMalloc(needed);
    if (taskStatusArray == nullptr) {
        // Leaves s_minStackFree at the PREVIOUS pass's value rather than at 0. That is the
        // right choice - a stale real number beats a fake zero - but it was previously
        // indistinguishable from a fresh measurement on the health topic, so the failure is
        // now counted and published as stack_min_age.
        s_scanFailures.fetch_add(1, std::memory_order_relaxed);
        LOG_ERROR(TAG, "Stack scan alloc fail (%u B)", static_cast<unsigned>(needed));
        return;
    }
    
    // Get task status
    uint32_t totalRunTime = 0;
    taskCount = uxTaskGetSystemState(taskStatusArray, taskCount, &totalRunTime);
    
    // Count issues
    int lowStackCount = 0;
    int blockedCount = 0;
    int suspendedCount = 0;

    // Worst offender, so the warning below can name it. Until 2026-09-18 lowStackCount
    // fed nothing but a LOG_DEBUG, and LOG_DEBUG is ((void)0) in LOG_MODE_RELEASE - so
    // the entire low-stack monitor was inert in release and merely buried in debug. Both
    // pump tasks sat under this threshold for months (288 B and 420 B free) printing
    // " !LOW!" into a debug stream nobody reads.
    size_t worstStackFree = SIZE_MAX;
    char worstStackTask[13] = {0};

    // The real budget figure, published on boiler/status/health as stack_min / stack_min_task.
    size_t minStackFreeThisPass = SIZE_MAX;
    char minStackTaskThisPass[13] = {0};
    
    // Log header
    if (verbose) {
        LOG_DEBUG(TAG, "Name          St Pri Stack Core");
    }
    
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

        // Core ID, formatted only when it is going to be printed - two snprintf calls per
        // task on a 60-second cadence are pointless when nothing reads the result.
        char coreStr[16];  // Increased size to handle any int value safely
        coreStr[0] = '\0';
        if (verbose) {
            int coreId = xTaskGetCoreID(taskStatusArray[i].xHandle);
            if (coreId == tskNO_AFFINITY) {
                snprintf(coreStr, sizeof(coreStr), "%s", "ANY");
            } else {
                snprintf(coreStr, sizeof(coreStr), "%d", coreId);
            }
        }

        // Truncate task name to 12 chars
        char shortName[13];
        strncpy(shortName, taskStatusArray[i].pcTaskName, 12);
        shortName[12] = '\0';

        const bool frameworkOwned =
            SystemConstants::System::isFrameworkOwnedStack(taskStatusArray[i].pcTaskName);

        // The TRUE minimum across the tasks we control, tracked unconditionally. Separate from
        // worstStackFree below, which only looks at tasks ALREADY under the warn threshold and
        // is therefore SIZE_MAX whenever nothing is in trouble - fine for naming an offender,
        // useless as a budget figure. Framework-owned stacks (IDLE, ipc) are excluded because
        // their sizes are not ours to set and IDLE's ~456 B free is normal; including them
        // would permanently mask the tightest task of ours.
        if (!frameworkOwned && stackFreeBytes < minStackFreeThisPass) {
            minStackFreeThisPass = stackFreeBytes;
            memcpy(minStackTaskThisPass, shortName, sizeof(minStackTaskThisPass));
        }

        // Check for low stack
        [[maybe_unused]] const char* warning = "";
        if (stackFreeBytes < SystemConstants::System::LOW_STACK_WARN_BYTES && !frameworkOwned) {
            warning = " !LOW!";
            lowStackCount++;
            if (stackFreeBytes < worstStackFree) {
                worstStackFree = stackFreeBytes;
                memcpy(worstStackTask, shortName, sizeof(worstStackTask));
            }
        }

        if (verbose) {
            LOG_DEBUG(TAG, "%-12s %3s %2d %5u %3s%s",
                     shortName,
                     stateStr,
                     taskStatusArray[i].uxCurrentPriority,
                     stackFreeBytes,
                     coreStr,
                     warning);

            // Yield every 4 tasks to let other tasks run (prevents starving lower priority
            // tasks). Only needed on the verbose path: the logging is what makes the loop long.
            if ((i + 1) % 4 == 0) {
                taskYIELD();
            }
        }
    }

    if (verbose) {
        LOG_DEBUG(TAG, "=== END ===");

        // Log issues summary
        LOG_DEBUG(TAG, "Issues: L%d B%d S%d",
                 lowStackCount, blockedCount, suspendedCount);
    }

    // A task within LOW_STACK_WARN_BYTES of overflow is a real hazard on this device -
    // an overflow in a pump or relay task leaves actuators in an arbitrary state. Warn at
    // a level that survives every build mode, and name the offender and its margin so the
    // report is actionable without a debug build.
    //
    // The condition is persistent, not an edge, so this is deliberately re-warned rather than
    // latched - staying quiet about it is what let the pump tasks run at 288 B for months. But
    // the scan now runs every 60 s instead of every 30 min, so repeat the warning on the old
    // cadence: a permanently low task would otherwise emit 1440 identical WARN lines a day.
    if (lowStackCount > 0) {
        static uint32_t lastLowStackWarnMs = 0;
        const uint32_t nowMs = millis();
        if (verbose || lastLowStackWarnMs == 0 ||
            (nowMs - lastLowStackWarnMs) >= LOW_STACK_WARN_REPEAT_MS) {
            lastLowStackWarnMs = nowMs;
            LOG_WARN(TAG, "LOW STACK: %d task(s) below %u B; worst %s at %u B free",
                     lowStackCount, (unsigned)SystemConstants::System::LOW_STACK_WARN_BYTES,
                     worstStackTask, (unsigned)worstStackFree);
        }
    }

    // Free memory
    // Publish the pass result for boiler/status/health. Name first, then the number with
    // release ordering; the reader loads the number then copies the name. A concurrent read can
    // in principle see a name from one pass with a number from the next - the buffer is fixed
    // size and always NUL-terminated at [12], so the worst case is a garbled diagnostic string,
    // never a bad access. Said plainly rather than pretending this is synchronised: a mutex for
    // a 13-byte diagnostic read from one task would cost more than it buys.
    if (minStackFreeThisPass != SIZE_MAX) {
        // Write the slot nobody is reading, then flip the index. A reader that loads the index
        // before the flip gets the previous complete name; after it, the new complete name.
        const uint8_t next = s_minStackSlot.load(std::memory_order_relaxed) ^ 1u;
        memcpy(s_minStackTask[next], minStackTaskThisPass, sizeof(s_minStackTask[next]));
        s_minStackTask[next][sizeof(s_minStackTask[next]) - 1] = '\0';
        s_minStackFree.store(static_cast<uint16_t>(
            minStackFreeThisPass > 0xFFFF ? 0xFFFF : minStackFreeThisPass),
            std::memory_order_release);
        s_minStackSlot.store(next, std::memory_order_release);
        s_scanFailures.store(0, std::memory_order_relaxed);  // a pass completed
    }

    vPortFree(taskStatusArray);
}

/**
 * @brief Publish Modbus error statistics to MQTT
 * Queries ModbusErrorTracker from ESP32-ModbusDevice library and publishes stats for all devices
 */
static void publishModbusErrorStats() {
    // Device addresses (from ProjectConfig.h)
    const uint8_t devices[] = {MB8ART_ADDRESS, RYN4_ADDRESS, ANDRTF3_ADDRESS};

    for (uint8_t address : devices) {
        // Query statistics from library (modbus namespace)
        uint32_t crcErrors = modbus::ModbusErrorTracker::getCrcErrors(address);
        uint32_t timeouts = modbus::ModbusErrorTracker::getTimeouts(address);
        uint32_t invalidData = modbus::ModbusErrorTracker::getInvalidDataErrors(address);
        uint32_t deviceErrors = modbus::ModbusErrorTracker::getDeviceErrors(address);
        uint32_t otherErrors = modbus::ModbusErrorTracker::getOtherErrors(address);
        uint32_t successCount = modbus::ModbusErrorTracker::getSuccessCount(address);
        uint32_t totalErrors = modbus::ModbusErrorTracker::getTotalErrors(address);
        float errorRate = modbus::ModbusErrorTracker::getErrorRate(address);
        uint32_t lastErrorTime = modbus::ModbusErrorTracker::getLastErrorTime(address);

        // Build JSON payload
        JsonDocument doc;
        doc["address"] = address;
        doc["crc_errors"] = crcErrors;
        doc["timeouts"] = timeouts;
        doc["invalid_data"] = invalidData;
        doc["device_errors"] = deviceErrors;
        doc["other_errors"] = otherErrors;
        doc["success_count"] = successCount;
        doc["total_errors"] = totalErrors;
        doc["error_rate_pct"] = errorRate;

        // Time since last error (in milliseconds)
        if (lastErrorTime > 0) {
            uint32_t timeSinceError = millis() - lastErrorTime;
            doc["last_error_ms_ago"] = timeSinceError;
        }

        // Build topic: boiler/diagnostics/modbus/{address}
        char topic[64];
        snprintf(topic, sizeof(topic), "%s/%02X", MQTT_DIAGNOSTICS_MODBUS_PREFIX, address);

        // Serialize and publish
        char payload[256];
        serializeJson(doc, payload, sizeof(payload));
        MQTTTask::publish(topic, payload, 0, false, MQTTPriority::PRIORITY_LOW);

        LOG_DEBUG(TAG, "Modbus stats 0x%02X: err=%lu (%.1f%%), ok=%lu",
                 address, totalErrors, errorRate, successCount);
    }
}
