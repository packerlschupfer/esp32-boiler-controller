// src/modules/scheduler/SchedulerContext.cpp

#include "SchedulerContext.h"
#include "DS3231Controller.h"
#include "RuntimeStorage.h"
#include "LoggingMacros.h"
#include <algorithm>

static const char* TAG = "SchedulerContext";

bool SchedulerContext::initialize(DS3231Controller* rtc, 
                                 rtstorage::RuntimeStorage* storage,
                                 schedules::ScheduleStorage* schedStorage) {
    if (initialized_) {
        LOG_WARN(TAG, "Already initialized");
        return true;
    }
    
    rtcController_ = rtc;
    runtimeStorage_ = storage;
    scheduleStorage_ = schedStorage;
    
    // Create event group
    eventGroup_ = xEventGroupCreate();
    if (!eventGroup_) {
        LOG_ERROR(TAG, "Failed to create event group");
        return false;
    }
    
    // Clear state
    schedules_.clear();
    activeSchedules_.clear();
    schedulesModified_ = false;
    lastPersistTime_ = millis();
    nextScheduleId_ = 1;
    
    initialized_ = true;
    LOG_INFO(TAG, "Initialized successfully");
    return true;
}

void SchedulerContext::cleanup() {
    if (checkTimer_) {
        xTimerDelete(checkTimer_, 0);
        checkTimer_ = nullptr;
    }
    
    if (eventGroup_) {
        vEventGroupDelete(eventGroup_);
        eventGroup_ = nullptr;
    }
    
    schedules_.clear();
    actionHandlers_.clear();
    activeSchedules_.clear();
    
    initialized_ = false;
}

bool SchedulerContext::addSchedule(const TimerSchedule& schedule) {
    if (schedules_.size() >= MAX_SCHEDULES) {
        LOG_ERROR(TAG, "Schedule limit reached");
        return false;
    }
    
    schedules_.push_back(schedule);
    activeSchedules_[schedule.id] = false;
    schedulesModified_ = true;
    
    return true;
}

bool SchedulerContext::removeSchedule(uint8_t id) {
    auto it = std::find_if(schedules_.begin(), schedules_.end(),
                          [id](const TimerSchedule& s) { return s.id == id; });
    
    if (it != schedules_.end()) {
        schedules_.erase(it);
        activeSchedules_.erase(id);
        schedulesModified_ = true;
        return true;
    }
    
    return false;
}

TimerSchedule* SchedulerContext::findSchedule(uint8_t id) {
    auto it = std::find_if(schedules_.begin(), schedules_.end(),
                          [id](const TimerSchedule& s) { return s.id == id; });
    
    return (it != schedules_.end()) ? &(*it) : nullptr;
}

void SchedulerContext::registerActionHandler(ScheduleType type, 
                                            std::unique_ptr<IScheduleAction> handler) {
    actionHandlers_[type] = std::move(handler);
}

IScheduleAction* SchedulerContext::getActionHandler(ScheduleType type) {
    auto it = actionHandlers_.find(type);
    return (it != actionHandlers_.end()) ? it->second.get() : nullptr;
}

uint8_t SchedulerContext::getNextFreeId() {
    // Find the lowest unused ID
    while (true) {
        bool inUse = false;
        for (const auto& schedule : schedules_) {
            if (schedule.id == nextScheduleId_) {
                inUse = true;
                break;
            }
        }
        
        if (!inUse) {
            return nextScheduleId_++;
        }
        
        nextScheduleId_++;
        if (nextScheduleId_ == 0) {
            nextScheduleId_ = 1; // Wrap around, skip 0
        }
    }
}