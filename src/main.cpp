// src/main.cpp - Streamlined version with SystemInitializer
#include <Arduino.h>
#include "config/ProjectConfig.h"
#include "config/SystemConstants.h"
#include "init/SystemInitializer.h"
#include "utils/ErrorHandler.h"
// #include "utils/EarlyLogCapture.h"  // Not needed - monitor connects early
#include <TaskManager.h>
#include <Watchdog.h>
#include "LoggingMacros.h"
#ifndef LOG_NO_CUSTOM_LOGGER
#include <Logger.h>
#include <LogInterfaceImpl.cpp>  // Include implementation once
#endif
#include "core/SharedResourceManager.h"
#include <MB8ART.h>
#include <RYN4.h>
#include "MQTTManager.h"
#include <esp32ModbusRTU.h>
#include <unordered_map>
#include "modules/control/HeatingControlModule.h"
#include "modules/control/PIDControlModule.h"
#include "shared/SharedSensorReadings.h"
#include "shared/SharedRelayReadings.h"
// BLE includes removed - using MB8ART channel 7 for inside temperature
#include "core/SystemResourceProvider.h"
#include "events/SystemEventsGenerated.h"  // C7: For DEGRADED_MODE
#include "modules/tasks/TimerSchedulerTask.h"
#include "modules/tasks/NTPTask.h"
#include <esp_log.h>

static const char* TAG = "Main";

// BurnerSystemController is created in SystemInitializer and passed to WheaterControlTask via parameter

// Task function declarations are now in header files
#include <nvs_flash.h>
#include <cstring>  // for strcmp
#include <RuntimeStorage.h>
#include <esp_task_wdt.h>  // For disabling global watchdog

// Type alias to avoid IntelliSense confusion with namespace/class naming
using RuntimeStoragePtr = class rtstorage::RuntimeStorage*;

// Configuration constants
constexpr uint32_t SERIAL_TIMEOUT_MS = 2000;

// Override weak function to reduce loopTask stack size
// This function has C++ linkage in Arduino.h
size_t getArduinoLoopTaskStackSize() {
    return STACK_SIZE_LOOP_TASK;  // Use configured stack size from ProjectConfig.h
}

// Note: The following functions may or may not exist in the framework
// We declare them as weak so they don't cause linker errors if unused
__attribute__((weak)) size_t getArduinoEventTaskStackSize() {
    return 3072;  // Attempt to reduce arduino_events stack
}

__attribute__((weak)) size_t getTimerTaskStackSize() {
    return 3072;  // Attempt to reduce timer service stack
}

// Global system initializer instance
SystemInitializer* gSystemInitializer = nullptr;

// Logger now uses singleton pattern - Logger::getInstance()
// The singleton automatically creates itself with ConsoleBackend
// auto consoleBackend = std::make_shared<ConsoleBackend>();
// Logger logger(consoleBackend);

// Global task manager with Watchdog singleton injection
TaskManager taskManager(&Watchdog::getInstance());

// Modbus master instance
esp32ModbusRTU modbusMaster(&Serial1);

// Global device map is defined in ModbusDevice.cpp
// Access via SRP::getDeviceMap() and SRP::getDeviceMapMutex()

// Legacy global device pointers (for backward compatibility)
// These are updated by the accessor functions
MB8ART* MB8ART1 = nullptr;
RYN4* RYN41 = nullptr;

// Runtime storage (FRAM) for persistent data
RuntimeStoragePtr gRuntimeStorage = nullptr;

// Control modules are accessed via SRP (SystemResourceProvider)
// ServiceContainer has been removed - use SRP::getXXX() methods

// Task handles
TaskHandle_t burnerTaskHandle = nullptr;

// Shared data structures are defined in their respective .cpp files

// Event groups (temporarily defined here until fully migrated to SharedResourceManager)
EventGroupHandle_t xGeneralSystemEventGroup = nullptr;

// RYN4 relay status bits are defined in RYN4.cpp
// Access via SRP::getRelayAllUpdateBits() and SRP::getRelayAllErrorBits()

