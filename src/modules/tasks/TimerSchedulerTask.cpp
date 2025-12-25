// src/modules/tasks/TimerSchedulerTask.cpp
// Generic timer scheduler task implementation

#include "modules/tasks/TimerSchedulerTask.h"
#include "TimerSchedule.h"
#include "IScheduleAction.h"
#include "modules/scheduler/WaterHeatingScheduleAction.h"
#include "modules/scheduler/SpaceHeatingScheduleAction.h"
#include "DS3231Controller.h"
#include "LoggingMacros.h"
#include "core/SystemResourceProvider.h"
#include "config/SystemConstants.h"
#include "modules/tasks/MQTTTask.h"
#include "MQTTTopics.h"
#include <TaskManager.h>
#include <ArduinoJson.h>
#include <vector>
#include <map>
#include <memory>
#include <atomic>
#include "freertos/timers.h"
#include "RuntimeStorageSchedules.h"
#include "modules/scheduler/SchedulerResponseFormatter.h"
#include "modules/scheduler/SchedulerContext.h"
#include "utils/MQTTValidator.h"
#include "modules/tasks/NTPTask.h"  // For NTP RTC callback
#include "utils/ResourceGuard.h"  // For TaskCleanupHandler

static const char* TAG = "TimerScheduler";

// Static variables
static DS3231Controller* rtcController = nullptr;
static std::vector<TimerSchedule> timerSchedules;  // Renamed from 'schedules' to avoid namespace conflict
static std::map<ScheduleType, std::unique_ptr<IScheduleAction>> actionHandlers;
static std::map<uint8_t, bool> activeSchedules;  // Schedule ID -> active state
static EventGroupHandle_t schedulerEventGroup = nullptr;
static TimerHandle_t scheduleCheckTimer = nullptr;
static std::atomic<bool> isInitialized{false};

// Round 14 Issue #5: Mutex for thread-safe schedule access
// Protects timerSchedules and activeSchedules from concurrent modification
// by task loop (checkSchedules) and MQTT handler (processMQTTCommand)
static SemaphoreHandle_t schedulesMutex = nullptr;

// RuntimeStorage instance for schedule persistence
static rtstorage::RuntimeStorage* runtimeStorage = nullptr;
static schedules::ScheduleStorage* scheduleStorage = nullptr;

// Event bits - use centralized definitions from SchedulerContext
using SchedulerEvents = SchedulerContext;

// Configuration - use centralized constants
namespace SchedulerConstants = SystemConstants::Tasks::Scheduler;

// NTP RTC callback to update DS3231 when NTP syncs
static void updateRTCFromNTP(time_t utcTime) {
    if (!rtcController) {
        LOG_WARN(TAG, "RTC controller not available for NTP update");
        return;
    }
    
    // Validate the UTC time
    if (utcTime < 1577836800) { // Jan 1, 2020
        LOG_ERROR(TAG, "Invalid UTC time received in RTC callback: %ld", (long)utcTime);
        return;
    }
    
    // Calculate timezone offset for CET/CEST
    struct tm localTm, utcTm;
    localtime_r(&utcTime, &localTm);
    gmtime_r(&utcTime, &utcTm);
    
    // Calculate hour difference
    int hourDiff = localTm.tm_hour - utcTm.tm_hour;
    
    // Handle day boundary
    if (localTm.tm_mday != utcTm.tm_mday) {
        if (localTm.tm_mday > utcTm.tm_mday) {
            hourDiff += 24;  // Local time is next day
        } else {
            hourDiff -= 24;  // Local time is previous day
        }
    }
    
    int32_t tzOffset = hourDiff * 3600;  // Convert to seconds
    
    // Use explicit casting to avoid format string issues
    LOG_INFO(TAG, "Updating RTC from NTP: UTC epoch=%ld, offset=%d (%+d hours)", 
             (long)utcTime, (int)tzOffset, (int)(tzOffset / 3600));
    
    // Update RTC with timezone-aware method
    if (rtcController->setTimeFromUTC(utcTime, tzOffset)) {
        // Verify the update
        DateTime now = rtcController->now();
        LOG_INFO(TAG, "RTC updated successfully: %04d-%02d-%02d %02d:%02d:%02d",
                 now.year(), now.month(), now.day(),
                 now.hour(), now.minute(), now.second());
    } else {
        LOG_ERROR(TAG, "Failed to update RTC from NTP");
    }
}
static uint32_t lastPersistTime = 0;
static bool schedulesModified = false;

