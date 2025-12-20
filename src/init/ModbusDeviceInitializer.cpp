// src/init/ModbusDeviceInitializer.cpp
#include "ModbusDeviceInitializer.h"
#include "SystemInitializer.h"
#include "TaskInitializer.h"

#include <Arduino.h>
#include "LoggingMacros.h"
#include "events/SystemEventsGenerated.h"

#include <TaskManager.h>
#include <MB8ART.h>
#include <RYN4.h>
#include <ANDRTF3.h>
#include <RuntimeStorage.h>
#include <esp_log.h>

// Error handling
#include "utils/ErrorHandler.h"
#include "utils/LibraryErrorMapper.h"
#include "utils/CriticalDataStorage.h"

// Core services
#include "core/SystemResourceProvider.h"
#include "core/ModbusCoordinator.h"

// Unified hardware mapping
#include "config/RelayHardwareConfig.h"
#include "config/SensorHardwareConfig.h"
#include "shared/RelayBindings.h"
#include "shared/SensorBindings.h"

// Tasks
#include "modules/tasks/MB8ARTTasks.h"
#include "modules/tasks/MB8ARTProcessingTask.h"
#include "modules/tasks/RYN4ProcessingTask.h"

// HAL
#include "hal/HardwareAbstractionLayer.h"
#include "shared/SharedI2CInitializer.h"
#include "DS3231Controller.h"

// Config
#include "config/ProjectConfig.h"

// ModbusInitRAII for RAII task parameter management
#include "ModbusInitRAII.h"


static const char* TAG = "ModbusDeviceInitializer";
// External globals
extern rtstorage::RuntimeStorage* gRuntimeStorage;

// HAL configuration function declarations
namespace HAL {
    extern bool configureHardwareAbstractionLayer(MB8ART* mb8art, RYN4* ryn4, DS3231Controller* rtc,
                                                 andrtf3::ANDRTF3* andrtf3);
    extern bool configureMB8ARTHAL(MB8ART* mb8art);
    extern bool configureANDRTF3HAL(andrtf3::ANDRTF3* andrtf3);
    extern bool configureRYN4HAL(RYN4* ryn4);
}

