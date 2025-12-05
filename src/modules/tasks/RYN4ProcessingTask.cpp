// src/modules/tasks/RYN4ProcessingTask.cpp
#include "modules/tasks/RYN4ProcessingTask.h"
#include "RYN4.h"
#include "LoggingMacros.h"
#include "core/SystemResourceProvider.h"
#include <TaskManager.h>
#include "shared/RelayFunctionDefs.h"
#include "shared/SharedRelayReadings.h"
#include <IDeviceInstance.h>
#include "events/SystemEventsGenerated.h"

void RYN4ProcessingTask(void* parameter) {
    auto* ryn4 = static_cast<RYN4*>(parameter);
    const char* TAG = "RYN4Proc";
    
    if (!ryn4) {
        LOG_ERROR(TAG, "Started with null RYN4 instance");
        vTaskDelete(NULL);
        return;
    }
    
    LOG_INFO(TAG, "Started C%d Stk:%d", xPortGetCoreID(), uxTaskGetStackHighWaterMark(NULL) * 4);
    
    // Register with watchdog
    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::enabled(
        false,  // not critical
        SystemConstants::System::WDT_SENSOR_PROCESSING_MS
    );

    if (!SRP::getTaskManager().registerCurrentTaskWithWatchdog("RYN4Processing", wdtConfig)) {
        LOG_WARN(TAG, "Failed to register with watchdog");
    } else {
        LOG_INFO(TAG, "WDT OK %lums", SystemConstants::System::WDT_SENSOR_PROCESSING_MS);
    }
    
    // RYN4 handles queue internally
    LOG_INFO(TAG, "Waiting for device initialization...");
    vTaskDelay(pdMS_TO_TICKS(1000)); // Give device time to initialize
    
    LOG_INFO(TAG, "Entering main processing loop");
    
    // Main processing loop
    while (true) {
        // RYN4 handles packet processing internally
        vTaskDelay(pdMS_TO_TICKS(1000));

        // Feed watchdog
        (void)SRP::getTaskManager().feedWatchdog();
    }
}