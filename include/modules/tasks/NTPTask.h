// include/modules/tasks/NTPTask.h
#ifndef NTP_TASK_H
#define NTP_TASK_H

#include <Arduino.h>
#include <functional>

// Main NTP task function
void NTPTask(void* parameter);

// NTP synchronization callbacks (optional)
typedef std::function<void(time_t epochTime)> NTPSyncCallback;

// Set RTC update callback (called when NTP successfully syncs)
void setNTPRTCCallback(NTPSyncCallback callback);

// Force immediate NTP sync (returns success/failure)
bool forceNTPSync();

// Check if NTP is synchronized
bool isNTPSynced();

// Get last sync time
time_t getLastNTPSyncTime();

#endif // NTP_TASK_H