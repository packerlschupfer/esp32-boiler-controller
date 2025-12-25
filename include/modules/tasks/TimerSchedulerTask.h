// include/modules/tasks/TimerSchedulerTask.h
// Generic timer scheduler task for managing multiple schedule types
#ifndef TIMER_SCHEDULER_TASK_H
#define TIMER_SCHEDULER_TASK_H

#include <Arduino.h>

// Task function declaration
void TimerSchedulerTask(void* parameter);

// Public interface functions for scheduler interaction
namespace TimerScheduler {
    /**
     * Process MQTT command for scheduler
     * @param command The command (add, remove, list, etc.)
     * @param payload The JSON payload
     */
    void processMQTTCommand(const String& command, const String& payload);
    
    /**
     * Get scheduler status as JSON
     * @return Pointer to static buffer with JSON status (do not free)
     */
    const char* getStatusJSON();
    
    /**
     * Check if any schedule is currently active
     * @return true if at least one schedule is active
     */
    bool isAnyScheduleActive();
}

#endif // TIMER_SCHEDULER_TASK_H