// Other global variables from SharedResources.h
int pidFactorSpaceHeating = 1;
int pidFactorWaterHeating = 1;

// Forward declarations for runtime functions
void handleRuntimeTasks();
void updateSystemHealth();
void configureLibraryLogging();
void setQuietMode();
void setVerboseMode();
void enableLibraryDebug(const char* libName);
void restoreNormalLogging();

// Helper function to ensure clean logging during critical sections
void flushLogsWithDelay(uint32_t delayMs = 50) {
    #ifndef LOG_NO_CUSTOM_LOGGER
    // Removed flush() - can cause blocking with full serial buffer
    vTaskDelay(pdMS_TO_TICKS(delayMs));
    #endif
}

void setup() {
    // Phase 1: Critical early initialization
    Serial.setTxBufferSize(8192);  // Maximum buffer size to prevent any log loss
    Serial.begin(SERIAL_BAUD_RATE);
    
    // Boot markers to verify monitor connection
    Serial.println("\n\n========== ESP32 BOOT START ==========");
    Serial.println("If you see this, monitor connected early");
    Serial.println("======================================\n");
    Serial.flush();
    
    // Small delay to ensure serial is stable
    delay(100);
    
    // Note: Removed Serial.println to avoid duplicate logs
    
    // Initialize NVS flash before any usage
    // IMPORTANT: Arduino framework tries to init NVS very early in initArduino()
    // We need to ensure it's properly initialized for our use
    
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition needs erasing
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    
    if (ret != ESP_OK) {
        // C7: NVS init failed - this is CRITICAL for safety config persistence
        Serial.printf("CRITICAL: NVS init failed (error: 0x%x)!\n", ret);
        Serial.println("System will use compile-time defaults - safety config NOT persisted!");

        // Set degraded mode flag if event group exists
        if (xGeneralSystemEventGroup) {
            xEventGroupSetBits(xGeneralSystemEventGroup,
                              SystemEvents::SystemState::DEGRADED_MODE);
        }
        // Note: LOG_ERROR not available yet at this point in startup
    } else {
        Serial.println("NVS initialized successfully");
    }

    // Note: Safety configuration is loaded by PersistentStorageTask
    // Note: ESP32 global task watchdog cannot be fully disabled (Arduino framework requirement)
    // Tasks must either:
    //   1. Call esp_task_wdt_reset() periodically, OR
    //   2. Unsubscribe via esp_task_wdt_delete(NULL)
    // We feed both watchdogs (ESP32 global + TaskManager) in critical tasks

    // Initialize Logger early to ensure it's ready for all logging
    #ifndef LOG_NO_CUSTOM_LOGGER
    Logger& logger = Logger::getInstance();
    
    // Logger now uses NonBlockingConsoleBackend by default
    
    // Initialize the logger with larger buffer to prevent overflow during startup
    logger.init(1024);  // Further increased buffer size to prevent message loss during heavy init
    
    // Set the log level based on build mode
    #ifdef LOG_MODE_RELEASE
        logger.setLogLevel(ESP_LOG_WARN);  // Only warnings and errors in release
    #else
        logger.setLogLevel(ESP_LOG_INFO);     // Set default log level
    #endif
    
    // Disable rate limiting during startup for complete logs
    logger.setMaxLogsPerSecond(0);  // 0 = unlimited
    
    // Enable ESP-IDF log redirection to prevent interleaving
    logger.enableESPLogRedirection();
    
    // Configure tag-specific logging levels
    configureLibraryLogging();
    
    // Test logger is working
    LOG_INFO("BOOT", "Logger initialized and ready");

    // Flush to ensure early logs are visible immediately
    logger.flush();
    #else
    // When using ESP-IDF logging, set default log level
    esp_log_level_set("*", ESP_LOG_INFO);
    LOG_INFO("BOOT", "Using ESP-IDF logging (no custom logger)");
    #endif
    
    // Suppress verbose HAL and other logs early
    #if defined(LOG_MODE_DEBUG_SELECTIVE)
        // Force selective logging mode - suppress verbose output
        esp_log_level_set("*", ESP_LOG_INFO);
        esp_log_level_set("efuse", ESP_LOG_NONE);
        esp_log_level_set("cpu_start", ESP_LOG_NONE);
        esp_log_level_set("heap_init", ESP_LOG_NONE);
        esp_log_level_set("intr_alloc", ESP_LOG_NONE);
        esp_log_level_set("spi_flash", ESP_LOG_WARN);
        esp_log_level_set("wifi", ESP_LOG_NONE);
        esp_log_level_set("wifi_init", ESP_LOG_NONE);
        esp_log_level_set("phy_init", ESP_LOG_NONE);
        esp_log_level_set("esp_core_dump_flash", ESP_LOG_NONE);  // Suppress core dump partition errors
        esp_log_level_set("esp_core_dump_elf", ESP_LOG_NONE);
    #endif
    
    // Set up LED for status indication
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);
    
    // Phase 2: Initialize critical resources
    // Device map mutex is now managed by ModbusRegistry singleton
    // No need to create it manually - ModbusRegistry::getInstance().getMutex() provides it
    
    // Create general system event group
    xGeneralSystemEventGroup = xEventGroupCreate();
    if (!xGeneralSystemEventGroup) {
        Serial.println("FATAL: Failed to create general system event group!");
        while (true) {
            digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
            delay(SystemConstants::Timing::FAILSAFE_LED_BLINK_MS);
        }
    }
    
    // Early initialization of critical singletons to prevent deadlock
    // Force singleton initialization in main thread before any tasks start
    LOG_INFO(TAG, "Pre-initializing critical singletons...");
    SharedResourceManager::getInstance();  // Initialize SharedResourceManager singleton
    // ServiceContainer removed - services accessed via SRP and gSystemInitializer
    
    // Phase 3: Create and run SystemInitializer
    // Use logger for banner output with proper line endings
    LOG_INFO(TAG, "");
    LOG_INFO(TAG, "==== ESPlan Boiler Controller ====");
    LOG_INFO(TAG, "%s v%s", PROJECT_NAME, FIRMWARE_VERSION);
    LOG_INFO(TAG, "==================================");
    LOG_INFO(TAG, "");
    
    LOG_INFO(TAG, "About to create SystemInitializer...");
    
    gSystemInitializer = new SystemInitializer();
    if (!gSystemInitializer) {
        Serial.println("FATAL: Failed to allocate SystemInitializer!");
        // Enter permanent failsafe
        while (true) {
            digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
            delay(SystemConstants::Timing::FAILSAFE_LED_BLINK_MS);
        }
    }
    
    LOG_INFO(TAG, "SystemInitializer created, about to call initializeSystem()...");
    
    // Run system initialization
    auto result = gSystemInitializer->initializeSystem();
    
    if (result.isError()) {
        // Log critical error
        Serial.printf("FATAL: System initialization failed at stage %d: %s\n", 
                     static_cast<int>(gSystemInitializer->getCurrentStage()),
                     ErrorHandler::errorToString(result.error()));
        
        // Attempt cleanup
        gSystemInitializer->cleanup();
        
        // Enter failsafe mode
        ErrorHandler::enterFailsafeMode(result.error());
        
        // Infinite loop with LED indication
        while (true) {
            digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
            delay(SystemConstants::Timing::FAILSAFE_LED_BLINK_MS);
        }
    }
    
    // Phase 3: System successfully initialized
    digitalWrite(LED_BUILTIN, HIGH); // LED on = system running
    
    // Re-apply task_wdt suppression after system init (in case it was overridden)
    // These errors come from NimBLE and are harmless
    esp_log_level_set("task_wdt", ESP_LOG_NONE);

    // Control modules are initialized in SystemInitializer
    // Access them using SRP::getXXX() methods (ServiceContainer removed)

    // BurnerControlTask is already initialized in TaskInitializer::initializeTasks()
    // No need to call initializeBurnerControlTask() again here

    // BurnerSystemController is now created in SystemInitializer::initializeControlModules()
    // and passed to WheaterControlTask via parameter (Pattern B: parameter passing)

    // Initialize TimerSchedulerTask first (registers RTC update callback needed by NTPTask)
    LOG_INFO(TAG, "Initializing TimerSchedulerTask...");

    TaskManager::WatchdogConfig timerWdtConfig = TaskManager::WatchdogConfig::disabled();
    if (SRP::getTaskManager().startTaskPinned(
        TimerSchedulerTask,
        "TimerSched",
        STACK_SIZE_TIMER_SCHEDULER_TASK,
        nullptr,
        2,     // Priority - same as monitoring task
        0,     // Core 0
        timerWdtConfig)) {
        LOG_INFO(TAG, "TimerSchedulerTask created successfully");
    } else {
        LOG_WARN(TAG, "Failed to create TimerSchedulerTask (non-critical)");
    }

    // Initialize NTPTask for network time synchronization (after TimerScheduler registers callback)
    LOG_INFO(TAG, "Initializing NTPTask...");

    // NTPTask manages its own watchdog internally (60s timeout via TaskManager + yield callback)
    TaskManager::WatchdogConfig ntpWdtConfig = TaskManager::WatchdogConfig::disabled();

    if (SRP::getTaskManager().startTaskPinned(
        NTPTask,
        "NTPTask",
        4096,  // Stack size increased to prevent overflow
        nullptr,
        2,     // Priority - same as monitoring task
        1,     // Core 1 (avoids Core 0 timing issues)
        ntpWdtConfig)) {
        LOG_INFO(TAG, "NTPTask created successfully");
    } else {
        LOG_WARN(TAG, "Failed to create NTPTask (non-critical)");
    }
    
    LOG_INFO(TAG, "==================================");
    LOG_INFO(TAG, "System initialization complete!");
    LOG_INFO(TAG, "Free heap: %d bytes", ESP.getFreeHeap());
    LOG_INFO(TAG, "==================================");
    
    // Wait for logs to output before re-enabling rate limiting
    #ifndef LOG_NO_CUSTOM_LOGGER
    // Removed flush() - can cause blocking with full serial buffer
    vTaskDelay(pdMS_TO_TICKS(100));  // Give time for serial output
    
    // Re-enable rate limiting with higher limit for normal operation
    Logger::getInstance().setMaxLogsPerSecond(200);  // Increased from 100 to prevent drops
    LOG_INFO(TAG, "Rate limiting enabled: 200 logs/second");
    
    // Restore normal logging levels after startup
    restoreNormalLogging();
    #endif
}

