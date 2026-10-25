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
#include "modules/scheduler/SchedulerCommandPolicy.h"
#include "modules/scheduler/SchedulerClock.h"
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
    
    // The RTC stores UTC, so the offset is zero by construction - see SchedulerClock.h for
    // why. This used to derive the live CET/CEST offset here and write local wall-clock into
    // the chip, which lost the offset and left every schedule an hour off across a DST change
    // on a device with no network. It also meant this one function had to get calendar
    // arithmetic right: F37 was a tm_mday comparison that inverted the sign at month and year
    // boundaries (local mday 1 is not > utc mday 31), setting the RTC ~47 h off at UTC Jan 31
    // 23:30. Storing UTC deletes the arithmetic rather than fixing it again.
    LOG_INFO(TAG, "Updating RTC from NTP (UTC): epoch=%ld", (long)utcTime);

    if (rtcController->setTimeFromUTC(static_cast<uint32_t>(utcTime), 0)) {
        // Verify the update
        DateTime now = rtcController->now();
        LOG_INFO(TAG, "RTC updated successfully (UTC): %04d-%02d-%02d %02d:%02d:%02d",
                 now.year(), now.month(), now.day(),
                 now.hour(), now.minute(), now.second());
    } else {
        LOG_ERROR(TAG, "Failed to update RTC from NTP");
    }
}
static uint32_t lastPersistTime = 0;
static bool schedulesModified = false;

// Clock publish cadence. Rollover-safe: compared as an unsigned difference, never as
// (last + interval) which wraps once every 49.7 days.
static constexpr uint32_t CLOCK_PUBLISH_INTERVAL_MS = 60000;
static uint32_t lastClockPublish = 0;

// Forward declarations
static void checkSchedules();
static void checkSchedule(TimerSchedule& schedule,
                          const SchedulerClock::WallClock& nowLocal);
static void activateSchedule(TimerSchedule& schedule);
static void deactivateSchedule(TimerSchedule& schedule);
static void cleanupScheduler();
static IScheduleAction* getActionHandler(ScheduleType type);
static uint8_t getNextFreeId();
static void publishSchedulerStatus();
static void requestStatusPublish();
static void publishClock();
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

    // Register NTP callback to update RTC when time syncs. main.cpp starts this task before
    // NTPTask precisely so this lands first; log what actually happened rather than assuming,
    // because for the life of this project the registration was silently discarded and this
    // line claimed success anyway.
    if (setNTPRTCCallback(updateRTCFromNTP)) {
        LOG_INFO(TAG, "Registered NTP RTC update callback");
    } else {
        LOG_ERROR(TAG, "NTP RTC callback NOT registered - the RTC will never be corrected");
    }

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

    if (!rtcController) {
        return;  // initializeScheduler() refuses to start without an RTC, so this cannot fire
    }

    // The RTC holds UTC; the wall-clock a schedule is written in is DERIVED from it with the
    // compiled DST rule - see SchedulerClock.h for why the chip no longer stores local time.
    //
    // Read the chip ONCE per pass and hand the same instant to every schedule. It used to be
    // read inside checkSchedule(), i.e. once per schedule over an I2C bus, so a pass that
    // straddled a minute boundary could evaluate one schedule at 09:59 and the next at 10:00.
    // At a handover - schedule A ending at 10:00, B starting at 10:00 - that is how you get
    // both running at once, or neither.
    //
    // S1: an implausible instant means the RTC lost its battery and NTP has never run. There
    // is no trustworthy time, so no schedule may be evaluated. (Note this leaves an already
    // active schedule active; that is the pre-existing behaviour and is not changed here.)
    const SchedulerClock::WallClock nowLocal =
        SchedulerClock::localWallClock(static_cast<int64_t>(rtcController->now().unixtime()));
    if (!nowLocal.valid) {
        static uint32_t lastRtcWarn = 0;
        if (millis() - lastRtcWarn > 60000) {  // Warn once per minute
            LOG_ERROR(TAG, "RTC time invalid (year=%d) - schedules suspended",
                      rtcController->now().year());
            lastRtcWarn = millis();
        }
        return;  // Skip all schedule checks
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
        checkSchedule(schedule, nowLocal);
    }

    xSemaphoreGive(schedulesMutex);
}

