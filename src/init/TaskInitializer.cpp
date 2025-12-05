// src/init/TaskInitializer.cpp
#include "TaskInitializer.h"
#include "SystemInitializer.h"

#include <Arduino.h>
#include "LoggingMacros.h"
#include "events/SystemEventsGenerated.h"

#include <TaskManager.h>
#include <esp_log.h>

// Devices
#include <MB8ART.h>
#include <RYN4.h>
#include <ANDRTF3.h>

// Control modules
#include "modules/control/HeatingControlModule.h"
#include "modules/control/PIDControlModule.h"
#include "modules/control/PumpControlModule.h"

// Tasks
#include "modules/tasks/MonitoringTask.h"
#include "modules/tasks/MB8ARTTasks.h"
#include "modules/tasks/MB8ARTProcessingTask.h"
#include "modules/tasks/RelayControlTask.h"
#include "modules/tasks/OTATask.h"
#include "modules/tasks/MQTTTask.h"
#include "modules/tasks/ANDRTF3Task.h"
#include "modules/tasks/ControlTask.h"
#include "modules/tasks/HeatingControlTask.h"
#include "modules/tasks/WheaterControlTask.h"
#include "modules/tasks/PIDControlTask.h"
#include "modules/tasks/BurnerControlTask.h"
#include "modules/tasks/PersistentStorageTask.h"

// Core services
#include "core/SystemResourceProvider.h"
#include "core/ModbusCoordinator.h"

// Error handling
#include "utils/ErrorHandler.h"

// Config
#include "config/ProjectConfig.h"