void loop() {
    // Main loop delegates to runtime handlers
    handleRuntimeTasks();
    updateSystemHealth();
    
    // Small delay to prevent watchdog issues
    delay(1);
}

/**
 * @brief Handle runtime tasks that need periodic execution
 */
void handleRuntimeTasks() {
    // Check for any system-wide events
    if (gSystemInitializer && gSystemInitializer->isFullyInitialized()) {
        // System is running normally
        static uint32_t lastMemoryReport = 0;
        uint32_t now = millis();
        
        // Memory monitoring every 5 minutes (or immediately if low)
        if (now - lastMemoryReport > 300000) {  // 5 minutes
            lastMemoryReport = now;
            size_t freeHeap = ESP.getFreeHeap();
            size_t minFreeHeap = ESP.getMinFreeHeap();
            
            // H14: Use standardized heap thresholds
            if (freeHeap < SystemConstants::System::MIN_FREE_HEAP_WARNING) {
                LOG_WARN(TAG, "LOW MEMORY WARNING: Free: %d, Min: %d bytes",
                         freeHeap, minFreeHeap);
            } else {
                LOG_DEBUG(TAG, "Memory status: Free: %d, Min: %d bytes",
                          freeHeap, minFreeHeap);
            }
        }

        // Check for critically low memory more frequently
        size_t currentFreeHeap = ESP.getFreeHeap();
        if (currentFreeHeap < SystemConstants::System::MIN_FREE_HEAP_CRITICAL) {
            static uint32_t lastLowMemoryWarning = 0;
            if (now - lastLowMemoryWarning > 10000) {  // Warn every 10s when memory is low
                lastLowMemoryWarning = now;
                LOG_WARN(TAG, "LOW MEMORY WARNING: Free: %d bytes", currentFreeHeap);
            }
        }
    }
}

