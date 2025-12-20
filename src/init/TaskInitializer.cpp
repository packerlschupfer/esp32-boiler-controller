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
#include "modules/tasks/BoilerTempControlTask.h"
#include "modules/tasks/PersistentStorageTask.h"

// Pump control (independent from burner)
#include "modules/control/PumpControlModule.h"

// Core services
#include "core/SystemResourceProvider.h"

// Error handling
#include "utils/ErrorHandler.h"

// Config
#include "config/ProjectConfig.h"


static const char* TAG = "TaskInitializer";
Result<void> TaskInitializer::initializeTasks(SystemInitializer* initializer) {
    LOG_INFO(TAG, "Initializing tasks...");
    LOG_DEBUG(TAG, "Starting task initialization at %lu ms", millis());

    /*
     * WATCHDOG CONFIGURATION (FMEA Round 6):
     *
     * Critical tasks (trigger system reset on timeout):
     * - BurnerControl: 15s (WDT_BURNER_CONTROL_MS) - gas burner safety
     * - RelayControl: 10s (WDT_RELAY_CONTROL_MS) - relay state safety
     *
     * Non-critical tasks (log warning only):
     * - MB8ART/MB8ARTProc: 30s (WDT_SENSOR_PROCESSING_MS) - sensor reads
     *
     * Tasks without watchdog (low priority):
     * - OTA, MQTT, Monitoring, etc.
     */

    // Initialize relay control task
    initializeRelayControlTask(initializer);

    // Initialize OTA task
    initializeOTATask();

    // ModbusCoordinator is started earlier in ModbusDeviceInitializer
    // to allow sensor reads to begin while network/other init continues

    // Initialize sensor tasks
    initializeSensorTasks(initializer);

    // Initialize control tasks
    initializeControlTasks(initializer);

    // MQTT task initialization
    #ifdef ENABLE_MQTT
    if (MQTTTask::init()) {
        if (MQTTTask::start()) {
            LOG_INFO(TAG, "MQTT task started successfully");
        } else {
            LOG_WARN(TAG, "Failed to start MQTT task");
        }
    } else {
        LOG_WARN(TAG, "Failed to initialize MQTT task");
    }
    #endif

    // Persistent Storage task
    LOG_INFO(TAG, "Starting persistent storage task...");
    LOG_INFO(TAG, "Free heap before persistent storage: %d bytes", ESP.getFreeHeap());

    TaskManager::WatchdogConfig storageWdtConfig = TaskManager::WatchdogConfig::disabled();

    if (SRP::getTaskManager().startTask(
        PersistentStorageTask,
        "PersistentStorage",
        STACK_SIZE_PERSISTENT_STORAGE_TASK,
        nullptr,
        PRIORITY_CONTROL_TASK - 1,
        storageWdtConfig)) {
        LOG_INFO(TAG, "Persistent storage task created successfully");
    } else {
        LOG_WARN(TAG, "Failed to create persistent storage task");
    }

    // Fallback: Ensure BurnerControlTask is created
    TaskHandle_t existingBurnerTask = SRP::getTaskManager().getTaskHandleByName("BurnerControl");
    if (existingBurnerTask == nullptr && initializer->burnerSystemController_ != nullptr) {
        LOG_WARN(TAG, "BurnerControlTask not created by background task - creating now as fallback");
        auto burnerResult = initializeBurnerControlTask(initializer);
        if (burnerResult.isError()) {
            LOG_ERROR(TAG, "Failed to create BurnerControlTask in fallback: %s",
                      ErrorHandler::errorToString(burnerResult.error()));
        } else {
            LOG_INFO(TAG, "BurnerControlTask created successfully via fallback");
        }
    } else if (existingBurnerTask != nullptr) {
        LOG_INFO(TAG, "BurnerControlTask already exists - skipping fallback creation");
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
            LOG_ERROR(TAG, "CRITICAL: Task '%s' not running after initialization!", taskName);
            criticalTasksOk = false;
        } else {
            eTaskState state = eTaskGetState(handle);
            if (state == eDeleted || state == eInvalid) {
                LOG_ERROR(TAG, "CRITICAL: Task '%s' has invalid state %d!", taskName, (int)state);
                criticalTasksOk = false;
            } else {
                LOG_DEBUG(TAG, "Task '%s' verified running (state=%d)", taskName, (int)state);
            }
        }
    }

    if (!criticalTasksOk) {
        LOG_ERROR(TAG, "One or more critical tasks failed to start - system may be unstable!");
        // Don't return error - let system continue in degraded mode
        // CentralizedFailsafe will handle individual component failures
    }

    LOG_INFO(TAG, "Task initialization complete");
    return Result<void>();
}

