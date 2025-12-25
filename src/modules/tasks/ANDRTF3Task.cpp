// src/modules/tasks/ANDRTF3Task.cpp
// ANDRTF3 temperature sensor task - reads room temperature

#include "modules/tasks/ANDRTF3Task.h"

#include "config/SystemConstants.h"
#include "config/SystemSettingsStruct.h"
#include "LoggingMacros.h"
#include "core/SystemResourceProvider.h"
#include "core/ModbusCoordinator.h"
#include "shared/SharedSensorReadings.h"
#include "shared/Temperature.h"
#include "events/SystemEventsGenerated.h"
#include <TaskManager.h>
#include <MutexGuard.h>
#include "freertos/timers.h"
#include "hal/HardwareAbstractionLayer.h"
#include <climits>  // For ULONG_MAX

static const char* TAG = "ANDRTF3";

// Task handle for coordinator notifications
static TaskHandle_t andrtf3TaskHandle = nullptr;

// Flag to enable/disable coordinator mode
static constexpr bool USE_MODBUS_COORDINATOR = true;

// Process temperature reading through HAL
static bool processTemperatureReading() {
    // Get HAL instance and room temperature sensor
    auto& hal = HAL::HardwareAbstractionLayer::getInstance();
    const auto& config = hal.getConfig();
    
    if (!config.roomTempSensor) {
        LOG_ERROR(TAG, "Room temperature sensor not configured in HAL!");
        return false;
    }
    
    // Read temperature through HAL interface
    auto reading = config.roomTempSensor->readTemperature();
    
    if (reading.valid) {
        // Convert from float to fixed-point format (x10)
        int16_t temperature = static_cast<int16_t>(reading.temperature * 10.0f);

        // Apply temperature compensation from settings
        Temperature_t offset = SRP::getSystemSettings().roomTempOffset;
        temperature += offset;

        LOG_DEBUG(TAG, "Temperature: %d.%d°C (raw: %d.%d°C, offset: %d.%d°C)",
                  temperature / 10, abs(temperature % 10),
                  (int)reading.temperature, abs((int)(reading.temperature * 10) % 10),
                  offset / 10, abs(offset % 10));
        
        // Update shared sensor readings
        MutexGuard lock(SRP::getSensorReadingsMutex());
        if (lock) {
                SharedSensorReadings& readings = SRP::getSensorReadings();
                
                // Update inside temperature
                readings.insideTemp = temperature;
                readings.isInsideTempValid = true;
                
                // ANDRTF3 doesn't provide humidity
                readings.isInsideHumidityValid = false;
                
                // Update timestamp
                readings.lastUpdateTimestamp = reading.timestamp;
                
                // Set update bit to notify other tasks
                xEventGroupSetBits(SRP::getSensorEventGroup(), SystemEvents::SensorUpdate::INSIDE);
                
                LOG_DEBUG(TAG, "Updated shared sensor readings");
            } else {
                LOG_WARN(TAG, "Failed to acquire sensor data mutex");
            }
            
        return true;
    } else {
        LOG_DEBUG(TAG, "Temperature read failed through HAL");
    }
    
    return false;
}

