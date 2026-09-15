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
     * @param out Caller buffer (at least 48 bytes)
     * @param size Size of out
     * @return out, or a constant error JSON if the buffer is too small
     */
    const char* getStatusJSON(char* out, size_t size);
    
    /**
     * Check if any schedule is currently active
     * @return true if at least one schedule is active
     */
    bool isAnyScheduleActive();
}

#endif // TIMER_SCHEDULER_TASK_H