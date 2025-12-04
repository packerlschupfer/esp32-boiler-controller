// src/modules/scheduler/SchedulerContext.h
#pragma once

#include <vector>
#include <map>
#include <memory>
#include <unordered_map>
#include "TimerSchedule.h"
#include "IScheduleAction.h"
#include "config/SystemConstants.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

// Forward declarations
class DS3231Controller;
namespace rtstorage { class RuntimeStorage; }
namespace schedules { class ScheduleStorage; }

/**
 * @brief Encapsulates scheduler state to reduce global variables
 * 
 * This class holds all the state that was previously in global/static variables,
 * providing better encapsulation and testability.
 */
class SchedulerContext {
public:
    // Event bits for scheduler operations
    static constexpr uint32_t SCHEDULER_EVENT_CHECK_SCHEDULE = (1 << 0);
    static constexpr uint32_t SCHEDULER_EVENT_PUBLISH_STATUS = (1 << 1);
    static constexpr uint32_t SCHEDULER_EVENT_SCHEDULE_CHANGED = (1 << 2);
    static constexpr uint32_t SCHEDULER_EVENT_SAVE_SCHEDULES = (1 << 3);
    
    // Timing constants - use centralized values
    static constexpr uint32_t CHECK_INTERVAL_MS = SystemConstants::Tasks::Scheduler::CHECK_INTERVAL_MS;
    static constexpr uint32_t PERSIST_INTERVAL_MS = SystemConstants::Tasks::Scheduler::PERSIST_INTERVAL_MS;
    
    // Maximum schedules
    static constexpr size_t MAX_SCHEDULES = 20;
    
private:
    // Core components
    DS3231Controller* rtcController_;
    rtstorage::RuntimeStorage* runtimeStorage_;
    schedules::ScheduleStorage* scheduleStorage_;
    
    // Schedule data
    std::vector<TimerSchedule> schedules_;
    std::map<ScheduleType, std::unique_ptr<IScheduleAction>> actionHandlers_;
    std::unordered_map<uint8_t, bool> activeSchedules_;
    
    // FreeRTOS handles
    EventGroupHandle_t eventGroup_;
    TimerHandle_t checkTimer_;
    
    // State tracking
    bool initialized_;
    bool schedulesModified_;
    uint32_t lastPersistTime_;
    uint8_t nextScheduleId_;
    
    // Singleton instance
    static SchedulerContext* instance_;
    
    // Private constructor for singleton
    SchedulerContext() 
        : rtcController_(nullptr)
        , runtimeStorage_(nullptr)
        , scheduleStorage_(nullptr)
        , eventGroup_(nullptr)
        , checkTimer_(nullptr)
        , initialized_(false)
        , schedulesModified_(false)
        , lastPersistTime_(0)
        , nextScheduleId_(1) {
    }
    
public:
    // Singleton access
    static SchedulerContext& getInstance() {
        if (!instance_) {
            instance_ = new SchedulerContext();
        }
        return *instance_;
    }
    
    // Prevent copying
    SchedulerContext(const SchedulerContext&) = delete;
    SchedulerContext& operator=(const SchedulerContext&) = delete;
    
    // Initialization
    bool initialize(DS3231Controller* rtc, 
                   rtstorage::RuntimeStorage* storage,
                   schedules::ScheduleStorage* schedStorage);
    
    void cleanup();
    
    // Component access
    DS3231Controller* getRTC() { return rtcController_; }
    schedules::ScheduleStorage* getScheduleStorage() { return scheduleStorage_; }
    EventGroupHandle_t getEventGroup() { return eventGroup_; }
    
    // Schedule management
    std::vector<TimerSchedule>& getSchedules() { return schedules_; }
    const std::vector<TimerSchedule>& getSchedules() const { return schedules_; }
    
    bool addSchedule(const TimerSchedule& schedule);
    bool removeSchedule(uint8_t id);
    TimerSchedule* findSchedule(uint8_t id);
    
    // Action handlers
    void registerActionHandler(ScheduleType type, std::unique_ptr<IScheduleAction> handler);
    IScheduleAction* getActionHandler(ScheduleType type);
    
    // Active schedule tracking
    void setScheduleActive(uint8_t id, bool active) { 
        activeSchedules_[id] = active; 
        schedulesModified_ = true;
    }
    
    bool isScheduleActive(uint8_t id) const {
        auto it = activeSchedules_.find(id);
        return it != activeSchedules_.end() && it->second;
    }
    
    bool isAnyScheduleActive() const {
        for (const auto& [id, active] : activeSchedules_) {
            if (active) return true;
        }
        return false;
    }
    
    const std::unordered_map<uint8_t, bool>& getActiveSchedules() const {
        return activeSchedules_;
    }
    
    // State management
    bool isInitialized() const { return initialized_; }
    void setInitialized(bool init) { initialized_ = init; }
    
    bool isModified() const { return schedulesModified_; }
    void setModified(bool modified = true) { schedulesModified_ = modified; }
    
    uint32_t getLastPersistTime() const { return lastPersistTime_; }
    void updatePersistTime() { lastPersistTime_ = millis(); }
    
    // ID management
    uint8_t getNextFreeId();
    
    // Event management
    void signalEvent(uint32_t bits) {
        if (eventGroup_) {
            xEventGroupSetBits(eventGroup_, bits);
        }
    }
    
    EventBits_t waitForEvents(uint32_t bits, TickType_t timeout) {
        if (!eventGroup_) return 0;
        return xEventGroupWaitBits(eventGroup_, bits, pdTRUE, pdFALSE, timeout);
    }
};

// Define static member
inline SchedulerContext* SchedulerContext::instance_ = nullptr;