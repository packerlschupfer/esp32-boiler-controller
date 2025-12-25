// src/modules/tasks/OTATask.cpp
// OTA update task - handles over-the-air firmware updates

#include "OTATask.h"

#include <SemaphoreGuard.h>
#include <TaskManager.h>
#include <OTAManager.h>
#include <EthernetManager.h>
#include "utils/ErrorHandler.h"
#include "core/SystemResourceProvider.h"
#include "core/SharedResourceManager.h"
#include "events/SystemEventsGenerated.h"
#include "freertos/timers.h"
#include "config/ProjectConfig.h"
#include "config/SystemConstants.h"
#include "LoggingMacros.h"

static const char* TAG = "OTA";

// Event bits for OTA operations
enum OTAEventBits : uint32_t {
    OTA_EVENT_NETWORK_CONNECTED = (1 << 0),
    OTA_EVENT_NETWORK_DISCONNECTED = (1 << 1),
    OTA_EVENT_CHECK_UPDATE = (1 << 2),
    OTA_EVENT_UPDATE_STARTED = (1 << 3),
    OTA_EVENT_UPDATE_COMPLETED = (1 << 4),
    OTA_EVENT_UPDATE_ERROR = (1 << 5)
};

// Initialize static members
TaskHandle_t OTATask::taskHandle = nullptr;
bool OTATask::otaUpdateInProgress = false;
SemaphoreHandle_t OTATask::otaStatusMutex = nullptr;

// Static variables for event-driven operation
static EventGroupHandle_t otaEventGroup = nullptr;
static TimerHandle_t otaCheckTimer = nullptr;
static bool networkConnected = false;

// Use timer periods from SystemConstants
using namespace SystemConstants::Timing;

/**
 * @brief Timer callback for periodic OTA checks
 */
static void otaCheckTimerCallback(TimerHandle_t xTimer) {
    if (otaEventGroup) {
        xEventGroupSetBits(otaEventGroup, OTA_EVENT_CHECK_UPDATE);
    }
}

/**
 * @brief Network state change handler
 */
static void onNetworkStateChange() {
    bool isConnected = EthernetManager::isConnected();
    
    if (isConnected && !networkConnected) {
        // Network just connected
        networkConnected = true;
        if (otaEventGroup) {
            xEventGroupSetBits(otaEventGroup, OTA_EVENT_NETWORK_CONNECTED);
        }
        
        // Start the OTA check timer
        if (otaCheckTimer) {
            xTimerStart(otaCheckTimer, 0);
        }
    } else if (!isConnected && networkConnected) {
        // Network just disconnected
        networkConnected = false;
        if (otaEventGroup) {
            xEventGroupSetBits(otaEventGroup, OTA_EVENT_NETWORK_DISCONNECTED);
        }
        
        // Stop the OTA check timer
        if (otaCheckTimer) {
            xTimerStop(otaCheckTimer, 0);
        }
    }
}

// Extension functions for event-driven callbacks - reserved for future use
// static void notifyOTAEvent(uint32_t eventBit) {
//     if (otaEventGroup) {
//         xEventGroupSetBits(otaEventGroup, eventBit);
//     }
// }
// 
// static void handleOTATimerControl(bool start) {
//     if (otaCheckTimer) {
//         if (start && networkConnected) {
//             xTimerStart(otaCheckTimer, 0);
//         } else {
//             xTimerStop(otaCheckTimer, 0);
//         }
//     }
// }