// Forward declarations
static void checkSchedules();
static void checkSchedule(TimerSchedule& schedule);
static void activateSchedule(TimerSchedule& schedule);
static void deactivateSchedule(TimerSchedule& schedule);
static void cleanupScheduler();
static IScheduleAction* getActionHandler(ScheduleType type);
static uint8_t getNextFreeId();
static void publishSchedulerStatus();
static void saveSchedules();
static void loadSchedules();

// Timer callback
static void scheduleCheckTimerCallback(TimerHandle_t xTimer) {
    if (schedulerEventGroup) {
        xEventGroupSetBits(schedulerEventGroup, SchedulerEvents::SCHEDULER_EVENT_CHECK_SCHEDULE);
    }
}

// Initialize the scheduler
static bool initializeScheduler() {
    LOG_INFO(TAG, "Initializing timer scheduler");

    // Round 14 Issue #5: Create mutex for schedule access
    if (schedulesMutex == nullptr) {
        schedulesMutex = xSemaphoreCreateMutex();
        if (!schedulesMutex) {
            LOG_ERROR(TAG, "Failed to create schedules mutex");
            return false;
        }
    }

    // Create event group
    schedulerEventGroup = xEventGroupCreate();
    if (!schedulerEventGroup) {
        LOG_ERROR(TAG, "Failed to create event group");
        return false;
    }

    // Get services from SRP (ServiceContainer removed)

    // Get RTC controller
    rtcController = SRP::getDS3231();
    if (!rtcController) {
        LOG_ERROR(TAG, "Failed to get RTC controller");
        return false;
    }

    // Register NTP callback to update RTC when time syncs
    setNTPRTCCallback(updateRTCFromNTP);
    LOG_INFO(TAG, "Registered NTP RTC update callback");

    // Get RuntimeStorage (initialized early in SystemInitializer::initializeHardware)
    runtimeStorage = SRP::getRuntimeStorage();
    if (!runtimeStorage) {
        LOG_ERROR(TAG, "RuntimeStorage not available - scheduler disabled");
        return false;
    }
    
    // Create schedule storage handler
    scheduleStorage = new schedules::ScheduleStorage(*runtimeStorage);
    if (!scheduleStorage) {
        LOG_ERROR(TAG, "Failed to create schedule storage handler");
        return false;
    }
    
    // Initialize schedule storage area if needed
    if (!scheduleStorage->initializeScheduleStorage()) {
        LOG_ERROR(TAG, "Failed to initialize schedule storage area");
        delete scheduleStorage;
        scheduleStorage = nullptr;
        return false;
    }
    
    // Register default action handlers
    actionHandlers[ScheduleType::WATER_HEATING] = std::make_unique<WaterHeatingScheduleAction>();
    LOG_INFO(TAG, "Registered water heating action handler");
    
    actionHandlers[ScheduleType::SPACE_HEATING] = std::make_unique<SpaceHeatingScheduleAction>();
    LOG_INFO(TAG, "Registered space heating action handler");
    
    // Load persisted schedules
    loadSchedules();
    
    // Create schedule check timer
    scheduleCheckTimer = xTimerCreate(
        "SchedCheck",
        pdMS_TO_TICKS(SchedulerConstants::CHECK_INTERVAL_MS),
        pdTRUE,  // Auto-reload
        nullptr,
        scheduleCheckTimerCallback
    );
    
    if (scheduleCheckTimer) {
        xTimerStart(scheduleCheckTimer, pdMS_TO_TICKS(100));
    }
    
    isInitialized = true;
    LOG_INFO(TAG, "Timer scheduler initialized with %d schedules", timerSchedules.size());
    return true;
}

