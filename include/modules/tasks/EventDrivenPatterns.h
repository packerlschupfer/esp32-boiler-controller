// include/modules/tasks/EventDrivenPatterns.h
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <freertos/event_groups.h>
#include <functional>
#include <unordered_map>
#include "LoggingMacros.h"

/**
 * @brief Event-driven task patterns for ESPlan
 * 
 * This header provides common patterns for converting polling tasks
 * to event-driven designs.
 */

/**
 * @brief Periodic timer with task notification
 * 
 * Creates a timer that notifies a task at regular intervals
 */
class PeriodicNotifier {
private:
    TimerHandle_t timer_;
    TaskHandle_t taskToNotify_;
    uint32_t notificationValue_;
    
    static void timerCallback(TimerHandle_t xTimer) {
        PeriodicNotifier* self = static_cast<PeriodicNotifier*>(pvTimerGetTimerID(xTimer));
        if (self && self->taskToNotify_) {
            xTaskNotify(self->taskToNotify_, self->notificationValue_, eSetBits);
        }
    }
    
public:
    PeriodicNotifier(const char* name, uint32_t periodMs, TaskHandle_t task, uint32_t notifyValue = 1)
        : taskToNotify_(task), notificationValue_(notifyValue) {
        timer_ = xTimerCreate(name, pdMS_TO_TICKS(periodMs), pdTRUE, this, timerCallback);
    }
    
    ~PeriodicNotifier() {
        if (timer_) {
            xTimerDelete(timer_, pdMS_TO_TICKS(100));
        }
    }
    
    bool start() {
        return timer_ && xTimerStart(timer_, pdMS_TO_TICKS(100)) == pdPASS;
    }
    
    bool stop() {
        return timer_ && xTimerStop(timer_, pdMS_TO_TICKS(100)) == pdPASS;
    }
    
    bool changePeriod(uint32_t newPeriodMs) {
        return timer_ && xTimerChangePeriod(timer_, pdMS_TO_TICKS(newPeriodMs), pdMS_TO_TICKS(100)) == pdPASS;
    }
};

/**
 * @brief Event aggregator for multiple event sources
 * 
 * Allows a task to wait for events from multiple sources efficiently
 */
class EventAggregator {
private:
    EventGroupHandle_t eventGroup_;
    static constexpr uint32_t MAX_SOURCES = 24;  // EventGroup has 24 bits

    // Ensure event group is created (lazy initialization for static instances)
    void ensureCreated() {
        if (!eventGroup_) {
            eventGroup_ = xEventGroupCreate();
            if (!eventGroup_) {
                LOG_ERROR("EventAggregator", "Failed to create event group!");
            }
        }
    }

public:
    // Don't create in constructor - may be called during static init before FreeRTOS starts
    EventAggregator() : eventGroup_(nullptr) {}

    ~EventAggregator() {
        if (eventGroup_) {
            vEventGroupDelete(eventGroup_);
        }
    }

    EventGroupHandle_t getHandle() {
        ensureCreated();
        return eventGroup_;
    }
    
    void setEvent(EventBits_t bits) {
        ensureCreated();
        if (eventGroup_) {
            xEventGroupSetBits(eventGroup_, bits);
        }
    }
    
    void setEventFromISR(EventBits_t bits, BaseType_t* pxHigherPriorityTaskWoken) {
        if (eventGroup_) {
            xEventGroupSetBitsFromISR(eventGroup_, bits, pxHigherPriorityTaskWoken);
        }
    }
    
    EventBits_t waitForAnyEvent(EventBits_t bitsToWaitFor, TickType_t timeout = portMAX_DELAY) {
        ensureCreated();
        if (!eventGroup_) return 0;
        return xEventGroupWaitBits(eventGroup_, bitsToWaitFor, pdTRUE, pdFALSE, timeout);
    }

    EventBits_t waitForAllEvents(EventBits_t bitsToWaitFor, TickType_t timeout = portMAX_DELAY) {
        ensureCreated();
        if (!eventGroup_) return 0;
        return xEventGroupWaitBits(eventGroup_, bitsToWaitFor, pdTRUE, pdTRUE, timeout);
    }
};

/**
 * @brief State machine helper for event-driven tasks
 */
template<typename StateEnum>
class EventDrivenStateMachine {
public:
    using StateHandler = std::function<StateEnum(EventBits_t events)>;
    
private:
    StateEnum currentState_;
    std::unordered_map<StateEnum, StateHandler> stateHandlers_;
    EventAggregator eventAggregator_;
    const char* tag_;
    
public:
    EventDrivenStateMachine(StateEnum initialState, const char* logTag) 
        : currentState_(initialState), tag_(logTag) {}
    
    void registerStateHandler(StateEnum state, StateHandler handler) {
        stateHandlers_[state] = handler;
    }
    
    EventGroupHandle_t getEventGroup() {
        return eventAggregator_.getHandle();
    }
    
    void triggerEvent(EventBits_t bits) {
        eventAggregator_.setEvent(bits);
    }
    
    void run(EventBits_t eventMask) {
        while (true) {
            // Wait for any event
            EventBits_t events = eventAggregator_.waitForAnyEvent(eventMask);
            
            if (events == 0) continue;  // Timeout
            
            // Find and execute current state handler
            auto it = stateHandlers_.find(currentState_);
            if (it != stateHandlers_.end()) {
                StateEnum newState = it->second(events);
                
                if (newState != currentState_) {
                    LOG_INFO(tag_, "State transition: %d -> %d", 
                            static_cast<int>(currentState_), static_cast<int>(newState));
                    currentState_ = newState;
                }
            } else {
                LOG_ERROR(tag_, "No handler for state %d", static_cast<int>(currentState_));
            }
        }
    }
    
    StateEnum getCurrentState() const { return currentState_; }
};

/**
 * @brief Notification bits for common events
 */
enum EventNotificationBits : uint32_t {
    EVENT_TIMER_TICK        = (1 << 0),
    EVENT_DATA_READY        = (1 << 1),
    EVENT_ERROR_OCCURRED    = (1 << 2),
    EVENT_STATE_CHANGE      = (1 << 3),
    EVENT_REQUEST_RECEIVED  = (1 << 4),
    EVENT_SHUTDOWN_REQUEST  = (1 << 5),
    EVENT_PRIORITY_REQUEST  = (1 << 6),
    EVENT_CONFIG_CHANGED    = (1 << 7),
    // Add more as needed up to bit 23
};

/**
 * @brief Helper to convert polling delay to event wait
 * 
 * Usage: Instead of vTaskDelay(pdMS_TO_TICKS(1000))
 *        Use: waitForEventOrTimeout(eventGroup, EVENT_MASK, 1000)
 */
inline EventBits_t waitForEventOrTimeout(EventGroupHandle_t eventGroup, 
                                         EventBits_t bitsToWaitFor, 
                                         uint32_t timeoutMs) {
    return xEventGroupWaitBits(eventGroup, bitsToWaitFor, pdTRUE, pdFALSE, 
                              timeoutMs == 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeoutMs));
}