// src/modules/tasks/PIDControlTask.cpp
#include "modules/tasks/PIDControlTask.h"
#include "modules/control/PIDControlModule.h"
#include "LoggingMacros.h"
#include <TaskManager.h>
#include "core/SystemResourceProvider.h"

// Logger tag
static const char* TAG = "PIDControlTask";

// Static task handle for TaskManager
static TaskHandle_t taskHandle = nullptr;

bool PIDControlTask::startTask(const char* taskName, uint16_t stackSize, UBaseType_t priority) {
    // Use TaskManager for proper watchdog integration
    // Task doesn't register watchdog (skeleton task)
    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::disabled();
    
    bool result = SRP::getTaskManager().startTask(
        taskEntryPoint, // Entry point function
        taskName,       // Task name
        stackSize,      // Stack size
        nullptr,        // Parameters to pass to the task
        priority,       // Task priority
        wdtConfig
    );
    
    if (result) {
        // Get the task handle after creation
        taskHandle = SRP::getTaskManager().getTaskHandleByName(taskName);
    }

    if (result) {
        LOG_INFO(TAG, "Task %s started successfully.", taskName);
        return true;
    } else {
        LOG_ERROR(TAG, "Failed to start task %s.", taskName);
        return false;
    }
}

void PIDControlTask::taskEntryPoint(void* parameter) {
    LOG_INFO(TAG, "PIDControlTask started.");
    // Call the main loop
    taskMainLoop();
}

void PIDControlTask::taskMainLoop() {
    while (true) {
        // Your PID logic here, e.g., calculating adjustments
        LOG_DEBUG(TAG, "Running PID control logic...");

        // Add task delay to prevent high CPU usage
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
    