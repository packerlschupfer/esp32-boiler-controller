// src/modules/tasks/OTATask_callbacks.cpp
// OTA callbacks implementation

#include "OTATask.h"
#include <SemaphoreGuard.h>
#include <OTAManager.h>
#include <EthernetManager.h>
#include "utils/ErrorHandler.h"
#include "utils/CriticalDataStorage.h"
#include "LoggingMacros.h"
#include "config/ProjectConfig.h"

static const char* TAG = "OTA";

// OTA callback implementations
void OTATask::onOTAStart() {
    LOG_INFO(TAG, "OTA update starting");

    {
        SemaphoreGuard guard(otaStatusMutex, pdMS_TO_TICKS(100));
        if (guard.hasLock()) {
            otaUpdateInProgress = true;
        } else {
            LOG_ERROR(TAG, "Failed to acquire OTA status mutex on start");
        }
    }

    // Round 21: Save critical state before OTA update
    LOG_INFO(TAG, "Saving critical state before OTA...");

    // Save runtime counters to FRAM
    if (CriticalDataStorage::saveRuntimeCounters()) {
        LOG_INFO(TAG, "Runtime counters saved");
    } else {
        LOG_WARN(TAG, "Failed to save runtime counters");
    }

    // Log OTA event to safety log
    CriticalDataStorage::logSafetyEvent(
        0x01,  // Event type: System update
        0x01,  // Action: OTA started
        0       // No additional data
    );

    // Indicate OTA in progress with LED
    // StatusLed::setPattern(1, 200, 200);  // Fast blink
}

void OTATask::onOTAEnd() {
    LOG_INFO(TAG, "OTA update ended");

    {
        SemaphoreGuard guard(otaStatusMutex, pdMS_TO_TICKS(100));
        if (guard.hasLock()) {
            otaUpdateInProgress = false;
        } else {
            LOG_ERROR(TAG, "Failed to acquire OTA status mutex on end");
        }
    }

    // Resume any suspended operations
    // SensorTask::resume();

    // Indicate OTA complete with LED
    // StatusLed::setPattern(2, 500, 1000);  // Double blink

    LOG_INFO(TAG, "OTA update successful - system will restart");
}

void OTATask::onOTAProgress(unsigned int progress, unsigned int total) {
    static unsigned int lastPercent = 0;
    unsigned int percent = (progress * 100) / total;

    // Only log at 10% intervals to reduce spam
    if (percent != lastPercent && percent % 10 == 0) {
        LOG_INFO(TAG, "OTA progress: %u%%", percent);
        lastPercent = percent;
    }

    // Could update LED brightness or pattern based on progress
    // StatusLed::setBrightness(percent);
}

void OTATask::onOTAError(ota_error_t error) {
    const char* errorStr = "Unknown error";
    switch (error) {
        case OTA_AUTH_ERROR: errorStr = "Authentication failed"; break;
        case OTA_BEGIN_ERROR: errorStr = "Begin failed"; break;
        case OTA_CONNECT_ERROR: errorStr = "Connect failed"; break;
        case OTA_RECEIVE_ERROR: errorStr = "Receive failed"; break;
        case OTA_END_ERROR: errorStr = "End failed"; break;
        default: break;
    }

    LOG_ERROR(TAG, "OTA error: %s (code: %d)", errorStr, error);

    {
        SemaphoreGuard guard(otaStatusMutex, pdMS_TO_TICKS(100));
        if (guard.hasLock()) {
            otaUpdateInProgress = false;
        } else {
            LOG_ERROR(TAG, "Failed to acquire OTA status mutex on error");
        }
    }

    // Resume any suspended operations
    // SensorTask::resume();

    // Indicate error with LED pattern - 5 quick blinks
    // StatusLed::setPattern(5, 100, 1500);
}

// Network check callback
bool OTATask::isNetworkConnected() {
    return EthernetManager::isConnected();
}