void ANDRTF3Task(void* parameter) {
    (void)parameter;  // Not used
    
    andrtf3TaskHandle = xTaskGetCurrentTaskHandle();
    
    const TickType_t STATS_REPORT_INTERVAL = pdMS_TO_TICKS(60000); // 1 minute
    TickType_t xLastStatsTime = 0;
    uint32_t readCount = 0;
    uint32_t errorCount = 0;
    uint32_t errorsThisPeriod = 0;  // Track errors in current reporting period
    uint32_t consecutiveErrors = 0;
    
    LOG_INFO(TAG, "ANDRTF3 Task started");
    LOG_INFO(TAG, "Running on core %d", xPortGetCoreID());

    // Wait for HAL configuration (may be deferred to background task)
    auto& hal = HAL::HardwareAbstractionLayer::getInstance();
    const uint32_t HAL_WAIT_TIMEOUT_MS = 10000;  // 10 seconds max wait
    const uint32_t HAL_CHECK_INTERVAL_MS = 500;
    uint32_t waitTime = 0;

    while (waitTime < HAL_WAIT_TIMEOUT_MS) {
        const auto& config = hal.getConfig();
        if (config.roomTempSensor) {
            LOG_INFO(TAG, "HAL room temperature sensor configured after %lu ms", waitTime);
            break;
        }

        if (waitTime == 0) {
            LOG_INFO(TAG, "Waiting for HAL room temperature sensor configuration...");
        }

        vTaskDelay(pdMS_TO_TICKS(HAL_CHECK_INTERVAL_MS));
        waitTime += HAL_CHECK_INTERVAL_MS;
    }

    // Final check after wait
    const auto& config = hal.getConfig();
    if (!config.roomTempSensor) {
        LOG_ERROR(TAG, "Room temperature sensor not configured in HAL after %lu ms!", waitTime);
        vTaskDelete(NULL);
        return;
    }
    
    LOG_INFO(TAG, "HAL sensor configured: %s", config.roomTempSensor->getName());

    if (USE_MODBUS_COORDINATOR) {
        // Register with ModbusCoordinator
        auto& coordinator = ModbusCoordinator::getInstance();
        if (!coordinator.registerSensor(ModbusCoordinator::SensorType::ANDRTF3, andrtf3TaskHandle)) {
            LOG_ERROR(TAG, "Failed to register with ModbusCoordinator");
            vTaskDelete(NULL);
            return;
        }
        LOG_INFO(TAG, "Registered with ModbusCoordinator - waiting for coordinated reads");
    } else {
        // Create timer for periodic reads (5 seconds) - fallback mode
        TimerHandle_t sensorReadTimer = xTimerCreate(
            "ANDRTF3Timer",
            pdMS_TO_TICKS(5000), // 5 second interval
            pdTRUE,  // Auto-reload
            NULL,
            [](TimerHandle_t) { if (andrtf3TaskHandle) xTaskNotifyGive(andrtf3TaskHandle); }
        );
        
        if (sensorReadTimer == nullptr) {
            LOG_ERROR(TAG, "Failed to create sensor read timer");
            vTaskDelete(NULL);
            return;
        }
        
        // Start the timer
        if (xTimerStart(sensorReadTimer, pdMS_TO_TICKS(100)) != pdPASS) {
            LOG_ERROR(TAG, "Failed to start sensor read timer");
            xTimerDelete(sensorReadTimer, 0);
            vTaskDelete(NULL);
            return;
        }
        
        LOG_INFO(TAG, "Sensor read timer started with 5s interval (standalone mode)");
    }
    
    // Register with watchdog after initialization is complete
    // Timeout should be WATCHDOG_MULTIPLIER (3x) sensor read interval to allow for delays
    // Note: ANDRTF3 may have coordinator wait delays, so use 4x for extra margin
    uint32_t watchdogTimeout = SystemConstants::Timing::ANDRTF3_SENSOR_READ_INTERVAL_MS *
                               (SystemConstants::System::WATCHDOG_MULTIPLIER + 1);  // 5000 * 4 = 20s
    
    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::enabled(
        false,  // not critical
        watchdogTimeout
    );
    
    if (!SRP::getTaskManager().registerCurrentTaskWithWatchdog("ANDRTF3", wdtConfig)) {
        LOG_ERROR(TAG, "Failed to register with watchdog");
    } else {
        LOG_INFO(TAG, "Successfully registered with watchdog (%lu ms timeout)", watchdogTimeout);
        (void)SRP::getTaskManager().feedWatchdog();  // Feed immediately after registration
    }

    // NOTE: No initial read here - let the coordinator handle the first read
    // This prevents bus contention with MB8ART during startup
    LOG_INFO(TAG, "Waiting for first coordinated read from ModbusCoordinator");

    // Main event loop
    while (true) {
        // Wait for notification from coordinator with short timeout to allow watchdog feeding
        // Similar pattern to MB8ART task
        uint32_t ulNotificationValue = 0;
        bool notified = false;

        if (USE_MODBUS_COORDINATOR) {
            // Wait for coordinator notification with SHORT timeout to allow watchdog feeding
            // Watchdog timeout is 20s, so we use 2s wait intervals
            constexpr TickType_t COORDINATOR_WAIT_INTERVAL = pdMS_TO_TICKS(2000);
            constexpr int MAX_WAIT_ITERATIONS = 15;  // 15 * 2s = 30s max wait

            for (int waitIter = 0; waitIter < MAX_WAIT_ITERATIONS && !notified; waitIter++) {
                // Use xTaskNotifyWait to receive notification value (SensorType)
                if (xTaskNotifyWait(0, ULONG_MAX, &ulNotificationValue, COORDINATOR_WAIT_INTERVAL) == pdTRUE) {
                    notified = true;
                } else {
                    // Feed watchdog while waiting for coordinator
                    (void)SRP::getTaskManager().feedWatchdog();
                }
            }

            if (!notified) {
                // Coordinator didn't respond - continue waiting
                // Note: Direct read fallback removed - causes bus contention on shared Modbus
                // MB8ART, ANDRTF3, and RYN4 all share the same RS485 bus
                LOG_WARN(TAG, "No coordinator notification in 30s - continuing to wait");
            }
        } else {
            // Standalone mode - use short timeout
            if (xTaskNotifyWait(0, ULONG_MAX, &ulNotificationValue, pdMS_TO_TICKS(1000)) == pdTRUE) {
                notified = true;
            }
        }

        if (notified) {
            // Timer triggered - read temperature
            // This may block waiting for Modbus bus mutex, so feed watchdog after
            if (processTemperatureReading()) {
                readCount++;
                consecutiveErrors = 0;
            } else {
                errorCount++;
                errorsThisPeriod++;
                consecutiveErrors++;
                
                // Mark sensor as invalid after 3 consecutive failures
                if (consecutiveErrors >= 3) {
                    MutexGuard lock(SRP::getSensorReadingsMutex());
                    if (lock) {
                        SharedSensorReadings& readings = SRP::getSensorReadings();
                        readings.isInsideTempValid = false;
                        readings.isInsideHumidityValid = false;
                        // Clear temperature to invalid value to prevent using stale data
                        readings.insideTemp = TEMP_INVALID;
                        
                        // Set error bit
                        xEventGroupSetBits(SRP::getSensorEventGroup(), SystemEvents::SensorUpdate::INSIDE_ERROR);
                        
                        if (consecutiveErrors == 3) {
                            LOG_ERROR(TAG, "Sensor failed 3 times - marking as invalid");
                        }
                    }
                }
            }

            // Feed watchdog after Modbus operation (may have blocked on mutex)
            (void)SRP::getTaskManager().feedWatchdog();
        }

        // Check if we should report statistics
        TickType_t xCurrentTime = xTaskGetTickCount();
        if ((xCurrentTime - xLastStatsTime) >= STATS_REPORT_INTERVAL) {
            xLastStatsTime = xCurrentTime;
            
            // Only log statistics if there were errors in this period or in debug mode
            if (errorsThisPeriod > 0) {
                float successRate = readCount > 0 ? (100.0f * readCount / (readCount + errorCount)) : 0.0f;
                LOG_INFO(TAG, "Statistics - Reads: %lu, Errors: %lu (Period: %lu), Success rate: %d.%d%%",
                         readCount, errorCount, errorsThisPeriod,
                         (int)successRate, (int)(successRate * 10) % 10);
            } else {
                [[maybe_unused]] float successRate = readCount > 0 ? (100.0f * readCount / (readCount + errorCount)) : 0.0f;
                LOG_DEBUG(TAG, "Statistics - Reads: %lu, Errors: %lu, Success rate: %d.%d%%",
                          readCount, errorCount,
                          (int)successRate, (int)(successRate * 10) % 10);
            }
            
            // Reset period counter
            errorsThisPeriod = 0;
        }

        // Feed watchdog
        (void)SRP::getTaskManager().feedWatchdog();
    }
    
    // Cleanup (should never reach here)
    if (USE_MODBUS_COORDINATOR) {
        ModbusCoordinator::getInstance().unregisterSensor(ModbusCoordinator::SensorType::ANDRTF3);
    }
    vTaskDelete(NULL);
}