// Check all schedules
static void checkSchedules() {
    // THREAD-SAFE: checkCount is only accessed from task loop (single task)
    static uint32_t checkCount = 0;
    checkCount++;

    // S1: Validate RTC time before schedule evaluation
    // If NTP failed and RTC battery died, time is invalid
    if (rtcController) {
        DateTime now = rtcController->now();
        if (now.year() < 2020) {
            static uint32_t lastRtcWarn = 0;
            if (millis() - lastRtcWarn > 60000) {  // Warn once per minute
                LOG_ERROR(TAG, "RTC time invalid (year=%d) - schedules suspended", now.year());
                lastRtcWarn = millis();
            }
            return;  // Skip all schedule checks
        }
    }

    // Round 14 Issue #5: Acquire mutex before accessing schedules
    if (schedulesMutex == nullptr || xSemaphoreTake(schedulesMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        LOG_WARN(TAG, "Failed to acquire schedules mutex for check");
        return;
    }

    if (checkCount % 10 == 0) {  // Log every 10th check (5 minutes)
        LOG_DEBUG(TAG, "Checking %d schedules (check #%lu)", timerSchedules.size(), checkCount);
    }

    for (auto& schedule : timerSchedules) {
        checkSchedule(schedule);
    }

    xSemaphoreGive(schedulesMutex);
}

// Check a single schedule
static void checkSchedule(TimerSchedule& schedule) {
    if (!schedule.enabled) {
        // If disabled but active, deactivate
        if (activeSchedules[schedule.id]) {
            deactivateSchedule(schedule);
        }
        return;
    }
    
    DateTime now = rtcController->now();
    bool shouldBeActive = schedule.isActiveNow(now.hour(), now.minute(), now.dayOfTheWeek());
    bool isActive = activeSchedules[schedule.id];
    
    if (shouldBeActive && !isActive) {
        activateSchedule(schedule);
    } else if (!shouldBeActive && isActive) {
        deactivateSchedule(schedule);
    }
}

// Activate a schedule
static void activateSchedule(TimerSchedule& schedule) {
    IScheduleAction* handler = getActionHandler(schedule.type);
    if (!handler) {
        LOG_ERROR(TAG, "No handler for schedule type %d", (int)schedule.type);
        return;
    }
    
    LOG_INFO(TAG, "Activating schedule %d '%s'", schedule.id, schedule.name.c_str());
    handler->onScheduleStart(schedule);
    activeSchedules[schedule.id] = true;
    
    // Publish MQTT event
    JsonDocument doc;  // ArduinoJson v7
    doc["event"] = "schedule_start";
    doc["id"] = schedule.id;
    doc["name"] = schedule.name;
    doc["type"] = handler->getTypeName();

    char payload[192];  // Sufficient for schedule event JSON
    serializeJson(doc, payload, sizeof(payload));
    MQTTTask::publish(MQTT_TOPIC_SCHEDULER_EVENT, payload);
}

// Deactivate a schedule
static void deactivateSchedule(TimerSchedule& schedule) {
    IScheduleAction* handler = getActionHandler(schedule.type);
    if (!handler) return;

    LOG_INFO(TAG, "Deactivating schedule %d '%s'", schedule.id, schedule.name.c_str());
    handler->onScheduleEnd(schedule);
    activeSchedules[schedule.id] = false;

    // Publish MQTT event
    JsonDocument doc;  // ArduinoJson v7
    doc["event"] = "schedule_end";
    doc["id"] = schedule.id;
    doc["name"] = schedule.name;
    doc["type"] = handler->getTypeName();
    
    char payload[192];  // Sufficient for schedule event JSON
    serializeJson(doc, payload, sizeof(payload));
    MQTTTask::publish(MQTT_TOPIC_SCHEDULER_EVENT, payload);
}

// Get action handler for a schedule type
static IScheduleAction* getActionHandler(ScheduleType type) {
    auto it = actionHandlers.find(type);
    return (it != actionHandlers.end()) ? it->second.get() : nullptr;
}

// Get next free schedule ID
static uint8_t getNextFreeId() {
    uint8_t id = 1;
    bool found;

    do {
        found = false;
        for (const auto& schedule : timerSchedules) {
            if (schedule.id == id) {
                found = true;
                id++;
                break;
            }
        }
    } while (found && id < 255);

    return id;
}

// Save schedules to FRAM
static void saveSchedules() {
    if (!scheduleStorage) {
        LOG_ERROR(TAG, "Schedule storage not initialized");
        return;
    }

    LOG_DEBUG(TAG, "Saving %d schedules to FRAM", timerSchedules.size());

    if (!scheduleStorage->saveSchedules(timerSchedules)) {
        LOG_ERROR(TAG, "Failed to save schedules to FRAM");
    } else {
        LOG_INFO(TAG, "Successfully saved %d schedules", timerSchedules.size());
    }
}

// Load schedules from FRAM
static void loadSchedules() {
    if (!scheduleStorage) {
        LOG_ERROR(TAG, "Schedule storage not initialized");
        return;
    }

    LOG_DEBUG(TAG, "Loading schedules from FRAM");

    timerSchedules.clear();
    activeSchedules.clear();

    if (!scheduleStorage->loadSchedules(timerSchedules)) {
        LOG_WARN(TAG, "Failed to load schedules from FRAM");
        // Initialize storage for first use
        if (!scheduleStorage->initializeScheduleStorage()) {
            LOG_ERROR(TAG, "Failed to initialize schedule storage");
        }
    } else {
        LOG_INFO(TAG, "Loaded %d schedules from FRAM", timerSchedules.size());
        // Initialize active state for all loaded schedules
        for (const auto& schedule : timerSchedules) {
            activeSchedules[schedule.id] = false;
        }
    }
}

// Main task function
void TimerSchedulerTask(void* parameter) {
    LOG_INFO(TAG, "Timer scheduler task starting");
    
    // Initialize scheduler
    if (!initializeScheduler()) {
        LOG_ERROR(TAG, "Failed to initialize scheduler");
        vTaskDelete(NULL);
        return;
    }

    // Register cleanup handler for proper resource release
    TaskCleanupHandler::registerCleanup([]() {
        cleanupScheduler();
    });

    // Register with watchdog
    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::disabled();
    (void)SRP::getTaskManager().registerCurrentTaskWithWatchdog("TimerScheduler", wdtConfig);
    
    // Main event loop
    const EventBits_t ALL_EVENTS = SchedulerEvents::SCHEDULER_EVENT_CHECK_SCHEDULE | 
                                   SchedulerEvents::SCHEDULER_EVENT_PUBLISH_STATUS |
                                   SchedulerEvents::SCHEDULER_EVENT_SCHEDULE_CHANGED |
                                   SchedulerEvents::SCHEDULER_EVENT_SAVE_SCHEDULES;
    
    while (true) {
        EventBits_t events = xEventGroupWaitBits(
            schedulerEventGroup,
            ALL_EVENTS,
            pdTRUE,   // Clear bits on exit
            pdFALSE,  // Wait for any bit
            pdMS_TO_TICKS(1000)  // 1 second timeout
        );
        
        if (events & SchedulerEvents::SCHEDULER_EVENT_CHECK_SCHEDULE) {
            checkSchedules();
        }
        
        if (events & SchedulerEvents::SCHEDULER_EVENT_PUBLISH_STATUS) {
            publishSchedulerStatus();
        }
        
        if (events & SchedulerEvents::SCHEDULER_EVENT_SAVE_SCHEDULES) {
            saveSchedules();
            schedulesModified = false;
            lastPersistTime = millis();
        }
        
        // Check if we need to auto-save schedules
        if (schedulesModified && (millis() - lastPersistTime) > SchedulerConstants::PERSIST_INTERVAL_MS) {
            LOG_DEBUG(TAG, "Auto-saving schedules after timeout");
            saveSchedules();
            schedulesModified = false;
            lastPersistTime = millis();
        }

        // Feed watchdog
        (void)SRP::getTaskManager().feedWatchdog();
    }
}

// Publish scheduler status
static void publishSchedulerStatus() {
    const char* status = TimerScheduler::getStatusJSON();
    MQTTTask::publish(MQTT_STATUS_SCHEDULER_INFO, status);
}

// Round 14 Issue #11: Cleanup function for scheduler resources
static void cleanupScheduler() {
    LOG_INFO(TAG, "Cleaning up scheduler resources");

    // Stop and delete schedule check timer
    if (scheduleCheckTimer) {
        xTimerStop(scheduleCheckTimer, pdMS_TO_TICKS(100));
        xTimerDelete(scheduleCheckTimer, pdMS_TO_TICKS(100));
        scheduleCheckTimer = nullptr;
    }

    // Delete event group
    if (schedulerEventGroup) {
        vEventGroupDelete(schedulerEventGroup);
        schedulerEventGroup = nullptr;
    }

    // Delete mutex
    if (schedulesMutex) {
        vSemaphoreDelete(schedulesMutex);
        schedulesMutex = nullptr;
    }

    // Cleanup storage handler
    if (scheduleStorage) {
        delete scheduleStorage;
        scheduleStorage = nullptr;
    }

    isInitialized = false;
    LOG_INFO(TAG, "Scheduler cleanup complete");
}

// Namespace implementation for public interface
namespace TimerScheduler {

void processMQTTCommand(const String& command, const String& payload) {
    if (!isInitialized) {
        LOG_WARN(TAG, "Scheduler not initialized, ignoring command");
        return;
    }
    
    LOG_DEBUG(TAG, "Processing command: %s", command.c_str());
    
    if (command == "add") {
        // Round 14 Issue #12: Check count FIRST before expensive operations
        // This is O(1) and prevents DoS via repeated add requests
        {
            if (schedulesMutex && xSemaphoreTake(schedulesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                size_t currentCount = timerSchedules.size();
                xSemaphoreGive(schedulesMutex);
                if (currentCount >= schedules::MAX_SCHEDULES) {
                    LOG_ERROR(TAG, "Max schedules (%d) reached, rejecting add", schedules::MAX_SCHEDULES);
                    MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE,
                        "{\"success\":false,\"error\":\"max_schedules_reached\"}");
                    return;
                }
            }
        }

        // Limit payload size to prevent heap exhaustion from malicious input
        constexpr size_t MAX_SCHEDULE_JSON_SIZE = 512;
        if (payload.length() > MAX_SCHEDULE_JSON_SIZE) {
            LOG_ERROR(TAG, "Schedule JSON too large: %d bytes (max %d)", payload.length(), MAX_SCHEDULE_JSON_SIZE);
            const char* response = SchedulerResponseFormatter::PreformattedResponses::ERROR_PARSE;
            MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE, response);
            return;
        }

        // Parse and add schedule
        JsonDocument doc;  // ArduinoJson v7
        DeserializationError error = deserializeJson(doc, payload);
        if (error) {
            LOG_ERROR(TAG, "Failed to parse add schedule command: %s", error.c_str());
            const char* response = SchedulerResponseFormatter::PreformattedResponses::ERROR_PARSE;
            MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE, response);
            return;
        }

        // Validate input
        auto validation = MQTTValidator::validateScheduleAdd(doc);
        if (!validation) {
            LOG_ERROR(TAG, "Invalid schedule add command: %s", validation.error);
            const char* response = SchedulerResponseFormatter::formatErrorResponse(validation.error);
            MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE, response);
            return;
        }

        TimerSchedule schedule;
        schedule.id = getNextFreeId();

        // Sanitize name
        char nameBuf[33];
        strncpy(nameBuf, doc["name"] | "Schedule", sizeof(nameBuf) - 1);
        nameBuf[sizeof(nameBuf) - 1] = '\0';
        MQTTValidator::sanitizeString(nameBuf, sizeof(nameBuf));
        schedule.name = nameBuf;
        schedule.enabled = doc["enabled"] | true;

        // Parse schedule type
        const char* typeStr = doc["type"] | "water_heating";
        if (strcmp(typeStr, "water_heating") == 0) {
            schedule.type = ScheduleType::WATER_HEATING;
        } else if (strcmp(typeStr, "space_heating") == 0) {
            schedule.type = ScheduleType::SPACE_HEATING;
        } else {
            LOG_ERROR(TAG, "Unknown schedule type: %s", typeStr);
            return;
        }

        // Parse time with validation
        int startHour = doc["start_hour"] | 0;
        int startMinute = doc["start_minute"] | 0;
        int endHour = doc["end_hour"] | 0;
        int endMinute = doc["end_minute"] | 0;

        // Validate time ranges
        if (startHour < 0 || startHour > 23 || endHour < 0 || endHour > 23 ||
            startMinute < 0 || startMinute > 59 || endMinute < 0 || endMinute > 59) {
            LOG_ERROR(TAG, "Invalid time values: %d:%d - %d:%d", startHour, startMinute, endHour, endMinute);
            MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE,
                "{\"success\":false,\"error\":\"invalid_time_range\"}");
            return;
        }

        schedule.startHour = static_cast<uint8_t>(startHour);
        schedule.startMinute = static_cast<uint8_t>(startMinute);
        schedule.endHour = static_cast<uint8_t>(endHour);
        schedule.endMinute = static_cast<uint8_t>(endMinute);

        // Parse days (can be array or bitmask) - use isNull() instead of containsKey()
        if (!doc["days"].isNull()) {
            if (doc["days"].is<JsonArray>()) {
                schedule.dayMask = 0;
                JsonArray days = doc["days"];
                for (JsonVariant day : days) {
                    int dayNum = day.as<int>();
                    if (dayNum >= 0 && dayNum <= 6) {
                        schedule.dayMask |= (1 << dayNum);
                    }
                }
            } else {
                schedule.dayMask = doc["days"];
            }
        } else {
            schedule.dayMask = 0x7F; // All days
        }
        
        // Parse action-specific data
        IScheduleAction* handler = getActionHandler(schedule.type);
        if (!handler) {
            LOG_ERROR(TAG, "No handler for schedule type");
            return;
        }
        
        // Type-specific parsing with bounds validation
        // Round 14 Issue #13: Use SystemConstants instead of hard-coded values
        if (schedule.type == ScheduleType::WATER_HEATING) {
            int targetTemp = doc["target_temp"] | 55;
            // Validate water heating target using centralized constants
            constexpr int waterMinTemp = SystemConstants::WaterHeating::MIN_TARGET_TEMP / 10;  // 30°C
            constexpr int waterMaxTemp = SystemConstants::WaterHeating::MAX_TARGET_TEMP / 10;  // 85°C
            if (targetTemp < waterMinTemp || targetTemp > waterMaxTemp) {
                LOG_ERROR(TAG, "Invalid water target temp: %d (must be %d-%d°C)",
                         targetTemp, waterMinTemp, waterMaxTemp);
                MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE,
                    "{\"success\":false,\"error\":\"invalid_water_temp\"}");
                return;
            }
            schedule.actionData.waterHeating.targetTempC = static_cast<uint8_t>(targetTemp);
            // Reserved bytes default to 0
            memset(schedule.actionData.waterHeating.reserved, 0,
                   sizeof(schedule.actionData.waterHeating.reserved));
        } else if (schedule.type == ScheduleType::SPACE_HEATING) {
            int targetTemp = doc["target_temp"] | 21;
            // Validate space heating target using centralized constants
            constexpr int spaceMinTemp = SystemConstants::Temperature::SpaceHeating::MIN_TARGET_TEMP / 10;  // 10°C
            constexpr int spaceMaxTemp = SystemConstants::Temperature::SpaceHeating::MAX_TARGET_TEMP / 10;  // 30°C
            if (targetTemp < spaceMinTemp || targetTemp > spaceMaxTemp) {
                LOG_ERROR(TAG, "Invalid space heating target temp: %d (must be %d-%d°C)",
                         targetTemp, spaceMinTemp, spaceMaxTemp);
                MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE,
                    "{\"success\":false,\"error\":\"invalid_space_temp\"}");
                return;
            }
            schedule.actionData.spaceHeating.targetTempC = static_cast<uint8_t>(targetTemp);
            schedule.actionData.spaceHeating.mode = doc["mode"] | 0;
            schedule.actionData.spaceHeating.zones = doc["zones"] | 0xFF;
        }

        // Round 14 Issue #5: Acquire mutex for thread-safe access
        // Note: Count check was already done at start of "add" handler (Issue #12)
        if (schedulesMutex == nullptr || xSemaphoreTake(schedulesMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            LOG_ERROR(TAG, "Failed to acquire schedules mutex");
            MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE,
                "{\"success\":false,\"error\":\"mutex_timeout\"}");
            return;
        }

        // Double-check schedule limit (race protection)
        if (timerSchedules.size() >= schedules::MAX_SCHEDULES) {
            xSemaphoreGive(schedulesMutex);
            LOG_ERROR(TAG, "Max schedules reached (race condition)");
            MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE,
                "{\"success\":false,\"error\":\"max_schedules_reached\"}");
            return;
        }

        // Add schedule (mutex held)
        timerSchedules.push_back(schedule);
        activeSchedules[schedule.id] = false;
        schedulesModified = true;

        xSemaphoreGive(schedulesMutex);

        LOG_INFO(TAG, "Added schedule %d: '%s' (%s)",
                 schedule.id, schedule.name.c_str(), handler->getTypeName());

        // Trigger immediate check and save
        xEventGroupSetBits(schedulerEventGroup, SchedulerEvents::SCHEDULER_EVENT_CHECK_SCHEDULE |
                                               SchedulerEvents::SCHEDULER_EVENT_SAVE_SCHEDULES);

        // Send response using formatter
        const char* response = SchedulerResponseFormatter::formatStatusResponse(true, schedule.id);
        LOG_INFO(TAG, "Publishing add response: %s to %s", response, MQTT_TOPIC_SCHEDULER_RESPONSE);
        MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE, response);
        
    } else if (command == "remove") {
        // Limit payload size (remove command is small - just {id: N})
        constexpr size_t MAX_REMOVE_JSON_SIZE = 64;
        if (payload.length() > MAX_REMOVE_JSON_SIZE) {
            LOG_ERROR(TAG, "Remove JSON too large: %d bytes (max %d)", payload.length(), MAX_REMOVE_JSON_SIZE);
            const char* response = SchedulerResponseFormatter::PreformattedResponses::ERROR_PARSE;
            MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE, response);
            return;
        }

        // Remove schedule
        JsonDocument doc;  // ArduinoJson v7
        DeserializationError error = deserializeJson(doc, payload);
        if (error) {
            LOG_ERROR(TAG, "Failed to parse remove command: %s", error.c_str());
            const char* response = SchedulerResponseFormatter::PreformattedResponses::ERROR_PARSE;
            MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE, response);
            return;
        }
        
        // Validate input
        auto validation = MQTTValidator::validateScheduleRemove(doc);
        if (!validation) {
            LOG_ERROR(TAG, "Invalid schedule remove command: %s", validation.error);
            const char* response = SchedulerResponseFormatter::formatErrorResponse(validation.error);
            MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE, response);
            return;
        }
        
        uint8_t id = doc["id"];
        bool found = false;

        // Round 14 Issue #5: Acquire mutex for thread-safe access
        if (schedulesMutex == nullptr || xSemaphoreTake(schedulesMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            LOG_ERROR(TAG, "Failed to acquire schedules mutex for remove");
            MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE,
                "{\"success\":false,\"error\":\"mutex_timeout\"}");
            return;
        }

        // Find and remove schedule (mutex held)
        for (auto it = timerSchedules.begin(); it != timerSchedules.end(); ++it) {
            if (it->id == id) {
                // Deactivate if active
                if (activeSchedules[id]) {
                    deactivateSchedule(*it);
                }

                LOG_INFO(TAG, "Removing schedule %d: '%s'", id, it->name.c_str());
                timerSchedules.erase(it);
                activeSchedules.erase(id);
                found = true;
                schedulesModified = true;
                break;
            }
        }

        xSemaphoreGive(schedulesMutex);

        // Send response using formatter
        const char* response = found ?
            SchedulerResponseFormatter::formatStatusResponse(true, id) :
            SchedulerResponseFormatter::PreformattedResponses::ERROR_NOT_FOUND;
        MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE, response);

        if (found) {
            // Trigger save
            xEventGroupSetBits(schedulerEventGroup, SchedulerEvents::SCHEDULER_EVENT_SAVE_SCHEDULES);
        }
    } else if (command == "list") {
        // Round 14 Issue #5: Acquire mutex for thread-safe access
        if (schedulesMutex == nullptr || xSemaphoreTake(schedulesMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            LOG_ERROR(TAG, "Failed to acquire schedules mutex for list");
            MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE,
                "{\"success\":false,\"error\":\"mutex_timeout\"}");
            return;
        }

        // List schedules using formatter (mutex held)
        const char* response = SchedulerResponseFormatter::formatScheduleList(timerSchedules);
        xSemaphoreGive(schedulesMutex);

        LOG_INFO(TAG, "Publishing list response: %s", response);
        MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE, response);
    } else if (command == "status") {
        publishSchedulerStatus();
    }
}

const char* getStatusJSON() {
    // Use formatter for consistent response (returns pointer to static buffer)
    return SchedulerResponseFormatter::formatScheduleStatus(
        timerSchedules, activeSchedules, isAnyScheduleActive());
}

bool isAnyScheduleActive() {
    return std::any_of(activeSchedules.begin(), activeSchedules.end(),
                      [](const auto& p) { return p.second; });
}

} // namespace TimerScheduler