void TaskInitializer::initializeRelayControlTask(SystemInitializer* initializer) {
    if (initializer->ryn4_ != nullptr) {
        LOG_DEBUG(TAG, "About to start relay control task at %lu ms", millis());
        LOG_INFO(TAG, "Starting relay control task...");
        LOG_INFO(TAG, "RYN4 pointer: %p, initialized: %s",
                 (void*)initializer->ryn4_, initializer->ryn4_->isInitialized() ? "YES" : "NO");

        if (!RelayControlTask::init(initializer->ryn4_)) {
            LOG_ERROR(TAG, "Failed to initialize relay control task - init() returned false");
        } else if (!RelayControlTask::start()) {
            LOG_ERROR(TAG, "Failed to start relay control task - start() returned false");
        } else {
            LOG_INFO(TAG, "Relay control task started successfully");
        }
    } else {
        LOG_ERROR(TAG, "Cannot start relay control task - ryn4_ is NULL");
    }
}

void TaskInitializer::initializeOTATask() {
    LOG_DEBUG(TAG, "About to start OTA task at %lu ms", millis());
    LOG_INFO(TAG, "Starting OTA task...");
    if (!OTATask::init()) {
        LOG_WARN(TAG, "Failed to initialize OTA task");
    } else if (!OTATask::start()) {
        LOG_WARN(TAG, "Failed to start OTA task");
    } else {
        LOG_INFO(TAG, "OTA task started successfully");
    }
    LOG_DEBUG(TAG, "OTA task done at %lu ms", millis());
}

void TaskInitializer::initializeSensorTasks(SystemInitializer* initializer) {
    // Initialize ANDRTF3 task
    if (initializer->andrtf3_ != nullptr) {
        LOG_INFO(TAG, "Starting ANDRTF3 room temperature sensor task...");
        TaskManager::WatchdogConfig andrtf3WdtConfig = TaskManager::WatchdogConfig::disabled();

        if (SRP::getTaskManager().startTaskPinned(
            ANDRTF3Task,
            "ANDRTF3",
            STACK_SIZE_SENSOR_TASK,
            nullptr,
            PRIORITY_SENSOR_TASK,
            1,
            andrtf3WdtConfig)) {
            LOG_INFO(TAG, "ANDRTF3 task started successfully");
        } else {
            LOG_WARN(TAG, "Failed to start ANDRTF3 task - inside temperature unavailable");
        }
    } else {
        LOG_INFO(TAG, "Skipping ANDRTF3 task - device not available");
    }

    // Note: SpaceHeatingPIDTask removed - PID control replaced by:
    // - Heating curve (HeatingControlModule::calculateSpaceHeatingTargetTemp)
    // - Bang-bang power control (BoilerTempControlTask)
}