Result<void> TaskInitializer::initializeTasks(SystemInitializer* initializer) {
    LOG_INFO(LOG_TAG_MAIN, "Initializing tasks...");
    LOG_DEBUG(LOG_TAG_MAIN, "Starting task initialization at %lu ms", millis());

    /*
     * WATCHDOG PATTERN DOCUMENTATION:
     *
     * All tasks MUST use TaskManager's startTask/startTaskPinned for proper management.
     * DO NOT use raw xTaskCreatePinnedToCore as it bypasses watchdog integration.
     *
     * Watchdog Configuration Pattern:
     * 1. All tasks with watchdog: Create with WatchdogConfig::disabled(), manually register
     * 2. Tasks without watchdog: Create with WatchdogConfig::disabled(), don't register
     */

    // Initialize relay control task
    initializeRelayControlTask(initializer);

    // Initialize OTA task
    initializeOTATask();

    // Start ModbusCoordinator
    LOG_INFO(LOG_TAG_MAIN, "Starting Modbus coordinator for sensor synchronization...");
    auto& modbusCoordinator = ModbusCoordinator::getInstance();
    if (!modbusCoordinator.start()) {
        LOG_ERROR(LOG_TAG_MAIN, "Failed to start ModbusCoordinator - sensors will run uncoordinated");
    } else {
        LOG_INFO(LOG_TAG_MAIN, "ModbusCoordinator started successfully");
    }

    // Initialize sensor tasks
    initializeSensorTasks(initializer);

    // Initialize control tasks
    initializeControlTasks(initializer);

    // MQTT task initialization
    #ifdef ENABLE_MQTT
    if (MQTTTask::init()) {
        if (MQTTTask::start()) {
            LOG_INFO(LOG_TAG_MAIN, "MQTT task started successfully");
        } else {
            LOG_WARN(LOG_TAG_MAIN, "Failed to start MQTT task");
        }
    } else {
        LOG_WARN(LOG_TAG_MAIN, "Failed to initialize MQTT task");
    }
    #endif

    // Persistent Storage task
    LOG_INFO(LOG_TAG_MAIN, "Starting persistent storage task...");
    LOG_INFO(LOG_TAG_MAIN, "Free heap before persistent storage: %d bytes", ESP.getFreeHeap());

    TaskManager::WatchdogConfig storageWdtConfig = TaskManager::WatchdogConfig::disabled();

    if (SRP::getTaskManager().startTask(
        PersistentStorageTask,
        "PersistentStorage",
        STACK_SIZE_PERSISTENT_STORAGE_TASK,
        nullptr,
        PRIORITY_CONTROL_TASK - 1,
        storageWdtConfig)) {
        LOG_INFO(LOG_TAG_MAIN, "Persistent storage task created successfully");
    } else {
        LOG_WARN(LOG_TAG_MAIN, "Failed to create persistent storage task");
    }

    // Fallback: Ensure BurnerControlTask is created
    TaskHandle_t existingBurnerTask = SRP::getTaskManager().getTaskHandleByName("BurnerControl");
    if (existingBurnerTask == nullptr && initializer->burnerSystemController_ != nullptr) {
        LOG_WARN(LOG_TAG_MAIN, "BurnerControlTask not created by background task - creating now as fallback");
        auto burnerResult = initializeBurnerControlTask(initializer);
        if (burnerResult.isError()) {
            LOG_ERROR(LOG_TAG_MAIN, "Failed to create BurnerControlTask in fallback: %s",
                      ErrorHandler::errorToString(burnerResult.error()));
        } else {
            LOG_INFO(LOG_TAG_MAIN, "BurnerControlTask created successfully via fallback");
        }
    } else if (existingBurnerTask != nullptr) {
        LOG_INFO(LOG_TAG_MAIN, "BurnerControlTask already exists - skipping fallback creation");
    }

    // Initialize pump control tasks
    initializePumpTasks(initializer);

    // Initialize monitoring task LAST
    initializeMonitoringTask();

    // Round 15 Issue #11: Verify critical tasks are running
    // This catches task creation failures that returned success but task didn't start
    vTaskDelay(pdMS_TO_TICKS(100));  // Brief delay to allow tasks to start

    bool criticalTasksOk = true;
    const char* criticalTasks[] = {
        "BurnerControl",    // Safety-critical: controls gas burner
        "RelayControl",     // Safety-critical: controls relays
        "MB8ART",           // Temperature sensors
        "MB8ARTProc"        // Temperature processing
    };

    for (const char* taskName : criticalTasks) {
        TaskHandle_t handle = SRP::getTaskManager().getTaskHandleByName(taskName);
        if (handle == nullptr) {
            LOG_ERROR(LOG_TAG_MAIN, "CRITICAL: Task '%s' not running after initialization!", taskName);
            criticalTasksOk = false;
        } else {
            eTaskState state = eTaskGetState(handle);
            if (state == eDeleted || state == eInvalid) {
                LOG_ERROR(LOG_TAG_MAIN, "CRITICAL: Task '%s' has invalid state %d!", taskName, (int)state);
                criticalTasksOk = false;
            } else {
                LOG_DEBUG(LOG_TAG_MAIN, "Task '%s' verified running (state=%d)", taskName, (int)state);
            }
        }
    }

    if (!criticalTasksOk) {
        LOG_ERROR(LOG_TAG_MAIN, "One or more critical tasks failed to start - system may be unstable!");
        // Don't return error - let system continue in degraded mode
        // CentralizedFailsafe will handle individual component failures
    }

    LOG_INFO(LOG_TAG_MAIN, "Task initialization complete");
    return Result<void>();
}