Result<void> ModbusDeviceInitializer::initializeDevices(SystemInitializer* initializer) {
    LOG_INFO(TAG, "Initializing Modbus devices...");

    // ========== Initialize Hardware Mapping Bindings ==========
    LOG_INFO(TAG, "Initializing unified hardware mapping bindings...");
    RelayBindings::initialize();
    SensorBindings::initialize();
    ANDRTF3Bindings::initialize();
    LOG_INFO(TAG, "Hardware mapping bindings initialized");

    // ModbusRTU now supports proper watchdog control - disable during device initialization
    LOG_INFO(TAG, "Disabling ModbusRTU watchdog for device initialization...");
    SRP::getModbusMaster().setWatchdogEnabled(false);

    // Configure ESP-IDF log levels for noisy libraries
    #if defined(LOG_MODE_DEBUG_SELECTIVE)
        esp_log_level_set("esp32ModbusRTU", ESP_LOG_WARN);
        esp_log_level_set("ModbusD", ESP_LOG_WARN);
        esp_log_level_set("RYN4", ESP_LOG_INFO);
        esp_log_level_set("MB8ART", ESP_LOG_INFO);
    #elif defined(LOG_MODE_RELEASE)
        esp_log_level_set("esp32ModbusRTU", ESP_LOG_ERROR);
        esp_log_level_set("ModbusD", ESP_LOG_ERROR);
        esp_log_level_set("RYN4", ESP_LOG_WARN);
        esp_log_level_set("MB8ART", ESP_LOG_WARN);
    #endif

    bool mb8artInitialized = false;
    bool andrtf3Initialized = false;
    bool ryn4Initialized = false;

    // Initialize MB8ART
    auto mb8artResult = initializeMB8ART(initializer, mb8artInitialized);
    if (mb8artResult.isError()) {
        return mb8artResult;
    }

    // RS485 bus settling delay between device initializations
    // With correct parity (8E1), minimal settling time needed
    constexpr uint32_t RS485_BUS_SETTLE_MS = 20;
    LOG_DEBUG(TAG, "RS485 bus settling delay: %lu ms", RS485_BUS_SETTLE_MS);
    vTaskDelay(pdMS_TO_TICKS(RS485_BUS_SETTLE_MS));

    // Initialize ANDRTF3
    initializeANDRTF3(initializer, andrtf3Initialized);

    // RS485 bus settling delay between ANDRTF3 and RYN4
    LOG_DEBUG(TAG, "RS485 bus settling delay: %lu ms", RS485_BUS_SETTLE_MS);
    vTaskDelay(pdMS_TO_TICKS(RS485_BUS_SETTLE_MS));

    // Initialize RYN4
    auto ryn4Result = initializeRYN4(initializer, ryn4Initialized);
    if (ryn4Result.isError()) {
        return ryn4Result;
    }

    // Start ModbusCoordinator if ANY sensor is ready (RYN4 doesn't need it - it's a relay module)
    // This allows sensor reads to begin while network/other init continues
    if (mb8artInitialized || andrtf3Initialized) {
        LOG_INFO(TAG, "Starting Modbus coordinator for sensor synchronization...");
        auto& modbusCoordinator = ModbusCoordinator::getInstance();
        if (!modbusCoordinator.start()) {
            LOG_ERROR(TAG, "Failed to start ModbusCoordinator - sensors will run uncoordinated");
        } else {
            LOG_INFO(TAG, "ModbusCoordinator started successfully");
        }
    }

    // Log status and continue initialization if both critical devices are ready
    if (mb8artInitialized && ryn4Initialized) {
        LOG_INFO(TAG, "All Modbus devices initialized successfully");

        // Initialize CriticalDataStorage (RuntimeStorage already initialized earlier)
        initializeCriticalDataStorage(initializer);

        // Configure HAL
        LOG_INFO(TAG, "Configuring HAL with all devices...");
        HAL::configureHardwareAbstractionLayer(initializer->mb8art_, initializer->ryn4_,
                                                initializer->ds3231_, initializer->andrtf3_);

        // Re-enable ModbusRTU watchdog
        LOG_INFO(TAG, "Re-enabling ModbusRTU watchdog...");
        SRP::getModbusMaster().setWatchdogEnabled(true);
    } else {
        LOG_WARN(TAG, "Some Modbus devices need background initialization - MB8ART: %s, RYN4: %s",
                 mb8artInitialized ? "OK" : "PENDING",
                 ryn4Initialized ? "OK" : "PENDING");
    }

    // Create background monitoring task
    createBackgroundMonitoringTask(initializer, mb8artInitialized, ryn4Initialized);

    LOG_INFO(TAG, "Modbus device initialization phase completed");
    return Result<void>();
}

