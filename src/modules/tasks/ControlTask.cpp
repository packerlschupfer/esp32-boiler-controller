#include "modules/tasks/ControlTask.h"
#include <freertos/FreeRTOS.h>
#include "freertos/event_groups.h"
#include "config/SystemSettings.h"
#include "shared/SharedResources.h"
#include "events/SystemEventsGenerated.h"
#include "core/SystemResourceProvider.h"
#include "core/StateManager.h"
#include <TaskManager.h>
#include <RuntimeStorage.h>

void ControlTask(void* parameter) {
    const char* TAG = "ControlTask";
    EventBits_t bits;
    
    // Register with watchdog
    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::enabled(
        false,  // not critical - handles remote control only
        SystemConstants::System::WDT_CONTROL_TASK_MS
    );

    if (!SRP::getTaskManager().registerCurrentTaskWithWatchdog("ControlTask", wdtConfig)) {
        LOG_ERROR(TAG, "Failed to register with watchdog");
    } else {
        LOG_INFO(TAG, "WDT OK %lums", SystemConstants::System::WDT_CONTROL_TASK_MS);
    }

    // Add initial delay to let system stabilize
    vTaskDelay(pdMS_TO_TICKS(2000));
    (void)SRP::getTaskManager().feedWatchdog();
    
    LOG_INFO(TAG, "ControlTask entering main loop");
    uint32_t loopCount = 0;
    
    while (1) {
        // Feed watchdog at start of loop
        (void)SRP::getTaskManager().feedWatchdog();
        
        bits = SRP::waitControlRequestsEventBits(
                                   SystemEvents::ControlRequest::BOILER_ENABLE |
                                   SystemEvents::ControlRequest::BOILER_DISABLE |
                                   SystemEvents::ControlRequest::WATER_ENABLE |
                                   SystemEvents::ControlRequest::WATER_DISABLE |
                                   SystemEvents::ControlRequest::WATER_PRIORITY_ENABLE |
                                   SystemEvents::ControlRequest::WATER_PRIORITY_DISABLE |
                                   SystemEvents::ControlRequest::WATER_ON_OVERRIDE |
                                   SystemEvents::ControlRequest::WATER_OFF_OVERRIDE |
                                   SystemEvents::ControlRequest::HEATING_ENABLE |
                                   SystemEvents::ControlRequest::HEATING_DISABLE |
                                   SystemEvents::ControlRequest::HEATING_ON_OVERRIDE |
                                   SystemEvents::ControlRequest::HEATING_OFF_OVERRIDE,
                                   pdTRUE, 
                                   pdFALSE, 
                                   pdMS_TO_TICKS(3000)); // Reduced to 3 second timeout

        // Feed watchdog after waiting for events
        (void)SRP::getTaskManager().feedWatchdog();

        // Increment loop counter (watchdog already monitors task health)
        ++loopCount;

        // Handle boiler requests (via StateManager - atomic update of event bits + settings)
        if (bits & SystemEvents::ControlRequest::BOILER_ENABLE) {
            StateManager::setBoilerEnabled(true);
            // Log system enable event
            rtstorage::RuntimeStorage* storage = SRP::getRuntimeStorage();
            if (storage) {
                (void)storage->logEvent(rtstorage::EVENT_USER_ACTION, 0x0001); // System enabled
            }
        } else if (bits & SystemEvents::ControlRequest::BOILER_DISABLE) {
            StateManager::setBoilerEnabled(false);
            // Log system disable event
            rtstorage::RuntimeStorage* storage = SRP::getRuntimeStorage();
            if (storage) {
                (void)storage->logEvent(rtstorage::EVENT_USER_ACTION, 0x0000); // System disabled
            }
        }

        // Handle water heating requests
        if (bits & SystemEvents::ControlRequest::WATER_ENABLE) {
            StateManager::setWaterEnabled(true);
        } else if (bits & SystemEvents::ControlRequest::WATER_DISABLE) {
            StateManager::setWaterEnabled(false);
        }
        // Override bits are transient control (not persisted enable state)
        if (bits & SystemEvents::ControlRequest::WATER_ON_OVERRIDE) {
            SRP::setSystemStateEventBits(SystemEvents::SystemState::WATER_ON);
        } else if (bits & SystemEvents::ControlRequest::WATER_OFF_OVERRIDE) {
            SRP::clearSystemStateEventBits(SystemEvents::SystemState::WATER_ON);
        }

        // Handle heating requests
        if (bits & SystemEvents::ControlRequest::HEATING_ENABLE) {
            StateManager::setHeatingEnabled(true);
        } else if (bits & SystemEvents::ControlRequest::HEATING_DISABLE) {
            StateManager::setHeatingEnabled(false);
        }
        // Override bits are transient control (not persisted enable state)
        if (bits & SystemEvents::ControlRequest::HEATING_ON_OVERRIDE) {
            SRP::setSystemStateEventBits(SystemEvents::SystemState::HEATING_ON);
        } else if (bits & SystemEvents::ControlRequest::HEATING_OFF_OVERRIDE) {
            SRP::clearSystemStateEventBits(SystemEvents::SystemState::HEATING_ON);
        }

        // Handle water priority
        if (bits & SystemEvents::ControlRequest::WATER_PRIORITY_ENABLE) {
            StateManager::setWaterPriorityEnabled(true);
        } else if (bits & SystemEvents::ControlRequest::WATER_PRIORITY_DISABLE) {
            StateManager::setWaterPriorityEnabled(false);
        }

        // Feed watchdog at end of loop
        (void)SRP::getTaskManager().feedWatchdog();
        vTaskDelay(pdMS_TO_TICKS(100)); // Increased delay to reduce CPU usage
    }
}