void TaskInitializer::initializeRelayControlTask(SystemInitializer* initializer) {
    if (initializer->ryn4_ != nullptr) {
        LOG_DEBUG(LOG_TAG_MAIN, "About to start relay control task at %lu ms", millis());
        LOG_INFO(LOG_TAG_MAIN, "Starting relay control task...");
        LOG_INFO(LOG_TAG_MAIN, "RYN4 pointer: %p, initialized: %s",
                 (void*)initializer->ryn4_, initializer->ryn4_->isInitialized() ? "YES" : "NO");

        if (!RelayControlTask::init(initializer->ryn4_)) {
            LOG_ERROR(LOG_TAG_MAIN, "Failed to initialize relay control task - init() returned false");
        } else if (!RelayControlTask::start()) {
            LOG_ERROR(LOG_TAG_MAIN, "Failed to start relay control task - start() returned false");
        } else {
            LOG_INFO(LOG_TAG_MAIN, "Relay control task started successfully");
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    } else {
        LOG_ERROR(LOG_TAG_MAIN, "Cannot start relay control task - ryn4_ is NULL");
    }
}

void TaskInitializer::initializeOTATask() {
    LOG_DEBUG(LOG_TAG_MAIN, "About to start OTA task at %lu ms", millis());
    LOG_INFO(LOG_TAG_MAIN, "Starting OTA task...");
    if (!OTATask::init()) {
        LOG_WARN(LOG_TAG_MAIN, "Failed to initialize OTA task");
    } else if (!OTATask::start()) {
        LOG_WARN(LOG_TAG_MAIN, "Failed to start OTA task");
    } else {
        LOG_INFO(LOG_TAG_MAIN, "OTA task started successfully");
    }
    LOG_DEBUG(LOG_TAG_MAIN, "OTA task done at %lu ms", millis());
}

void TaskInitializer::initializeSensorTasks(SystemInitializer* initializer) {
    // Initialize ANDRTF3 task
    if (initializer->andrtf3_ != nullptr) {
        LOG_INFO(LOG_TAG_MAIN, "Starting ANDRTF3 room temperature sensor task...");
        TaskManager::WatchdogConfig andrtf3WdtConfig = TaskManager::WatchdogConfig::disabled();

        if (SRP::getTaskManager().startTaskPinned(
            ANDRTF3Task,
            "ANDRTF3",
            STACK_SIZE_SENSOR_TASK,
            nullptr,
            PRIORITY_SENSOR_TASK,
            1,
            andrtf3WdtConfig)) {
            LOG_INFO(LOG_TAG_MAIN, "ANDRTF3 task started successfully");
        } else {
            LOG_WARN(LOG_TAG_MAIN, "Failed to start ANDRTF3 task - inside temperature unavailable");
        }
    } else {
        LOG_INFO(LOG_TAG_MAIN, "Skipping ANDRTF3 task - device not available");
    }

    // Start SpaceHeatingPIDTask after ANDRTF3
    if (initializer->heatingControl_ != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(200));
        LOG_INFO(LOG_TAG_MAIN, "Starting SpaceHeatingPID task...");
        initializer->heatingControl_->startSpaceHeatingPIDTask();
        LOG_INFO(LOG_TAG_MAIN, "SpaceHeatingPID task started");
    }
}