void TaskInitializer::initializeControlTasks(SystemInitializer* initializer) {
    // Main control task
    LOG_INFO(TAG, "Starting main control task...");

    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::disabled();

    if (SRP::getTaskManager().startTask(
        ControlTask,
        "ControlTask",
        STACK_SIZE_CONTROL_TASK,
        nullptr,
        PRIORITY_CONTROL_TASK,
        wdtConfig)) {
        LOG_INFO(TAG, "Control task created successfully");
    } else {
        LOG_WARN(TAG, "Failed to create control task");
    }

    // Check device readiness but don't block - tasks will wait internally for devices
    // Blocking here prevents RYN4 background initialization from completing
    EventBits_t deviceBits = xEventGroupGetBits(initializer->deviceReadyEventGroup_);
    bool devicesReady = (deviceBits & SystemEvents::DeviceReady::ALL_CRITICAL_READY) ==
                        SystemEvents::DeviceReady::ALL_CRITICAL_READY;

    if (devicesReady) {
        LOG_INFO(TAG, "Essential devices ready - starting control tasks");
    } else {
        LOG_WARN(TAG, "Devices not yet ready (MB8ART:%d RYN4:%d) - tasks will wait internally",
                 (deviceBits & SystemEvents::DeviceReady::MB8ART_READY) ? 1 : 0,
                 (deviceBits & SystemEvents::DeviceReady::RYN4_READY) ? 1 : 0);
        LOG_INFO(TAG, "Starting control tasks - they will defer operation until devices ready");
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
        LOG_INFO(TAG, "Starting burner control task...");
        auto result = initializeBurnerControlTask(initializer);
        if (result.isError()) {
            LOG_ERROR(TAG, "Failed to initialize burner control task: %s",
                     ErrorHandler::errorToString(result.error()));
        } else {
            LOG_INFO(TAG, "Burner control task initialized successfully");
        }
    }

    // Initialize BoilerTempControlTask - cascade control inner loop
    // This task determines power levels (OFF/HALF/FULL) based on boiler temperature
    LOG_INFO(TAG, "Starting boiler temperature control task...");
    initializeBoilerTempControlTask(initializer);
}

void TaskInitializer::initializePumpTasks(SystemInitializer* initializer) {
    // Pump control tasks restored - independent from burner control
    // PumpControlModule watches HEATING_ON/WATER_ON event bits and controls pump relays
    // This allows pumps to run while burner is off (coasting for heat distribution)

    LOG_INFO(TAG, "=== Pump Control Tasks Initialization ===");
    LOG_INFO(TAG, "Free heap before pump tasks: %d bytes", ESP.getFreeHeap());

    // Stack size for pump tasks (lightweight event-driven tasks)
    constexpr uint32_t STACK_SIZE_PUMP_TASK = 2048;
    constexpr uint8_t PRIORITY_PUMP_TASK = 3;  // Same as control tasks

    // Heating pump task
    TaskManager::WatchdogConfig pumpWdtConfig = TaskManager::WatchdogConfig::enabled(
        true,   // Critical - pumps are important for heat distribution
        10000   // 10 second timeout
    );

    LOG_INFO(TAG, "Starting HeatingPump task...");
    if (SRP::getTaskManager().startTask(
        PumpControlModule::HeatingPumpTask,
        "HeatingPump",
        STACK_SIZE_PUMP_TASK,
        nullptr,
        PRIORITY_PUMP_TASK,
        pumpWdtConfig)) {

        TaskHandle_t handle = SRP::getTaskManager().getTaskHandleByName("HeatingPump");
        if (handle) {
            initializer->registerTask(handle, "HeatingPump");
            LOG_INFO(TAG, "HeatingPump task created successfully");
        }
    } else {
        LOG_ERROR(TAG, "Failed to create HeatingPump task!");
    }

    // Water pump task
    LOG_INFO(TAG, "Starting WaterPump task...");
    if (SRP::getTaskManager().startTask(
        PumpControlModule::WaterPumpTask,
        "WaterPump",
        STACK_SIZE_PUMP_TASK,
        nullptr,
        PRIORITY_PUMP_TASK,
        pumpWdtConfig)) {

        TaskHandle_t handle = SRP::getTaskManager().getTaskHandleByName("WaterPump");
        if (handle) {
            initializer->registerTask(handle, "WaterPump");
            LOG_INFO(TAG, "WaterPump task created successfully");
        }
    } else {
        LOG_ERROR(TAG, "Failed to create WaterPump task!");
    }

    LOG_INFO(TAG, "=== Pump Tasks Initialization Complete ===");
    LOG_INFO(TAG, "Free heap after pump tasks: %d bytes", ESP.getFreeHeap());
}