// Check a single schedule against the instant resolved by checkSchedules()
static void checkSchedule(TimerSchedule& schedule, const SchedulerClock::WallClock& nowLocal) {
    if (!schedule.enabled) {
        // If disabled but active, deactivate
        if (activeSchedules[schedule.id]) {
            deactivateSchedule(schedule);
        }
        return;
    }

    bool shouldBeActive = schedule.isActiveNow(nowLocal.hour, nowLocal.minute,
                                               nowLocal.dayOfWeek);
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
    requestStatusPublish();
    
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
    requestStatusPublish();

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
        
        // Publish the clock on a slow cadence - it is a diagnostic, and each publish costs an
        // I2C read of the DS3231 on the bus the FRAM shares.
        if ((millis() - lastClockPublish) >= CLOCK_PUBLISH_INTERVAL_MS) {
            lastClockPublish = millis();
            publishClock();
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

// Ask the task loop to publish the scheduler status.
//
// SCHEDULER_EVENT_PUBLISH_STATUS was defined, waited on, and handled - and SET BY NOTHING, so
// publishSchedulerStatus() was unreachable and boiler/status/scheduler/info only ever appeared
// as a reply to an explicit `status` command. Noticed 2026-10-25 while placing publishClock(),
// which is why that went into the task loop instead. Driving the bit from here rather than
// from the four `schedulesModified` sites and the two activate/deactivate sites individually
// is the point: one function to call, nothing to forget at a seventh site later.
static void requestStatusPublish() {
    if (schedulerEventGroup) {
        xEventGroupSetBits(schedulerEventGroup, SchedulerEvents::SCHEDULER_EVENT_PUBLISH_STATUS);
    }
}

// Publish the clock: the RTC's stored instant and the local wall-clock derived from it.
//
// This runs from the scheduler task because the task already owns the DS3231 reads - the
// chip sits on the shared I2C bus with the FRAM, and reading it from the MQTT task instead
// would put a second reader on that bus for a diagnostic.
//
// Deliberately NOT folded into publishSchedulerStatus(): nothing sets
// SCHEDULER_EVENT_PUBLISH_STATUS, so that function is only ever reached from the MQTT command
// reply path and a clock published there would only appear when someone asked for it.
static void publishClock() {
    if (!rtcController) {
        return;
    }
    const int64_t utc = static_cast<int64_t>(rtcController->now().unixtime());
    const SchedulerClock::WallClock w = SchedulerClock::localWallClock(utc);

    char payload[160];
    if (!w.valid) {
        // Say so explicitly rather than publishing a plausible-looking 1970. An invalid clock
        // means schedules are suspended, which is worth being able to see from outside.
        snprintf(payload, sizeof(payload),
                 "{\"valid\":false,\"utc\":%lld,\"ntp\":%s}",
                 static_cast<long long>(utc), isNTPSynced() ? "true" : "false");
    } else {
        snprintf(payload, sizeof(payload),
                 "{\"valid\":true,\"utc\":%lld,"
                 "\"local\":\"%04d-%02d-%02d %02d:%02d:%02d\","
                 "\"dow\":%u,\"dst\":%s,\"ntp\":%s}",
                 static_cast<long long>(utc),
                 w.year, w.month, w.day, w.hour, w.minute, w.second,
                 static_cast<unsigned>(w.dayOfWeek),
                 w.dst ? "true" : "false",
                 isNTPSynced() ? "true" : "false");
    }
    MQTTTask::publish(MQTT_STATUS_CLOCK, payload);
}

// Publish scheduler status
static void publishSchedulerStatus() {
    // Local buffer: called from the scheduler task and from the MQTT task ("status")
    char status[128];
    MQTTTask::publish(MQTT_STATUS_SCHEDULER_INFO, TimerScheduler::getStatusJSON(status, sizeof(status)));
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

// Reply buffer for processMQTTCommand(), which only runs in the MQTT task (single-task
// static, docs/MEMORY_OPTIMIZATION.md). Sized to the MQTT publish payload.
static char s_replyBuffer[sizeof(MQTTPublishRequest::payload)];

void processMQTTCommand(const String& command, const String& payload) {
    if (!isInitialized) {
        // Reply instead of returning silently: a command without an answer looks like a hang
        LOG_WARN(TAG, "Scheduler not initialized, rejecting command");
        MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE,
            "{\"status\":\"error\",\"msg\":\"not_initialized\"}");
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
            const char* response = SchedulerResponseFormatter::formatErrorResponse(
                s_replyBuffer, sizeof(s_replyBuffer), validation.error);
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
            MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE,
                SchedulerResponseFormatter::PreformattedResponses::ERROR_INVALID_TYPE);
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
            MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE,
                SchedulerResponseFormatter::PreformattedResponses::ERROR_INVALID_TYPE);
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
            // "priority" is not parsed: waterHeating.priority is only logged at start and not
            // stored in FRAM; water priority is the global WATER_PRIORITY state (boiler/cmd/water)
        } else if (schedule.type == ScheduleType::SPACE_HEATING) {
            // Mode first: without target_temp the schedule uses the mode's default temperature
            if (!doc["mode"].isNull() && !doc["mode"].is<int>()) {
                LOG_ERROR(TAG, "Invalid space heating mode type");
                MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE,
                    "{\"success\":false,\"error\":\"invalid_mode\"}");
                return;
            }
            int mode = doc["mode"] | 0;
            if (!SchedulerCommandPolicy::spaceModeValid(mode)) {
                LOG_ERROR(TAG, "Invalid space heating mode: %d (must be 0-2)", mode);
                MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE,
                    "{\"success\":false,\"error\":\"invalid_mode\"}");
                return;
            }
            int targetTemp = doc["target_temp"] |
                SchedulerCommandPolicy::spaceDefaultTargetC(static_cast<uint8_t>(mode));
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
        requestStatusPublish();

        xSemaphoreGive(schedulesMutex);

        LOG_INFO(TAG, "Added schedule %d: '%s' (%s)",
                 schedule.id, schedule.name.c_str(), handler->getTypeName());

        // Trigger immediate check and save
        xEventGroupSetBits(schedulerEventGroup, SchedulerEvents::SCHEDULER_EVENT_CHECK_SCHEDULE |
                                               SchedulerEvents::SCHEDULER_EVENT_SAVE_SCHEDULES);

        // Send response using formatter
        const char* response = SchedulerResponseFormatter::formatStatusResponse(
            s_replyBuffer, sizeof(s_replyBuffer), true, schedule.id);
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
            const char* response = SchedulerResponseFormatter::formatErrorResponse(
                s_replyBuffer, sizeof(s_replyBuffer), validation.error);
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
                requestStatusPublish();
                break;
            }
        }

        xSemaphoreGive(schedulesMutex);

        // Send response using formatter
        const char* response = found ?
            SchedulerResponseFormatter::formatStatusResponse(s_replyBuffer, sizeof(s_replyBuffer), true, id) :
            SchedulerResponseFormatter::PreformattedResponses::ERROR_NOT_FOUND;
        MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE, response);

        if (found) {
            // Trigger save
            xEventGroupSetBits(schedulerEventGroup, SchedulerEvents::SCHEDULER_EVENT_SAVE_SCHEDULES);
        }
    } else if (command == "enable" || command == "disable") {
        // Payload {"id":N,"enabled":true|false}; the disable alias takes {"id":N}
        const bool disableAlias = (command == "disable");
        constexpr size_t MAX_ENABLE_JSON_SIZE = 64;
        if (payload.length() > MAX_ENABLE_JSON_SIZE) {
            LOG_ERROR(TAG, "Enable JSON too large: %d bytes (max %d)", payload.length(), MAX_ENABLE_JSON_SIZE);
            const char* response = SchedulerResponseFormatter::PreformattedResponses::ERROR_PARSE;
            MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE, response);
            return;
        }

        JsonDocument doc;  // ArduinoJson v7
        DeserializationError error = deserializeJson(doc, payload);
        if (error) {
            LOG_ERROR(TAG, "Failed to parse enable command: %s", error.c_str());
            const char* response = SchedulerResponseFormatter::PreformattedResponses::ERROR_PARSE;
            MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE, response);
            return;
        }

        SchedulerCommandPolicy::EnablePayload request;
        request.hasId = !doc["id"].isNull();
        request.idIsInt = doc["id"].is<int>();
        request.id = doc["id"].as<long>();
        request.hasEnabled = !doc["enabled"].isNull();
        request.enabledIsBool = doc["enabled"].is<bool>();

        if (disableAlias) {
            // The topic carries the state, so "enabled" may be omitted; a payload asking for
            // the opposite state is a mistake and must not disable the schedule anyway
            const char* conflict = SchedulerCommandPolicy::validateDisableAlias(
                request.hasEnabled, request.enabledIsBool, doc["enabled"].as<bool>());
            if (conflict) {
                LOG_ERROR(TAG, "Invalid schedule disable command: %s", conflict);
                const char* response = SchedulerResponseFormatter::formatErrorResponse(
                    s_replyBuffer, sizeof(s_replyBuffer), conflict);
                MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE, response);
                return;
            }
            request.hasEnabled = true;
            request.enabledIsBool = true;
        }

        const char* invalid = SchedulerCommandPolicy::validateEnable(request);
        if (invalid) {
            LOG_ERROR(TAG, "Invalid schedule enable command: %s", invalid);
            const char* response = SchedulerResponseFormatter::formatErrorResponse(
                s_replyBuffer, sizeof(s_replyBuffer), invalid);
            MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE, response);
            return;
        }

        uint8_t id = static_cast<uint8_t>(request.id);
        bool enable = disableAlias ? false : doc["enabled"].as<bool>();
        bool found = false;
        bool changed = false;

        if (schedulesMutex == nullptr || xSemaphoreTake(schedulesMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            LOG_ERROR(TAG, "Failed to acquire schedules mutex for enable");
            MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE,
                "{\"success\":false,\"error\":\"mutex_timeout\"}");
            return;
        }

        // Find schedule and apply (mutex held)
        for (auto& schedule : timerSchedules) {
            if (schedule.id != id) continue;

            auto activeIt = activeSchedules.find(id);
            bool active = (activeIt != activeSchedules.end()) && activeIt->second;
            auto decision = SchedulerCommandPolicy::decideEnable(schedule.enabled, enable, active);

            schedule.enabled = enable;
            if (decision.endActiveRun) {
                // End the running schedule like remove does (releases its heating/water request)
                deactivateSchedule(schedule);
            }
            if (decision.changed) {
                LOG_INFO(TAG, "Schedule %d '%s' %s", id, schedule.name.c_str(),
                         enable ? "enabled" : "disabled");
                schedulesModified = true;
                requestStatusPublish();
            }
            changed = decision.changed;
            found = true;
            break;
        }

        xSemaphoreGive(schedulesMutex);

        const char* response = found ?
            SchedulerResponseFormatter::formatEnableResponse(s_replyBuffer, sizeof(s_replyBuffer), id, enable) :
            SchedulerResponseFormatter::PreformattedResponses::ERROR_NOT_FOUND;
        MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE, response);

        if (changed) {
            // Save; an enabled schedule inside its time window starts on this check
            xEventGroupSetBits(schedulerEventGroup, SchedulerEvents::SCHEDULER_EVENT_CHECK_SCHEDULE |
                                                   SchedulerEvents::SCHEDULER_EVENT_SAVE_SCHEDULES);
        }
    } else if (command == "clear") {
        // Erases every schedule: require the explicit payload, like the FRAM format command
        if (!SchedulerCommandPolicy::clearConfirmed(payload.c_str())) {
            LOG_WARN(TAG, "Scheduler clear requested - send '%s' to proceed",
                     SchedulerCommandPolicy::CLEAR_CONFIRM_PAYLOAD);
            const char* response = SchedulerResponseFormatter::formatErrorResponse(
                s_replyBuffer, sizeof(s_replyBuffer), "use_confirm");
            MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE, response);
            return;
        }

        if (schedulesMutex == nullptr || xSemaphoreTake(schedulesMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            LOG_ERROR(TAG, "Failed to acquire schedules mutex for clear");
            MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE,
                "{\"success\":false,\"error\":\"mutex_timeout\"}");
            return;
        }

        // End running schedules first (mutex held): releases their water/heating request
        // like remove does, otherwise the request would stay until the next check
        const size_t cleared = timerSchedules.size();
        for (auto& schedule : timerSchedules) {
            auto activeIt = activeSchedules.find(schedule.id);
            if (activeIt != activeSchedules.end() && activeIt->second) {
                deactivateSchedule(schedule);
            }
        }
        timerSchedules.clear();
        activeSchedules.clear();
        schedulesModified = true;
        requestStatusPublish();

        xSemaphoreGive(schedulesMutex);

        LOG_WARN(TAG, "Cleared %u schedules via MQTT", static_cast<unsigned>(cleared));

        const char* response = SchedulerResponseFormatter::formatClearResponse(
            s_replyBuffer, sizeof(s_replyBuffer), static_cast<unsigned>(cleared));
        MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE, response);

        // Save the empty list (ScheduleStorage writes a header with count 0)
        xEventGroupSetBits(schedulerEventGroup, SchedulerEvents::SCHEDULER_EVENT_SAVE_SCHEDULES);
    } else if (command == "list") {
        // Round 14 Issue #5: Acquire mutex for thread-safe access
        if (schedulesMutex == nullptr || xSemaphoreTake(schedulesMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            LOG_ERROR(TAG, "Failed to acquire schedules mutex for list");
            MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE,
                "{\"success\":false,\"error\":\"mutex_timeout\"}");
            return;
        }

        // List schedules using formatter (mutex held)
        const char* response = SchedulerResponseFormatter::formatScheduleList(
            s_replyBuffer, sizeof(s_replyBuffer), timerSchedules);
        xSemaphoreGive(schedulesMutex);

        LOG_INFO(TAG, "Publishing list response: %s", response);
        MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE, response);
    } else if (command == "status") {
        // Reply buffer instead of publishSchedulerStatus()'s 128-byte stack buffer: room for
        // all active and disabled IDs, and less MQTT task stack
        MQTTTask::publish(MQTT_STATUS_SCHEDULER_INFO, getStatusJSON(s_replyBuffer, sizeof(s_replyBuffer)));
    } else {
        // boiler/cmd/scheduler/+ delivers every sub-topic here, so a command without a branch
        // must answer: it was ignored before and looked like a hung command (2026-09-16)
        const char* reason = SchedulerCommandPolicy::unhandledReply(command.c_str());
        LOG_WARN(TAG, "Scheduler command '%s': %s", command.c_str(), reason);
        const char* response = SchedulerResponseFormatter::formatErrorResponse(
            s_replyBuffer, sizeof(s_replyBuffer), reason);
        MQTTTask::publish(MQTT_TOPIC_SCHEDULER_RESPONSE, response);
    }
}

const char* getStatusJSON(char* out, size_t size) {
    // The scheduler task inserts into activeSchedules while checking schedules
    if (schedulesMutex == nullptr || xSemaphoreTake(schedulesMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return "{\"success\":false,\"error\":\"mutex_timeout\"}";
    }
    const char* json = SchedulerResponseFormatter::formatScheduleStatus(
        out, size, timerSchedules, activeSchedules, isAnyScheduleActive());
    xSemaphoreGive(schedulesMutex);
    return json;
}

bool isAnyScheduleActive() {
    return std::any_of(activeSchedules.begin(), activeSchedules.end(),
                      [](const auto& p) { return p.second; });
}

} // namespace TimerScheduler