void TaskInitializer::initializeControlTasks(SystemInitializer* initializer) {
    // Main control task
    LOG_INFO(LOG_TAG_MAIN, "Starting main control task...");

    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::disabled();

    if (SRP::getTaskManager().startTask(
        ControlTask,
        "ControlTask",
        STACK_SIZE_CONTROL_TASK,
        nullptr,
        PRIORITY_CONTROL_TASK,
        wdtConfig)) {
        LOG_INFO(LOG_TAG_MAIN, "Control task created successfully");
    } else {
        LOG_WARN(LOG_TAG_MAIN, "Failed to create control task");
    }

    // Check device readiness but don't block - tasks will wait internally for devices
    // Blocking here prevents RYN4 background initialization from completing
    EventBits_t deviceBits = xEventGroupGetBits(initializer->deviceReadyEventGroup_);
    bool devicesReady = (deviceBits & SystemEvents::DeviceReady::ALL_CRITICAL_READY) ==
                        SystemEvents::DeviceReady::ALL_CRITICAL_READY;

    if (devicesReady) {
        LOG_INFO(LOG_TAG_MAIN, "Essential devices ready - starting control tasks");
    } else {
        LOG_WARN(LOG_TAG_MAIN, "Devices not yet ready (MB8ART:%d RYN4:%d) - tasks will wait internally",
                 (deviceBits & SystemEvents::DeviceReady::MB8ART_READY) ? 1 : 0,
                 (deviceBits & SystemEvents::DeviceReady::RYN4_READY) ? 1 : 0);
        LOG_INFO(LOG_TAG_MAIN, "Starting control tasks - they will defer operation until devices ready");
    }

    // Initialize HeatingControlTask (will wait for devices internally)
    if (initializer->heatingControl_ != nullptr) {
        createHeatingControlTask(initializer);
    }

    // Initialize WheaterControlTask (will wait for devices internally)
    // Note: WheaterControlTask uses BurnerSystemController, not WheaterControlModule
    createWaterControlTask(initializer);

    // Initialize BurnerControlTask (will wait for devices internally)
    // BurnerControlTask uses BurnerSystemController via SRP, no direct pointer needed
    if (initializer->burnerSystemController_ != nullptr) {
        LOG_INFO(LOG_TAG_MAIN, "Starting burner control task...");
        auto result = initializeBurnerControlTask(initializer);
        if (result.isError()) {
            LOG_ERROR(LOG_TAG_MAIN, "Failed to initialize burner control task: %s",
                     ErrorHandler::errorToString(result.error()));
        } else {
            LOG_INFO(LOG_TAG_MAIN, "Burner control task initialized successfully");
        }
    }
}

void TaskInitializer::initializePumpTasks(SystemInitializer* initializer) {
    (void)initializer;  // PumpControlModule uses static methods

    // Pump control tasks REMOVED - Round 18
    // Pumps are now controlled exclusively via BurnerSystemController batch commands
    // This ensures atomic pump+burner activation in a single Modbus transaction
    // Cooldown (pump running after burner stops) is handled by BurnerSystemController
    LOG_INFO(LOG_TAG_MAIN, "Pump control centralized in BurnerSystemController - no separate tasks");
}

void TaskInitializer::initializeMonitoringTask() {
    #if ENABLE_MONITORING_TASK
    LOG_INFO(LOG_TAG_MAIN, "Starting monitoring task...");
    if (!MonitoringTask::init()) {
        LOG_WARN(LOG_TAG_MAIN, "Failed to initialize monitoring task");
    } else if (!MonitoringTask::start()) {
        LOG_WARN(LOG_TAG_MAIN, "Failed to start monitoring task");
    } else {
        LOG_INFO(LOG_TAG_MAIN, "Monitoring task started successfully");
    }
    #endif
}

void TaskInitializer::createMB8ARTTasks(SystemInitializer* initializer) {
    if (initializer->mb8art_ == nullptr) {
        LOG_ERROR(LOG_TAG_MAIN, "Cannot create MB8ART tasks - device is null");
        return;
    }

    LOG_INFO(LOG_TAG_MAIN, "Creating MB8ART tasks...");

    // Create MB8ART processing task
    TaskManager::WatchdogConfig mb8artProcWdtConfig = TaskManager::WatchdogConfig::disabled();

    if (SRP::getTaskManager().startTaskPinned(
        MB8ARTProcessingTask,
        "MB8ARTProc",
        STACK_SIZE_MB8ART_PROCESSING_TASK,
        initializer->mb8art_,
        PRIORITY_MB8ART_PROCESSING_TASK,
        1,
        mb8artProcWdtConfig)) {
        LOG_INFO(LOG_TAG_MAIN, "MB8ART processing task created successfully on core 1");
    } else {
        LOG_ERROR(LOG_TAG_MAIN, "Failed to create MB8ART processing task");
        return;
    }

    // Create MB8ART data acquisition task
    TaskManager::WatchdogConfig mb8artWdtConfig = TaskManager::WatchdogConfig::disabled();

    if (SRP::getTaskManager().startTaskPinned(
        MB8ARTTask,
        "MB8ART",
        STACK_SIZE_MODBUS_CONTROL_TASK,
        initializer->mb8art_,
        PRIORITY_MODBUS_CONTROL_TASK,
        1,
        mb8artWdtConfig)) {
        LOG_INFO(LOG_TAG_MAIN, "MB8ART data acquisition task created successfully on core 1");
    } else {
        LOG_ERROR(LOG_TAG_MAIN, "Failed to create MB8ART data acquisition task");
    }
}