/**
 * @brief Update system health indicators
 */
void updateSystemHealth() {
    static uint32_t lastLedToggle = 0;
    static bool ledState = true;
    uint32_t now = millis();
    
    // Heartbeat LED pattern when system is healthy
    if (gSystemInitializer && gSystemInitializer->isFullyInitialized()) {
        // Slow blink = healthy (1Hz)
        if (now - lastLedToggle > 1000) {
            lastLedToggle = now;
            ledState = !ledState;
            digitalWrite(LED_BUILTIN, ledState);
        }
    }
    // Fast blink handled in error conditions above
}

// Logging configuration functions
void configureLibraryLogging() {
    #ifndef LOG_NO_CUSTOM_LOGGER
    Logger& logger = Logger::getInstance();
    
    // ========== Critical System Components ==========
    // Reduce verbosity during startup to prevent buffer overflow
    logger.setTagLevel("BurnerControl", ESP_LOG_WARN);  // Reduced from INFO
    logger.setTagLevel("HeatingControl", ESP_LOG_WARN);  // Reduced from INFO
    logger.setTagLevel("WheaterControl", ESP_LOG_WARN);  // Reduced from INFO
    logger.setTagLevel("PIDControl", ESP_LOG_WARN);     // Reduced from INFO
    logger.setTagLevel("SystemInit", ESP_LOG_INFO);     // Keep for startup tracking
    logger.setTagLevel(TAG, ESP_LOG_INFO);     // Keep for main flow
    logger.setTagLevel("HWScheduler", ESP_LOG_WARN);    // Reduced from INFO
    logger.setTagLevel("NTPTask", ESP_LOG_INFO);        // Keep at INFO to debug startup
    logger.setTagLevel("NTPClient", ESP_LOG_WARN);      // Reduced from DEBUG
    
    // ========== Hardware Devices ==========
    // Debug level controlled by compile flags
    #ifdef MB8ART_DEBUG
        logger.setTagLevel("MB8ART", ESP_LOG_DEBUG);
    #else
        logger.setTagLevel("MB8ART", ESP_LOG_WARN);      // Reduced from INFO
    #endif
    
    #ifdef RYN4_DEBUG
        logger.setTagLevel("RYN4", ESP_LOG_DEBUG);
    #else
        logger.setTagLevel("RYN4", ESP_LOG_WARN);         // Reduced from INFO
    #endif
    
    // Modbus components - reduce noise during startup
    #ifdef MODBUSDEVICE_DEBUG
        logger.setTagLevel("ModbusD", ESP_LOG_DEBUG);
        logger.setTagLevel("ModbusDevice", ESP_LOG_DEBUG);
    #else
        logger.setTagLevel("ModbusD", ESP_LOG_WARN);      // Reduced from INFO
        logger.setTagLevel("ModbusDevice", ESP_LOG_WARN); // Reduced from INFO
    #endif
    
    #ifdef ESP32MODBUSRTU_DEBUG
        logger.setTagLevel("ModbusRTU", ESP_LOG_DEBUG);
    #else
        logger.setTagLevel("ModbusRTU", ESP_LOG_WARN);    // Reduced from INFO
    #endif
    
    // ========== Network Components ==========
    // These can be very verbose, especially during connection
    #ifdef ETH_DEBUG
        logger.setTagLevel("ETH", ESP_LOG_DEBUG);
        logger.setTagLevel("EthernetManager", ESP_LOG_DEBUG);
        logger.setTagLevel("NetworkMonitor", ESP_LOG_DEBUG);
        logger.setTagLevel("sys_evt", ESP_LOG_DEBUG);
    #else
        logger.setTagLevel("ETH", ESP_LOG_WARN);
        logger.setTagLevel("EthernetManager", ESP_LOG_WARN);
        logger.setTagLevel("NetworkMonitor", ESP_LOG_WARN);  // Add NetworkMonitor tag
        logger.setTagLevel("sys_evt", ESP_LOG_WARN);         // Add system event tag
    #endif
    
    #ifdef OTA_DEBUG
        logger.setTagLevel("OTAMgr", ESP_LOG_DEBUG);
        logger.setTagLevel("OTAManager", ESP_LOG_DEBUG);
    #else
        logger.setTagLevel("OTAMgr", ESP_LOG_WARN);
        logger.setTagLevel("OTAManager", ESP_LOG_WARN);
    #endif
    
    logger.setTagLevel("MQTTManager", ESP_LOG_WARN);
    logger.setTagLevel("MQTTTask", ESP_LOG_WARN);
    
    // ========== Utility Libraries ==========
    // These are usually very noisy and not needed unless debugging
    #ifdef TASK_MANAGER_DEBUG
        logger.setTagLevel("TaskManager", ESP_LOG_DEBUG);
    #else
        logger.setTagLevel("TaskManager", ESP_LOG_ERROR);
    #endif
    
    logger.setTagLevel("SemaphoreGuard", ESP_LOG_ERROR);
    logger.setTagLevel("MutexGuard", ESP_LOG_ERROR);
    logger.setTagLevel("Watchdog", ESP_LOG_ERROR);
    logger.setTagLevel("PStore", ESP_LOG_WARN);  // PersistentStorage
    logger.setTagLevel("PersistentStora", ESP_LOG_WARN);  // Truncated tag
    
    // ========== BLE Components Removed ==========
    // BLE functionality removed - using MB8ART channel 7 for inside temperature
    
    // ========== Monitoring ==========
    #ifdef MONITOR_TASK_DEBUG
        logger.setTagLevel("MonitoringTask", ESP_LOG_DEBUG);
        logger.setTagLevel("MON", ESP_LOG_DEBUG);
    #else
        logger.setTagLevel("MonitoringTask", ESP_LOG_WARN);  // Reduced from INFO
        logger.setTagLevel("MON", ESP_LOG_WARN);             // Reduced from INFO
    #endif
    
    LOG_INFO(TAG, "Library logging configured - Critical: INFO, Hardware: %s, Network: WARN, Utility: ERROR",
             #if defined(MB8ART_DEBUG)
             "MB8ART:DEBUG"
             #elif defined(RYN4_DEBUG)
             "RYN4:DEBUG"
             #else
             "WARN"
             #endif
    );
    #endif // LOG_NO_CUSTOM_LOGGER
}