void TaskInitializer::initializeMonitoringTask() {
    #if ENABLE_MONITORING_TASK
    LOG_INFO(TAG, "Starting monitoring task...");
    if (!MonitoringTask::init()) {
        LOG_WARN(TAG, "Failed to initialize monitoring task");
    } else if (!MonitoringTask::start()) {
        LOG_WARN(TAG, "Failed to start monitoring task");
    } else {
        LOG_INFO(TAG, "Monitoring task started successfully");
    }
    #endif
}

void TaskInitializer::createMB8ARTTasks(SystemInitializer* initializer) {
    if (initializer->mb8art_ == nullptr) {
        LOG_ERROR(TAG, "Cannot create MB8ART tasks - device is null");
        return;
    }

    LOG_INFO(TAG, "Creating MB8ART tasks...");

    // Create MB8ART processing task
    // FMEA Round 6: Enable watchdog for sensor processing (non-critical, logs only)
    TaskManager::WatchdogConfig mb8artProcWdtConfig = TaskManager::WatchdogConfig::enabled(
        false,  // Non-critical - logs warning but doesn't reset
        SystemConstants::System::WDT_SENSOR_PROCESSING_MS  // 30s timeout
    );

    if (SRP::getTaskManager().startTaskPinned(
        MB8ARTProcessingTask,
        "MB8ARTProc",
        STACK_SIZE_MB8ART_PROCESSING_TASK,
        initializer->mb8art_,
        PRIORITY_MB8ART_PROCESSING_TASK,
        1,
        mb8artProcWdtConfig)) {
        LOG_INFO(TAG, "MB8ART processing task created successfully on core 1 (WDT: %lums)",
                 SystemConstants::System::WDT_SENSOR_PROCESSING_MS);
    } else {
        LOG_ERROR(TAG, "Failed to create MB8ART processing task");
        return;
    }

    // Create MB8ART data acquisition task
    // FMEA Round 6: Enable watchdog for sensor acquisition (non-critical, logs only)
    TaskManager::WatchdogConfig mb8artWdtConfig = TaskManager::WatchdogConfig::enabled(
        false,  // Non-critical - logs warning but doesn't reset
        SystemConstants::System::WDT_SENSOR_PROCESSING_MS  // 30s timeout
    );

    if (SRP::getTaskManager().startTaskPinned(
        MB8ARTTask,
        "MB8ART",
        STACK_SIZE_MODBUS_CONTROL_TASK,
        initializer->mb8art_,
        PRIORITY_MODBUS_CONTROL_TASK,
        1,
        mb8artWdtConfig)) {
        LOG_INFO(TAG, "MB8ART data acquisition task created successfully on core 1 (WDT: %lums)",
                 SystemConstants::System::WDT_SENSOR_PROCESSING_MS);
    } else {
        LOG_ERROR(TAG, "Failed to create MB8ART data acquisition task");
    }
}

void TaskInitializer::createHeatingControlTask(SystemInitializer* initializer) {
    LOG_INFO(TAG, "createHeatingControlTask() called");

    if (initializer->heatingControl_ == nullptr) {
        LOG_ERROR(TAG, "Cannot create heating control task - heatingControl_ is NULL!");
        LOG_ERROR(TAG, "Check initialization order in SystemInitializer::initializeControlModules()");
        return;
    }

    // Check if task already exists
    TaskHandle_t existingTask = SRP::getTaskManager().getTaskHandleByName("HeatingControl");
    if (existingTask != nullptr) {
        LOG_INFO(TAG, "Heating control task already exists at handle %p", existingTask);
        return;
    }

    LOG_INFO(TAG, "Creating heating control task with stack size %d...", STACK_SIZE_CONTROL_TASK);

    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::disabled();

    if (SRP::getTaskManager().startTask(
        HeatingControlTask,
        "HeatingControl",
        STACK_SIZE_CONTROL_TASK,
        nullptr,
        PRIORITY_CONTROL_TASK,
        wdtConfig)) {
        LOG_INFO(TAG, "Heating control task created successfully");
    } else {
        LOG_ERROR(TAG, "Failed to create heating control task");
    }
}