void OTATask::taskFunction(void* pvParameters) {
    LOG_INFO(TAG, "OTA task starting");
    
    // Create event group for OTA events
    otaEventGroup = xEventGroupCreate();
    if (!otaEventGroup) {
        LOG_ERROR(TAG, "Failed to create OTA event group");
        vTaskDelete(nullptr);
        return;
    }
    
    // Create timer for periodic OTA checks
    otaCheckTimer = xTimerCreate(
        "OTACheck",
        pdMS_TO_TICKS(OTA_UPDATE_CHECK_INTERVAL_MS),
        pdTRUE,  // Auto-reload
        nullptr,
        otaCheckTimerCallback
    );
    
    if (!otaCheckTimer) {
        LOG_ERROR(TAG, "Failed to create OTA check timer");
        vEventGroupDelete(otaEventGroup);
        vTaskDelete(nullptr);
        return;
    }
    
    // No watchdog needed for event-driven task
    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::disabled();
    (void)SRP::getTaskManager().registerCurrentTaskWithWatchdog("OTATask", wdtConfig);
    
    // Check initial network state
    networkConnected = EthernetManager::isConnected();
    if (networkConnected) {
        LOG_INFO(TAG, "Network already connected, starting OTA checks");
        xTimerStart(otaCheckTimer, 0);
    }
    
    // Get system state event group for network events
    auto& resourceManager = SharedResourceManager::getInstance();
    EventGroupHandle_t systemStateEventGroup = resourceManager.getEventGroup(SharedResourceManager::EventGroups::SYSTEM_STATE);
    
    LOG_INFO(TAG, "OTA task running");
    
    const EventBits_t ALL_EVENTS = 
        OTA_EVENT_NETWORK_CONNECTED | OTA_EVENT_NETWORK_DISCONNECTED |
        OTA_EVENT_CHECK_UPDATE | OTA_EVENT_UPDATE_STARTED |
        OTA_EVENT_UPDATE_COMPLETED | OTA_EVENT_UPDATE_ERROR;
    
    // Main event loop
    while (true) {
        // Check system network state
        if (systemStateEventGroup) {
            EventBits_t sysBits = xEventGroupGetBits(systemStateEventGroup);
            if (sysBits & SystemEvents::GeneralSystem::NETWORK_READY) {
                onNetworkStateChange();
            }
        }
        
        // Wait for OTA events - 1000ms timeout is optimal for system health
        // Gives scheduler plenty of time to run Modbus tasks on Core 1
        // OTA uploads still work fine with 1s intervals (was 100ms which starved other tasks)
        EventBits_t events = xEventGroupWaitBits(
            otaEventGroup,
            ALL_EVENTS,
            pdTRUE,   // Clear bits on exit
            pdFALSE,  // Wait for any bit
            pdMS_TO_TICKS(1000)  // 1 second - OTA responsive, Modbus healthy
        );
        
        // Always check for network state changes
        onNetworkStateChange();
        
        // Handle network connected event
        if (events & OTA_EVENT_NETWORK_CONNECTED) {
            LOG_INFO(TAG, "Network connected - enabling OTA updates");
            // Timer already started in onNetworkStateChange
        }
        
        // Handle network disconnected event
        if (events & OTA_EVENT_NETWORK_DISCONNECTED) {
            LOG_INFO(TAG, "Network disconnected - OTA updates disabled");
            // Timer already stopped in onNetworkStateChange
        }

        // CRITICAL: Call handleUpdates() on EVERY loop iteration when network is connected
        // ArduinoOTA.handle() must be called frequently to process incoming OTA data
        if (networkConnected && !OTATask::otaUpdateInProgress) {
            OTAManager::handleUpdates();
        }

        // Handle OTA check event (for logging only now)
        if (events & OTA_EVENT_CHECK_UPDATE) {
            LOG_DEBUG(TAG, "OTA check timer fired - handle() called continuously");
        }
        
        // Handle update started event
        if (events & OTA_EVENT_UPDATE_STARTED) {
            LOG_INFO(TAG, "OTA update in progress");
            // Could update LED status here
        }
        
        // Handle update completed event
        if (events & OTA_EVENT_UPDATE_COMPLETED) {
            LOG_INFO(TAG, "OTA update finished successfully");
            // System will likely reboot soon
        }
        
        // Handle update error event
        if (events & OTA_EVENT_UPDATE_ERROR) {
            LOG_ERROR(TAG, "OTA update failed");
            // Could indicate error via LED
        }
    }
    
    // Cleanup (should never reach here)
    if (otaCheckTimer) {
        xTimerDelete(otaCheckTimer, 0);
    }
    if (otaEventGroup) {
        vEventGroupDelete(otaEventGroup);
    }
    vTaskDelete(nullptr);
}

// Initialization and start methods
bool OTATask::init() {
    LOG_INFO(TAG, "Initializing OTA task");
    
    // Create status mutex
    OTATask::otaStatusMutex = xSemaphoreCreateMutex();
    if (OTATask::otaStatusMutex == nullptr) {
        LOG_ERROR(TAG, "Failed to create status mutex");
        return false;
    }
    
    // Initialize OTA manager
    OTAManager::initialize(DEVICE_HOSTNAME,    // Use the project hostname
                          OTA_PASSWORD,       // OTA password from config
                          OTA_PORT,           // OTA port from config
                          isNetworkConnected  // Network check callback
    );
    
    // Set up callbacks
    OTAManager::setStartCallback(OTATask::onOTAStart);
    OTAManager::setProgressCallback(OTATask::onOTAProgress);
    OTAManager::setEndCallback(OTATask::onOTAEnd);
    OTAManager::setErrorCallback(OTATask::onOTAError);
    
    LOG_INFO(TAG, "OTA task initialized");
    return true;
}

bool OTATask::start() {
    LOG_INFO(TAG, "Starting OTA task");
    
    // Use TaskManager for proper watchdog integration
    // Task will manually register watchdog from its own context
    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::disabled();
    
    bool result = SRP::getTaskManager().startTaskPinned(
        taskFunction,
        "OTATask",
        STACK_SIZE_OTA_TASK,
        nullptr,
        PRIORITY_OTA_TASK,
        1,  // Pin to core 1
        wdtConfig
    );
    
    if (result) {
        // Get the task handle after creation
        OTATask::taskHandle = SRP::getTaskManager().getTaskHandleByName("OTATask");
    }
    
    if (!result) {
        LOG_ERROR(TAG, "Failed to create OTA task");
        return false;
    }
    
    LOG_INFO(TAG, "OTA task started");
    return true;
}