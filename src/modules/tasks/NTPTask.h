// src/modules/tasks/NTPTask.h
#pragma once

#include <Arduino.h>
#include <functional>

// NTP Task function for FreeRTOS
void NTPTask(void* parameter);

// NTP synchronization callback type
using NTPSyncCallback = std::function<void(time_t)>;

// Public API functions
void setNTPRTCCallback(NTPSyncCallback callback);
bool forceNTPSync();
bool isNTPSynced();
time_t getLastNTPSyncTime();