void TaskInitializer::createWaterControlTask(SystemInitializer* initializer) {
    LOG_INFO(TAG, "createWaterControlTask() called");

    // Check if task already exists
    TaskHandle_t existingTask = SRP::getTaskManager().getTaskHandleByName("WheaterControl");
    if (existingTask != nullptr) {
        LOG_INFO(TAG, "Water control task already exists at handle %p", existingTask);
        return;
    }

    LOG_INFO(TAG, "Creating water control task with stack size %d...", STACK_SIZE_WHEATER_CONTROL_TASK);

    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::disabled();

    // Pass BurnerSystemController as parameter (Pattern B: parameter passing)
    if (SRP::getTaskManager().startTask(
        WheaterControlTask,
        "WheaterControl",
        STACK_SIZE_WHEATER_CONTROL_TASK,
        initializer->burnerSystemController_,  // Pass controller to task
        PRIORITY_CONTROL_TASK,
        wdtConfig)) {
        LOG_INFO(TAG, "Water control task created successfully");
    } else {
        LOG_ERROR(TAG, "Failed to create water control task");
    }
}

void TaskInitializer::createBurnerControlTask(SystemInitializer* initializer) {
    LOG_INFO(TAG, "createBurnerControlTask() called");

    if (initializer->burnerSystemController_ == nullptr) {
        LOG_ERROR(TAG, "Cannot create burner control task - burnerSystemController_ is NULL!");
        LOG_ERROR(TAG, "Check initialization order in SystemInitializer::initializeControlModules()");
        return;
    }

    // Check if task already exists
    TaskHandle_t existingTask = SRP::getTaskManager().getTaskHandleByName("BurnerControl");
    if (existingTask != nullptr) {
        LOG_INFO(TAG, "Burner control task already exists at handle %p", existingTask);
        return;
    }

    LOG_INFO(TAG, "Creating burner control task...");

    auto result = initializeBurnerControlTask(initializer);
    if (result.isError()) {
        LOG_ERROR(TAG, "Failed to create burner control task: %s",
                 ErrorHandler::errorToString(result.error()));
    }
}

Result<void> TaskInitializer::initializeBurnerControlTask(SystemInitializer* initializer) {
    LOG_INFO(TAG, "=== BurnerControlTask Initialization Started ===");
    LOG_INFO(TAG, "Free heap before task creation: %d bytes", ESP.getFreeHeap());

    // Check if task already exists
    TaskHandle_t existingTask = SRP::getTaskManager().getTaskHandleByName("BurnerControl");
    if (existingTask != nullptr) {
        LOG_INFO(TAG, "BurnerControl task already exists at handle %p - skipping creation", existingTask);
        return Result<void>();
    }

    if (initializer->burnerSystemController_ == nullptr) {
        LOG_ERROR(TAG, "BurnerSystemController is NULL - cannot create task!");
        return Result<void>(SystemError::INVALID_PARAMETER, "BurnerSystemController not initialized");
    }

    LOG_INFO(TAG, "BurnerSystemController pointer valid: %p", initializer->burnerSystemController_);
    LOG_INFO(TAG, "Stack size: %d bytes, Priority: %d, Core: 1",
             STACK_SIZE_BURNER_CONTROL_TASK, PRIORITY_BURNER_CONTROL_TASK);

    // FMEA Round 6: Enable watchdog for safety-critical burner control
    // Critical task - will trigger system reset if watchdog times out
    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::enabled(
        true,  // Critical - triggers system reset
        SystemConstants::System::WDT_BURNER_CONTROL_MS  // 15s timeout
    );

    LOG_INFO(TAG, "Calling startTaskPinned for BurnerControlTask (WDT: %lums)...",
             SystemConstants::System::WDT_BURNER_CONTROL_MS);

    // BurnerControlTask uses BurnerSystemController via SRP, no direct parameter needed
    if (!SRP::getTaskManager().startTaskPinned(
        BurnerControlTask,
        "BurnerControl",
        STACK_SIZE_BURNER_CONTROL_TASK,
        nullptr,  // Task accesses controller via SRP::getBurnerSystemController()
        PRIORITY_BURNER_CONTROL_TASK,  // Safety-critical: priority 4
        1,
        wdtConfig)) {
        LOG_ERROR(TAG, "startTaskPinned FAILED for BurnerControlTask!");
        LOG_ERROR(TAG, "Free heap after failure: %d bytes", ESP.getFreeHeap());
        return Result<void>(SystemError::TASK_CREATE_FAILED, "Failed to start BurnerControlTask");
    }

    LOG_INFO(TAG, "startTaskPinned succeeded, getting task handle...");

    TaskHandle_t taskHandle = SRP::getTaskManager().getTaskHandleByName("BurnerControl");
    if (taskHandle == nullptr) {
        LOG_ERROR(TAG, "Failed to get BurnerControlTask handle after creation!");
        return Result<void>(SystemError::TASK_CREATE_FAILED, "Failed to get BurnerControlTask handle");
    }

    LOG_INFO(TAG, "Task handle obtained: %p", taskHandle);

    // Track the task handle for cleanup
    initializer->registerTask(taskHandle, "BurnerControl");
    LOG_INFO(TAG, "Task registered for cleanup");

    // Update global task handle through SRP
    SRP::getBurnerTaskHandle() = taskHandle;
    LOG_INFO(TAG, "Global task handle updated");

    LOG_INFO(TAG, "=== BurnerControlTask Initialization Complete ===");
    LOG_INFO(TAG, "Free heap after task creation: %d bytes", ESP.getFreeHeap());

    return Result<void>();
}