void TaskInitializer::createHeatingControlTask(SystemInitializer* initializer) {
    LOG_INFO(LOG_TAG_MAIN, "createHeatingControlTask() called");

    if (initializer->heatingControl_ == nullptr) {
        LOG_ERROR(LOG_TAG_MAIN, "Cannot create heating control task - heatingControl_ is NULL!");
        LOG_ERROR(LOG_TAG_MAIN, "Check initialization order in SystemInitializer::initializeControlModules()");
        return;
    }

    // Check if task already exists
    TaskHandle_t existingTask = SRP::getTaskManager().getTaskHandleByName("HeatingControl");
    if (existingTask != nullptr) {
        LOG_INFO(LOG_TAG_MAIN, "Heating control task already exists at handle %p", existingTask);
        return;
    }

    LOG_INFO(LOG_TAG_MAIN, "Creating heating control task with stack size %d...", STACK_SIZE_CONTROL_TASK);

    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::disabled();

    if (SRP::getTaskManager().startTask(
        HeatingControlTask,
        "HeatingControl",
        STACK_SIZE_CONTROL_TASK,
        nullptr,
        PRIORITY_CONTROL_TASK,
        wdtConfig)) {
        LOG_INFO(LOG_TAG_MAIN, "Heating control task created successfully");
    } else {
        LOG_ERROR(LOG_TAG_MAIN, "Failed to create heating control task");
    }
}

void TaskInitializer::createWaterControlTask(SystemInitializer* initializer) {
    LOG_INFO(LOG_TAG_MAIN, "createWaterControlTask() called");

    // Check if task already exists
    TaskHandle_t existingTask = SRP::getTaskManager().getTaskHandleByName("WheaterControl");
    if (existingTask != nullptr) {
        LOG_INFO(LOG_TAG_MAIN, "Water control task already exists at handle %p", existingTask);
        return;
    }

    LOG_INFO(LOG_TAG_MAIN, "Creating water control task with stack size %d...", STACK_SIZE_WHEATER_CONTROL_TASK);

    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::disabled();

    // Pass BurnerSystemController as parameter (Pattern B: parameter passing)
    if (SRP::getTaskManager().startTask(
        WheaterControlTask,
        "WheaterControl",
        STACK_SIZE_WHEATER_CONTROL_TASK,
        initializer->burnerSystemController_,  // Pass controller to task
        PRIORITY_CONTROL_TASK,
        wdtConfig)) {
        LOG_INFO(LOG_TAG_MAIN, "Water control task created successfully");
    } else {
        LOG_ERROR(LOG_TAG_MAIN, "Failed to create water control task");
    }
}

void TaskInitializer::createBurnerControlTask(SystemInitializer* initializer) {
    LOG_INFO(LOG_TAG_MAIN, "createBurnerControlTask() called");

    if (initializer->burnerSystemController_ == nullptr) {
        LOG_ERROR(LOG_TAG_MAIN, "Cannot create burner control task - burnerSystemController_ is NULL!");
        LOG_ERROR(LOG_TAG_MAIN, "Check initialization order in SystemInitializer::initializeControlModules()");
        return;
    }

    // Check if task already exists
    TaskHandle_t existingTask = SRP::getTaskManager().getTaskHandleByName("BurnerControl");
    if (existingTask != nullptr) {
        LOG_INFO(LOG_TAG_MAIN, "Burner control task already exists at handle %p", existingTask);
        return;
    }

    LOG_INFO(LOG_TAG_MAIN, "Creating burner control task...");

    auto result = initializeBurnerControlTask(initializer);
    if (result.isError()) {
        LOG_ERROR(LOG_TAG_MAIN, "Failed to create burner control task: %s",
                 ErrorHandler::errorToString(result.error()));
    }
}