Result<void> ModbusDeviceInitializer::initializeMB8ART(SystemInitializer* initializer, bool& outInitialized) {
    outInitialized = false;

    // Create MB8ART temperature sensor device
    initializer->mb8art_ = new MB8ART(MB8ART_ADDRESS, "MB8ART1");
    if (!initializer->mb8art_) {
        return Result<void>(SystemError::MEMORY_ALLOCATION_FAILED, "Failed to create MB8ART device");
    }

    // Bind hardware config and sensor pointers (unified mapping)
    LOG_INFO(TAG, "Binding MB8ART sensor pointers...");
    initializer->mb8art_->setHardwareConfig(SensorHardware::CONFIGS.data());
    initializer->mb8art_->bindSensorPointers(SensorBindings::getBindingArray());

    // Configure MB8ART with event group
    LOG_INFO(TAG, "Configuring MB8ART with device ready event group...");
    initializer->mb8art_->setEventGroup(initializer->deviceReadyEventGroup_,
                                         SystemEvents::DeviceReady::MB8ART_READY,
                                         SystemEvents::DeviceReady::MB8ART_ERROR);

    // MB8ART is accessed via SystemInitializer member pointer or SRP
    // No ServiceContainer registration needed

    // Initialize MB8ART
    LOG_INFO(TAG, "Initializing MB8ART device...");

    const uint32_t INITIAL_RETRIES = 2;
    const uint32_t RETRY_DELAY_MS = 250;
    uint32_t retryCount = 0;

    LOG_INFO(TAG, "Attempting initial MB8ART connection...");
    while (retryCount < INITIAL_RETRIES) {
        unsigned long startTime = millis();
        IDeviceInstance::DeviceResult<void> result = initializer->mb8art_->initialize();
        unsigned long initTime = millis() - startTime;

        if (result.isOk()) {
            LOG_INFO(TAG, "MB8ART initialized successfully after %lu attempts", retryCount + 1);
            outInitialized = true;
            break;
        } else {
            retryCount++;
            if (retryCount < INITIAL_RETRIES) {
                // Get error info - show device error code if mapping doesn't provide useful info
                SystemError mappedError = LibraryErrorMapper::mapDeviceError(result.error());
                if (mappedError == SystemError::SUCCESS || mappedError == SystemError::UNKNOWN_ERROR) {
                    LOG_WARN(TAG, "MB8ART init attempt %lu failed (%lu ms, device error %d) - retrying...",
                             retryCount, initTime, static_cast<int>(result.error()));
                } else {
                    LOG_WARN(TAG, "MB8ART init attempt %lu failed (%lu ms): %s - retrying...",
                             retryCount, initTime, ErrorHandler::errorToString(mappedError));
                }
                vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            }
        }
    }

    if (!outInitialized) {
        LOG_WARN(TAG, "MB8ART not available after initial attempts - will retry in background");
    }

    // Deactivate unused MB8ART channels to prevent error log flood
    // Only configure if not already deactivated (config persists in hardware EEPROM)
#ifdef MB8ART_ACTIVE_CHANNELS
    if (outInitialized && initializer->mb8art_ != nullptr) {
        const auto* configs = initializer->mb8art_->getChannelConfigs();
        for (uint8_t ch = MB8ART_ACTIVE_CHANNELS; ch < 8; ch++) {
            // Check if already deactivated (mode 0x00 = DEACTIVATED)
            if (configs[ch].mode != 0x00) {
                auto result = initializer->mb8art_->configureChannelMode(ch, 0x0000);
                if (result.isOk()) {
                    LOG_INFO(TAG, "MB8ART CH%d deactivated (was mode 0x%02X)", ch, configs[ch].mode);
                } else {
                    LOG_WARN(TAG, "Failed to deactivate MB8ART CH%d", ch);
                }
            }
        }
        LOG_DEBUG(TAG, "MB8ART unused channel check complete (CH%d-7)", MB8ART_ACTIVE_CHANNELS);
    }
#endif

    // Configure MB8ART HAL immediately after successful initialization
    // This ensures temperature sensor HAL is available even if RYN4 fails later
    if (outInitialized && initializer->mb8art_ != nullptr) {
        if (HAL::configureMB8ARTHAL(initializer->mb8art_)) {
            LOG_INFO(TAG, "MB8ART HAL configured successfully");
        } else {
            LOG_WARN(TAG, "Failed to configure MB8ART HAL");
        }
    }

    // Create MB8ART tasks if device was initialized
    if (outInitialized && initializer->mb8art_ != nullptr) {
        LOG_INFO(TAG, "Creating MB8ART processing task...");
        TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::disabled();

        if (SRP::getTaskManager().startTaskPinned(
            MB8ARTProcessingTask,
            "MB8ARTProc",
            STACK_SIZE_MB8ART_PROCESSING_TASK,
            initializer->mb8art_,
            PRIORITY_MB8ART_PROCESSING_TASK,
            1,
            wdtConfig)) {
            LOG_INFO(TAG, "MB8ART processing task created successfully");
        } else {
            LOG_ERROR(TAG, "Failed to create MB8ART processing task");
            delete initializer->mb8art_;
            initializer->mb8art_ = nullptr;
            outInitialized = false;
        }

        if (initializer->mb8art_ != nullptr) {
            LOG_INFO(TAG, "Creating MB8ART data acquisition task...");
            TaskManager::WatchdogConfig mb8artWdtConfig = TaskManager::WatchdogConfig::disabled();

            if (SRP::getTaskManager().startTaskPinned(
                MB8ARTTask,
                "MB8ART",
                STACK_SIZE_MODBUS_CONTROL_TASK,
                initializer->mb8art_,
                PRIORITY_MODBUS_CONTROL_TASK,
                1,
                mb8artWdtConfig)) {
                LOG_INFO(TAG, "MB8ART data acquisition task created successfully");
            } else {
                LOG_ERROR(TAG, "Failed to create MB8ART data acquisition task");
                delete initializer->mb8art_;
                initializer->mb8art_ = nullptr;
                outInitialized = false;
            }
        }
    }

    return Result<void>();
}