void TaskInitializer::initializeBoilerTempControlTask(SystemInitializer* initializer) {
    LOG_INFO(TAG, "=== BoilerTempControlTask Initialization Started ===");
    LOG_INFO(TAG, "Free heap before task creation: %d bytes", ESP.getFreeHeap());

    // Check if task already exists
    TaskHandle_t existingTask = SRP::getTaskManager().getTaskHandleByName("BoilerTempCtrl");
    if (existingTask != nullptr) {
        LOG_INFO(TAG, "BoilerTempCtrl task already exists - skipping creation");
        return;
    }

    // Stack size for boiler temp control task
    constexpr uint32_t STACK_SIZE_BOILER_TEMP_TASK = 3072;
    constexpr uint8_t PRIORITY_BOILER_TEMP_TASK = 4;  // Same as BurnerControlTask

    // Configure watchdog for non-critical monitoring
    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::enabled(
        false,  // non-critical (won't reset system on timeout)
        SystemConstants::BoilerControl::WDT_TIMEOUT_MS
    );

    LOG_INFO(TAG, "Creating BoilerTempControlTask (stack=%lu, priority=%d, WDT=%lums)...",
             STACK_SIZE_BOILER_TEMP_TASK, PRIORITY_BOILER_TEMP_TASK,
             SystemConstants::BoilerControl::WDT_TIMEOUT_MS);

    if (!SRP::getTaskManager().startTaskPinned(
        BoilerTempControlTask,
        "BoilerTempCtrl",
        STACK_SIZE_BOILER_TEMP_TASK,
        nullptr,  // No parameter needed
        PRIORITY_BOILER_TEMP_TASK,
        1,  // Core 1
        wdtConfig)) {
        LOG_ERROR(TAG, "Failed to create BoilerTempControlTask!");
        return;
    }

    TaskHandle_t taskHandle = SRP::getTaskManager().getTaskHandleByName("BoilerTempCtrl");
    if (taskHandle != nullptr) {
        initializer->registerTask(taskHandle, "BoilerTempCtrl");
        LOG_INFO(TAG, "BoilerTempControlTask created successfully (handle: %p)", taskHandle);
    } else {
        LOG_ERROR(TAG, "Failed to get BoilerTempControlTask handle");
    }

    LOG_INFO(TAG, "=== BoilerTempControlTask Initialization Complete ===");
    LOG_INFO(TAG, "Free heap after task creation: %d bytes", ESP.getFreeHeap());
}