Result<void> TaskInitializer::initializeBurnerControlTask(SystemInitializer* initializer) {
    LOG_INFO(LOG_TAG_MAIN, "=== BurnerControlTask Initialization Started ===");
    LOG_INFO(LOG_TAG_MAIN, "Free heap before task creation: %d bytes", ESP.getFreeHeap());

    // Check if task already exists
    TaskHandle_t existingTask = SRP::getTaskManager().getTaskHandleByName("BurnerControl");
    if (existingTask != nullptr) {
        LOG_INFO(LOG_TAG_MAIN, "BurnerControl task already exists at handle %p - skipping creation", existingTask);
        return Result<void>();
    }

    if (initializer->burnerSystemController_ == nullptr) {
        LOG_ERROR(LOG_TAG_MAIN, "BurnerSystemController is NULL - cannot create task!");
        return Result<void>(SystemError::INVALID_PARAMETER, "BurnerSystemController not initialized");
    }

    LOG_INFO(LOG_TAG_MAIN, "BurnerSystemController pointer valid: %p", initializer->burnerSystemController_);
    LOG_INFO(LOG_TAG_MAIN, "Stack size: %d bytes, Priority: %d, Core: 1",
             STACK_SIZE_BURNER_CONTROL_TASK, PRIORITY_BURNER_CONTROL_TASK);

    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::disabled();

    LOG_INFO(LOG_TAG_MAIN, "Calling startTaskPinned for BurnerControlTask...");

    // BurnerControlTask uses BurnerSystemController via SRP, no direct parameter needed
    if (!SRP::getTaskManager().startTaskPinned(
        BurnerControlTask,
        "BurnerControl",
        STACK_SIZE_BURNER_CONTROL_TASK,
        nullptr,  // Task accesses controller via SRP::getBurnerSystemController()
        PRIORITY_BURNER_CONTROL_TASK,  // Safety-critical: priority 4
        1,
        wdtConfig)) {
        LOG_ERROR(LOG_TAG_MAIN, "startTaskPinned FAILED for BurnerControlTask!");
        LOG_ERROR(LOG_TAG_MAIN, "Free heap after failure: %d bytes", ESP.getFreeHeap());
        return Result<void>(SystemError::TASK_CREATE_FAILED, "Failed to start BurnerControlTask");
    }

    LOG_INFO(LOG_TAG_MAIN, "startTaskPinned succeeded, getting task handle...");

    TaskHandle_t taskHandle = SRP::getTaskManager().getTaskHandleByName("BurnerControl");
    if (taskHandle == nullptr) {
        LOG_ERROR(LOG_TAG_MAIN, "Failed to get BurnerControlTask handle after creation!");
        return Result<void>(SystemError::TASK_CREATE_FAILED, "Failed to get BurnerControlTask handle");
    }

    LOG_INFO(LOG_TAG_MAIN, "Task handle obtained: %p", taskHandle);

    // Track the task handle for cleanup
    initializer->registerTask(taskHandle, "BurnerControl");
    LOG_INFO(LOG_TAG_MAIN, "Task registered for cleanup");

    // Update global task handle through SRP
    SRP::getBurnerTaskHandle() = taskHandle;
    LOG_INFO(LOG_TAG_MAIN, "Global task handle updated");

    LOG_INFO(LOG_TAG_MAIN, "=== BurnerControlTask Initialization Complete ===");
    LOG_INFO(LOG_TAG_MAIN, "Free heap after task creation: %d bytes", ESP.getFreeHeap());

    return Result<void>();
}