void ModbusDeviceInitializer::initializeANDRTF3(SystemInitializer* initializer, bool& outInitialized) {
    outInitialized = false;
    LOG_INFO(TAG, "=== Starting ANDRTF3 Initialization ===");

    LOG_INFO(TAG, "Creating ANDRTF3 instance for address 0x%02X...", ANDRTF3_ADDRESS);

    initializer->andrtf3_ = new andrtf3::ANDRTF3(ANDRTF3_ADDRESS);
    if (!initializer->andrtf3_) {
        LOG_ERROR(TAG, "Failed to allocate memory for ANDRTF3");
        return;
    }

    LOG_INFO(TAG, "ANDRTF3 instance created successfully");

    // Bind temperature pointers (unified mapping)
    LOG_INFO(TAG, "Binding ANDRTF3 temperature pointers...");
    initializer->andrtf3_->bindTemperaturePointers(ANDRTF3Bindings::insideTempPtr,
                                                    ANDRTF3Bindings::insideTempValidPtr);

    // Configure after creation
    andrtf3::ANDRTF3::Config config = initializer->andrtf3_->getConfig();
    config.timeout = 1000;
    config.retries = 3;
    initializer->andrtf3_->setConfig(config);
    LOG_INFO(TAG, "ANDRTF3 configured with timeout=%dms, retries=%d", config.timeout, config.retries);

    // ANDRTF3 is accessed via SystemInitializer member pointer or SRP
    // No ServiceContainer registration needed

    // Test connection
    LOG_INFO(TAG, "Testing ANDRTF3 connection...");
    if (initializer->andrtf3_->readTemperature()) {
        LOG_INFO(TAG, "ANDRTF3 connection test passed");
        int16_t initialTemp = initializer->andrtf3_->getTemperature();
        float tempCelsius = initialTemp / 10.0f;
        LOG_INFO(TAG, "Initial temperature: %.1f°C", tempCelsius);

        // Configure ANDRTF3 HAL immediately after successful connection test
        // This ensures ANDRTF3Task can use HAL even if RYN4 fails later
        if (HAL::configureANDRTF3HAL(initializer->andrtf3_)) {
            LOG_INFO(TAG, "ANDRTF3 HAL configured successfully");
            outInitialized = true;
        } else {
            LOG_WARN(TAG, "Failed to configure ANDRTF3 HAL");
        }
    } else {
        LOG_WARN(TAG, "ANDRTF3 connection test failed - sensor may not be connected");
    }

    LOG_INFO(TAG, "ANDRTF3 initialization section complete");
}