void setQuietMode() {
    #ifndef LOG_NO_CUSTOM_LOGGER
    Logger& logger = Logger::getInstance();
    
    // Set global level to ERROR
    logger.setLogLevel(ESP_LOG_ERROR);
    
    // But keep critical components at WARN level
    logger.setTagLevel("BurnerControl", ESP_LOG_WARN);
    logger.setTagLevel("HeatingControl", ESP_LOG_WARN);
    logger.setTagLevel("WheaterControl", ESP_LOG_WARN);
    logger.setTagLevel("SystemInit", ESP_LOG_WARN);
    logger.setTagLevel(TAG, ESP_LOG_WARN);
    
    // Silence everything else
    logger.setTagLevel("MB8ART", ESP_LOG_ERROR);
    logger.setTagLevel("RYN4", ESP_LOG_ERROR);
    logger.setTagLevel("ModbusD", ESP_LOG_NONE);
    logger.setTagLevel("ModbusDevice", ESP_LOG_NONE);
    logger.setTagLevel("ModbusRTU", ESP_LOG_NONE);
    
    LOG_WARN(TAG, "Quiet mode enabled - minimal logging");
    #endif
}

void setVerboseMode() {
    #ifndef LOG_NO_CUSTOM_LOGGER
    Logger& logger = Logger::getInstance();
    
    // Set global level to VERBOSE
    logger.setLogLevel(ESP_LOG_VERBOSE);
    
    // Enable verbose logging for all major components
    logger.setTagLevel("MB8ART", ESP_LOG_VERBOSE);
    logger.setTagLevel("RYN4", ESP_LOG_VERBOSE);
    logger.setTagLevel("ModbusD", ESP_LOG_DEBUG);
    logger.setTagLevel("ModbusDevice", ESP_LOG_DEBUG);
    logger.setTagLevel("ModbusRTU", ESP_LOG_DEBUG);
    logger.setTagLevel("BurnerControl", ESP_LOG_DEBUG);
    logger.setTagLevel("HeatingControl", ESP_LOG_DEBUG);
    logger.setTagLevel("WheaterControl", ESP_LOG_DEBUG);
    
    // But keep utility libraries reasonable
    logger.setTagLevel("TaskManager", ESP_LOG_INFO);
    logger.setTagLevel("SemaphoreGuard", ESP_LOG_WARN);
    logger.setTagLevel("MutexGuard", ESP_LOG_WARN);
    
    LOG_INFO(TAG, "Verbose mode enabled - detailed logging");
    #endif
}

