// src/modules/tasks/MB8ARTProcessingTask.cpp
#include "modules/tasks/MB8ARTProcessingTask.h"
#include "MB8ART.h"
#include "LoggingMacros.h"
#include "core/SystemResourceProvider.h"
#include <TaskManager.h>

static const char* TAG = "MB8ARTProcessingTask";

void MB8ARTProcessingTask(void* parameter) {
    auto* mb8art = static_cast<MB8ART*>(parameter);

    if (!mb8art) {
        LOG_ERROR(TAG, "Started with null MB8ART instance");
        vTaskDelete(NULL);
        return;
    }
    
    LOG_INFO(TAG, "Started C%d Stk:%d", xPortGetCoreID(), uxTaskGetStackHighWaterMark(NULL) * 4);
    
    // Register with watchdog
    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::enabled(
        false,  // not critical
        SystemConstants::System::WDT_SENSOR_PROCESSING_MS
    );

    if (!SRP::getTaskManager().registerCurrentTaskWithWatchdog("MB8ARTProc", wdtConfig)) {
        LOG_WARN(TAG, "Failed to register with watchdog");
    } else {
        LOG_INFO(TAG, "WDT OK %lums", SystemConstants::System::WDT_SENSOR_PROCESSING_MS);
    }
    
    // MB8ART v1.2+ handles queue internally
    // No need to check queue directly
    
    LOG_INFO(TAG, "Queue ready, entering main loop");
    
    // Main processing loop
    while (true) {
        // MB8ART v1.2+ handles packet processing internally
        // This task just needs to stay alive for the queue processing
        vTaskDelay(pdMS_TO_TICKS(1000));

        // Feed watchdog
        (void)SRP::getTaskManager().feedWatchdog();
    }
}