Result<void> ModbusDeviceInitializer::initializeRYN4(SystemInitializer* initializer, bool& outInitialized) {
    outInitialized = false;

    LOG_INFO(TAG, "Creating RYN4 instance for address 0x%02X...", RYN4_ADDRESS);
    initializer->ryn4_ = new RYN4(RYN4_ADDRESS, "RYN41");
    if (!initializer->ryn4_) {
        LOG_ERROR(TAG, "Failed to allocate memory for RYN4");
        return Result<void>(SystemError::MEMORY_ALLOCATION_FAILED, "Failed to create RYN4 device");
    }
    LOG_INFO(TAG, "RYN4 instance created successfully");

    // Bind hardware config and relay pointers (unified mapping)
    LOG_INFO(TAG, "Binding RYN4 relay pointers...");
    initializer->ryn4_->setHardwareConfig(RelayHardware::CONFIGS.data());
    initializer->ryn4_->bindRelayPointers(RelayBindings::getPointerArray());

    // Configure with event group
    LOG_INFO(TAG, "Configuring RYN4 with device ready event group...");
    initializer->ryn4_->setEventGroup(initializer->deviceReadyEventGroup_,
                                       SystemEvents::DeviceReady::RYN4_READY,
                                       SystemEvents::DeviceReady::RYN4_ERROR);

    // RYN4 is accessed via SystemInitializer member pointer or SRP
    // No ServiceContainer registration needed

    // Create RYN4 processing task first
    LOG_INFO(TAG, "Creating RYN4 processing task...");
    TaskManager::WatchdogConfig ryn4ProcWdtConfig = TaskManager::WatchdogConfig::disabled();

    if (SRP::getTaskManager().startTaskPinned(
        RYN4ProcessingTask,
        "RYN4Proc",
        STACK_SIZE_RYN4_PROCESSING_TASK,
        initializer->ryn4_,
        PRIORITY_RYN4_PROCESSING_TASK,
        1,
        ryn4ProcWdtConfig)) {
        LOG_INFO(TAG, "RYN4 processing task created successfully on core 1");
    } else {
        LOG_ERROR(TAG, "Failed to create RYN4 processing task");
        delete initializer->ryn4_;
        initializer->ryn4_ = nullptr;
        return Result<void>(SystemError::TASK_CREATE_FAILED, "RYN4 processing task creation failed");
    }

    // Small delay to ensure task is running
    vTaskDelay(pdMS_TO_TICKS(50));

    // Initialize RYN4
    LOG_INFO(TAG, "Initializing RYN4 device...");

    const uint32_t INITIAL_RETRIES = 2;
    const uint32_t RETRY_DELAY_MS = 250;
    uint32_t retryCount = 0;

    LOG_DEBUG(TAG, "About to log 'Attempting initial RYN4 connection' at %lu ms", millis());
    LOG_INFO(TAG, "Attempting initial RYN4 connection...");
    LOG_DEBUG(TAG, "After logging 'Attempting initial RYN4 connection' at %lu ms", millis());

    // Configure RYN4 to reset relays and verify states for safety
    RYN4::InitConfig ryn4InitConfig;
    ryn4InitConfig.resetRelaysOnInit = true;
    ryn4InitConfig.skipRelayStateRead = false;  // Verify relay states after reset
    LOG_INFO(TAG, "RYN4 InitConfig: resetRelaysOnInit=%s, skipRelayStateRead=%s",
             ryn4InitConfig.resetRelaysOnInit ? "true" : "false",
             ryn4InitConfig.skipRelayStateRead ? "true" : "false");

    while (retryCount < INITIAL_RETRIES) {
        unsigned long startTime = millis();
        LOG_DEBUG(TAG, "About to call ryn4->initialize() at %lu ms", startTime);

        IDeviceInstance::DeviceResult<void> result = initializer->ryn4_->initialize(ryn4InitConfig);

        unsigned long endTime = millis();
        unsigned long initTime = endTime - startTime;
        LOG_DEBUG(TAG, "ryn4->initialize() returned after %lu ms (start:%lu, end:%lu)",
                 initTime, startTime, endTime);

        if (result.isOk()) {
            LOG_INFO(TAG, "RYN4 initialized successfully after %lu attempts", retryCount + 1);

            // NOTE: DELAY timer cancellation now handled by library (RYN4Config.cpp)
            // The library's resetRelaysOnInit (default=true) sends DELAY 0 × 8 during initialize()
            // This ensures all relays are OFF and all hardware DELAY timers are cancelled
            // even if RYN4 module kept power through ESP32 reset
            // Redundant emergencyStopAll() call commented out (2025-12-16):
            /*
            LOG_INFO(TAG, "Cancelling any active DELAY timers and forcing all relays OFF");
            auto stopResult = initializer->ryn4_->emergencyStopAll();
            if (stopResult == ryn4::RelayErrorCode::SUCCESS) {
                LOG_INFO(TAG, "Emergency stop complete - clean initial state established");
            } else {
                LOG_WARN(TAG, "Emergency stop failed - relays may have active timers");
            }
            */

            outInitialized = true;
            break;
        } else {
            retryCount++;
            if (retryCount < INITIAL_RETRIES) {
                // Get error info - show device error code if mapping doesn't provide useful info
                SystemError mappedError = LibraryErrorMapper::mapDeviceError(result.error());
                if (mappedError == SystemError::SUCCESS || mappedError == SystemError::UNKNOWN_ERROR) {
                    LOG_WARN(TAG, "RYN4 init attempt %lu failed (%lu ms, device error %d) - retrying...",
                             retryCount, initTime, static_cast<int>(result.error()));
                } else {
                    LOG_WARN(TAG, "RYN4 init attempt %lu failed (%lu ms): %s - retrying...",
                             retryCount, initTime, ErrorHandler::errorToString(mappedError));
                }
                vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            }
        }
    }

    // Configure RYN4 HAL immediately after successful initialization
    // This ensures relay control via HAL is available even if MB8ART fails later
    if (outInitialized && initializer->ryn4_ != nullptr) {
        if (HAL::configureRYN4HAL(initializer->ryn4_)) {
            LOG_INFO(TAG, "RYN4 HAL configured successfully");
        } else {
            LOG_WARN(TAG, "Failed to configure RYN4 HAL");
        }
    }

    if (!outInitialized) {
        LOG_WARN(TAG, "RYN4 not available after initial attempts - will retry in background");
        // Set error bit so RYN4ProcessingTask can continue without waiting for full timeout
        if (initializer->deviceReadyEventGroup_ != nullptr) {
            xEventGroupSetBits(initializer->deviceReadyEventGroup_,
                               SystemEvents::DeviceReady::RYN4_ERROR);
            LOG_DEBUG(TAG, "Set RYN4_ERROR event bit to unblock processing task");
        }
    }

    return Result<void>();
}