void enableLibraryDebug(const char* libName) {
    #ifndef LOG_NO_CUSTOM_LOGGER
    if (!libName) return;
    
    Logger& logger = Logger::getInstance();
    logger.setTagLevel(libName, ESP_LOG_DEBUG);
    
    LOG_INFO(TAG, "Debug logging enabled for library: %s", libName);
    
    // Also enable related tags for common libraries
    if (strcmp(libName, "MB8ART") == 0) {
        logger.setTagLevel("ModbusDevice", ESP_LOG_DEBUG);
    } else if (strcmp(libName, "RYN4") == 0) {
        logger.setTagLevel("ModbusDevice", ESP_LOG_DEBUG);
    } else if (strcmp(libName, "ETH") == 0) {
        logger.setTagLevel("EthernetManager", ESP_LOG_DEBUG);
    } else if (strcmp(libName, "OTAMgr") == 0) {
        logger.setTagLevel("OTAManager", ESP_LOG_DEBUG);
    }
    #endif
}

// Example: MQTT command handler for runtime log control
// Add this to your MQTT command processing:
/*
void handleMQTTLogCommand(const char* command, const char* payload) {
    if (strcmp(command, "log/mode/quiet") == 0) {
        setQuietMode();
    } else if (strcmp(command, "log/mode/verbose") == 0) {
        setVerboseMode();
    } else if (strcmp(command, "log/mode/normal") == 0) {
        configureLibraryLogging();
    } else if (strcmp(command, "log/debug/enable") == 0) {
        enableLibraryDebug(payload);  // payload = library name
    } else if (strcmp(command, "log/level/set") == 0) {
        // Format: "library:level" e.g., "MB8ART:DEBUG" or "global:WARN"
        char lib[32] = {0};    // Zero-initialize for safety
        char level[16] = {0};  // Zero-initialize for safety
        if (sscanf(payload, "%31[^:]:%15s", lib, level) == 2) {
            esp_log_level_t logLevel = ESP_LOG_INFO;
            if (strcmp(level, "ERROR") == 0) logLevel = ESP_LOG_ERROR;
            else if (strcmp(level, "WARN") == 0) logLevel = ESP_LOG_WARN;
            else if (strcmp(level, "INFO") == 0) logLevel = ESP_LOG_INFO;
            else if (strcmp(level, "DEBUG") == 0) logLevel = ESP_LOG_DEBUG;
            else if (strcmp(level, "VERBOSE") == 0) logLevel = ESP_LOG_VERBOSE;
            else if (strcmp(level, "NONE") == 0) logLevel = ESP_LOG_NONE;
            
            if (strcmp(lib, "global") == 0) {
                Logger::getInstance().setLogLevel(logLevel);
                LOG_INFO(TAG, "Global log level set to %s", level);
            } else {
                Logger::getInstance().setTagLevel(lib, logLevel);
                LOG_INFO(TAG, "Log level for %s set to %s", lib, level);
            }
        }
    }
}
*/

void restoreNormalLogging() {
    #ifndef LOG_NO_CUSTOM_LOGGER
    Logger& logger = Logger::getInstance();
    
    LOG_INFO(TAG, "Restoring normal logging levels after startup...");
    
    // Restore system components based on debug flags
    #ifdef CONTROL_MODULE_DEBUG
        logger.setTagLevel("BurnerControl", ESP_LOG_DEBUG);
        logger.setTagLevel("HeatingControl", ESP_LOG_DEBUG);
        logger.setTagLevel("WheaterControl", ESP_LOG_DEBUG);
        logger.setTagLevel("PIDControl", ESP_LOG_DEBUG);
    #else
        logger.setTagLevel("BurnerControl", ESP_LOG_INFO);
        logger.setTagLevel("HeatingControl", ESP_LOG_INFO);
        logger.setTagLevel("WheaterControl", ESP_LOG_INFO);
        logger.setTagLevel("PIDControl", ESP_LOG_INFO);
    #endif
    
    logger.setTagLevel("HWScheduler", ESP_LOG_INFO);
    logger.setTagLevel("NTPTask", ESP_LOG_INFO);
    
    // Hardware devices back to INFO
    #ifndef MB8ART_DEBUG
        logger.setTagLevel("MB8ART", ESP_LOG_INFO);
    #endif
    
    #ifndef RYN4_DEBUG
        logger.setTagLevel("RYN4", ESP_LOG_INFO);
    #endif
    
    // Modbus components back to INFO
    #ifndef MODBUSDEVICE_DEBUG
        logger.setTagLevel("ModbusD", ESP_LOG_INFO);
        logger.setTagLevel("ModbusDevice", ESP_LOG_INFO);
    #endif
    
    #ifndef ESP32MODBUSRTU_DEBUG
        logger.setTagLevel("ModbusRTU", ESP_LOG_INFO);
    #endif
    
    // Network components
    #ifdef ETH_DEBUG
        logger.setTagLevel("ETH", ESP_LOG_DEBUG);
        logger.setTagLevel("EthernetManager", ESP_LOG_DEBUG);
        logger.setTagLevel("NetworkMonitor", ESP_LOG_DEBUG);
        logger.setTagLevel("sys_evt", ESP_LOG_DEBUG);
    #else
        logger.setTagLevel("ETH", ESP_LOG_INFO);
        logger.setTagLevel("EthernetManager", ESP_LOG_INFO);
        logger.setTagLevel("NetworkMonitor", ESP_LOG_INFO);
        logger.setTagLevel("sys_evt", ESP_LOG_INFO);
    #endif
    
    // OTA components
    #ifdef OTA_DEBUG
        logger.setTagLevel("OTAMgr", ESP_LOG_DEBUG);
        logger.setTagLevel("OTAManager", ESP_LOG_DEBUG);
    #else
        logger.setTagLevel("OTAMgr", ESP_LOG_INFO);
        logger.setTagLevel("OTAManager", ESP_LOG_INFO);
    #endif
    
    // Task Manager
    #ifdef TASK_MANAGER_DEBUG
        logger.setTagLevel("TaskManager", ESP_LOG_DEBUG);
    #else
        logger.setTagLevel("TaskManager", ESP_LOG_INFO);
    #endif
    
    // Temperature sensor
    #ifdef ANDRTF3_DEBUG
        logger.setTagLevel("ANDRTF3", ESP_LOG_DEBUG);
    #else
        logger.setTagLevel("ANDRTF3", ESP_LOG_INFO);
    #endif
    
    // Monitoring - adjust based on debug flag
    #ifdef MONITOR_TASK_DEBUG
        logger.setTagLevel("MonitoringTask", ESP_LOG_DEBUG);
        logger.setTagLevel("MON", ESP_LOG_DEBUG);
    #else
        logger.setTagLevel("MonitoringTask", ESP_LOG_INFO);
        logger.setTagLevel("MON", ESP_LOG_INFO);
    #endif
    
    LOG_INFO(TAG, "Normal logging levels restored");
    #endif
}