void ModbusDeviceInitializer::initializeCriticalDataStorage(SystemInitializer* initializer) {
    // RuntimeStorage is now initialized early in SystemInitializer::initializeHardware()
    // This function only handles CriticalDataStorage which depends on RuntimeStorage

    if (!initializer->runtimeStorage_) {
        LOG_WARN(TAG, "RuntimeStorage not available - skipping CriticalDataStorage init");
        return;
    }

    // Initialize CriticalDataStorage
    LOG_INFO(TAG, "Initializing CriticalDataStorage...");
    if (CriticalDataStorage::begin()) {
        LOG_INFO(TAG, "CriticalDataStorage initialized successfully");

        if (CriticalDataStorage::hasEmergencyState()) {
            auto emergencyState = CriticalDataStorage::getEmergencyState();
            LOG_WARN(TAG, "Previous emergency state detected at timestamp %lu, reason=%d",
                    emergencyState.timestamp, emergencyState.reason);
        }
    } else {
        LOG_WARN(TAG, "Failed to initialize CriticalDataStorage");
    }
}

void ModbusDeviceInitializer::createBackgroundMonitoringTask(SystemInitializer* initializer,
                                                              bool mb8artInitialized,
                                                              bool ryn4Initialized) {
    LOG_INFO(TAG, "Creating background device monitoring task...");

    // Create a structure to pass initialization state to the task
    struct ModbusInitState {
        MB8ART* mb8art;
        RYN4* ryn4;
        andrtf3::ANDRTF3* andrtf3;
        bool mb8artDone;
        bool ryn4Done;
        bool andrtf3Done;
        SystemInitializer* initializer;
    };

    // Use RAII to ensure cleanup on all paths
    TaskParamRAII<ModbusInitState> initState(new ModbusInitState{
        initializer->mb8art_,
        initializer->ryn4_,
        initializer->andrtf3_,
        mb8artInitialized,
        ryn4Initialized,
        initializer->andrtf3_ != nullptr && initializer->andrtf3_->isConnected(),
        initializer
    });

    // Create background initialization task - lightweight verification only
    // Main initialization is done synchronously; this task only verifies and handles edge cases
    if (xTaskCreate(
        [](void* param) {
            ModbusInitState* state = (ModbusInitState*)param;

            LOG_INFO(TAG, "Background Modbus verification task started");

            // Verify SystemInitializer is valid
            if (state->initializer == nullptr) {
                LOG_ERROR(TAG, "SystemInitializer is NULL in background task!");
                delete state;
                vTaskDelete(NULL);
                return;
            }

            // Brief delay for any pending operations to settle (100ms, not 3 seconds)
            vTaskDelay(pdMS_TO_TICKS(100));

            // Check device status - they should already be initialized synchronously
            bool mb8artReady = state->mb8artDone || (state->mb8art != nullptr && state->mb8art->isInitialized());
            bool ryn4Ready = state->ryn4Done || (state->ryn4 != nullptr && state->ryn4->isInitialized());
            bool andrtf3Ready = state->andrtf3Done || (state->andrtf3 != nullptr && state->andrtf3->isConnected());

            LOG_INFO(TAG, "Device status - MB8ART:%s RYN4:%s ANDRTF3:%s",
                     mb8artReady ? "OK" : "FAIL",
                     ryn4Ready ? "OK" : "FAIL",
                     andrtf3Ready ? "OK" : "FAIL");

            if (mb8artReady && ryn4Ready) {
                // Devices initialized - background verification complete
                // Note: Control tasks (BurnerControl, HeatingControl, WaterControl) are created
                // later by TaskInitializer, so we don't check for them here.
                LOG_INFO(TAG, "Background verification complete - all devices ready");
            } else {
                // Devices not ready during synchronous init - wait briefly for async completion
                const TickType_t DEVICE_READY_TIMEOUT = pdMS_TO_TICKS(2000);  // 2 seconds max
                EventBits_t readyBits = xEventGroupWaitBits(
                    state->initializer->deviceReadyEventGroup_,
                    SystemEvents::DeviceReady::ALL_CRITICAL_READY,
                    pdFALSE,
                    pdTRUE,
                    DEVICE_READY_TIMEOUT
                );

                if ((readyBits & SystemEvents::DeviceReady::ALL_CRITICAL_READY) == SystemEvents::DeviceReady::ALL_CRITICAL_READY) {
                    LOG_INFO(TAG, "Devices became ready after async wait");
                } else {
                    // Log error but don't try to recreate - synchronous init should have handled this
                    LOG_ERROR(TAG, "Device init incomplete - MB8ART:%d RYN4:%d (bits:0x%02X)",
                             (readyBits & SystemEvents::DeviceReady::MB8ART_READY) ? 1 : 0,
                             (readyBits & SystemEvents::DeviceReady::RYN4_READY) ? 1 : 0,
                             readyBits);
                }
            }

            LOG_INFO(TAG, "Background verification task completed");
            delete state;
            vTaskDelete(NULL);
        },
        "ModbusInit",
        4096,
        initState.get(),
        1,
        nullptr
    ) != pdPASS) {
        LOG_ERROR(TAG, "Failed to create background Modbus initialization task!");
    } else {
        initState.release();
    }
}
