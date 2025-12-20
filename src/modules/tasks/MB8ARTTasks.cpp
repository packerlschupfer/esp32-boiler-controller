// src/modules/tasks/MB8ARTTasksEventDriven.cpp
// Event-driven version of MB8ART sensor reading task

#include "MB8ARTTasks.h"
#include "config/ProjectConfig.h"
#include "config/SystemConstants.h"
#include "config/SystemSettingsStruct.h"
#include <atomic>
#include "shared/SharedSensorReadings.h"
#include "shared/Temperature.h"
#include "shared/Pressure.h"
#include "events/SystemEventsGenerated.h"
#include "LoggingMacros.h"
#include "freertos/timers.h"
#include "core/SystemResourceProvider.h"
#include "core/ModbusCoordinator.h"
#include "utils/MutexRetryHelper.h"
#include <TaskManager.h>
#include <Watchdog.h>
#include "freertos/semphr.h"


static const char* TAG = "MB8ART";
// Task configuration constants from SystemConstants
// Timing strategy:
// - MB8ART: 2.5s (critical boiler temperatures need faster updates)
// - ANDRTF3: 5s (room temperature changes slowly)  
// - MQTT publish: 10s (reasonable for monitoring without flooding)
using namespace SystemConstants::Timing;

// Additional MB8ART specific constants
static constexpr uint32_t MB8ART_READ_TIMEOUT_MS = MB8ART_SENSOR_READ_INTERVAL_MS;  // Match read interval
static constexpr uint32_t MB8ART_INITIAL_DELAY_MS = 500;          // Initial delay before first read
static constexpr uint32_t MB8ART_TIMER_START_TIMEOUT_MS = 100;    // Timer start timeout

// Static variables
// THREAD-SAFE: Initialized once at startup, then read-only
static TaskHandle_t mb8artTaskHandle = nullptr;
static MB8ART* mb8artDevice = nullptr;

// Flag to enable/disable coordinator mode
static constexpr bool USE_MODBUS_COORDINATOR = true;

/**
 * @brief Event-driven MB8ART combined task
 * 
 * This version uses timer-based notifications instead of polling
 */
void MB8ARTCombinedTaskEventDriven(void* parameter) {
    MB8ART* mb8art = static_cast<MB8ART*>(parameter);
    if (!mb8art) {
        LOG_ERROR(TAG, "MB8ART device is null");
        vTaskDelete(NULL);
        return;
    }
    
    mb8artDevice = mb8art;
    mb8artTaskHandle = xTaskGetCurrentTaskHandle();
    
    // In async mode, MB8ART automatically sends temperature updates
    // We don't need to monitor event groups
    
    const TickType_t STATS_REPORT_INTERVAL = pdMS_TO_TICKS(SENSOR_DIAGNOSTICS_INTERVAL_MS);
    TickType_t xLastStatsTime = 0;
    TickType_t xLastDataTime = xTaskGetTickCount();
    uint32_t readCount = 0;
    uint32_t errorCount = 0;
    uint32_t errorsThisPeriod = 0;  // Track errors in current reporting period
    
    LOG_INFO(TAG, "MB8ART Combined Task started (async event-driven)");
    LOG_INFO(TAG, "Device in async mode - waiting for temperature updates");
    
    // In async mode, MB8ART automatically sends temperature updates
    // We don't need a timer to trigger reads
    LOG_INFO(TAG, "MB8ART in async mode - no polling timer needed");
    
    // Register with watchdog after initialization is complete
    // Timeout should be 4x sensor read interval to allow for delays
    uint32_t watchdogTimeout = MB8ART_SENSOR_READ_INTERVAL_MS * 4;  // 10 seconds for 2.5s interval
    
    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::enabled(
        false,  // not critical
        watchdogTimeout
    );
    if (!SRP::getTaskManager().registerCurrentTaskWithWatchdog("MB8ART", wdtConfig)) {
        LOG_ERROR(TAG, "Failed to register with watchdog");
    } else {
        LOG_INFO(TAG, "Successfully registered with watchdog (%lu ms timeout)", watchdogTimeout);
        (void)SRP::getTaskManager().feedWatchdog();  // Feed immediately after registration
    }
    
    if (USE_MODBUS_COORDINATOR) {
        // Register with ModbusCoordinator
        auto& coordinator = ModbusCoordinator::getInstance();
        if (!coordinator.registerSensor(ModbusCoordinator::SensorType::MB8ART, mb8artTaskHandle)) {
            LOG_ERROR(TAG, "Failed to register with ModbusCoordinator");
            vTaskDelete(NULL);
            return;
        }
        LOG_INFO(TAG, "Registered with ModbusCoordinator - waiting for coordinated reads");
    }
    
    // Main event loop - async event-driven with periodic requests
    TickType_t xLastRequestTime = 0;
    const TickType_t REQUEST_INTERVAL = pdMS_TO_TICKS(MB8ART_SENSOR_READ_INTERVAL_MS);  // 2.5 seconds
    
    while (true) {
        TickType_t currentTime = xTaskGetTickCount();

        // Wait for notification from coordinator or timeout for standalone mode
        uint32_t notificationValue = 0;
        BaseType_t notified = pdFALSE;

        if (USE_MODBUS_COORDINATOR) {
            // Wait for coordinator notification with SHORT timeout to allow watchdog feeding
            // Watchdog timeout is 10s, so we use 2s wait intervals
            constexpr TickType_t COORDINATOR_WAIT_INTERVAL = pdMS_TO_TICKS(2000);
            constexpr int MAX_WAIT_ITERATIONS = 15;  // 15 * 2s = 30s max wait
            static uint8_t consecutiveCoordinatorTimeouts = 0;
            constexpr uint8_t MAX_COORDINATOR_TIMEOUTS = 2;  // 2 * 30s = 60s before failure

            for (int waitIter = 0; waitIter < MAX_WAIT_ITERATIONS && !notified; waitIter++) {
                notified = xTaskNotifyWait(0, ULONG_MAX, &notificationValue, COORDINATOR_WAIT_INTERVAL);
                if (!notified) {
                    // Feed watchdog while waiting for coordinator
                    (void)SRP::getTaskManager().feedWatchdog();
                }
            }

            if (!notified) {
                consecutiveCoordinatorTimeouts++;
                if (consecutiveCoordinatorTimeouts >= MAX_COORDINATOR_TIMEOUTS) {
                    // Coordinator failed for 60s+ - signal failure for monitoring/recovery
                    LOG_ERROR(TAG, "COORDINATOR FAILURE: No notification in %ds - signaling error",
                              consecutiveCoordinatorTimeouts * 30);
                    xEventGroupSetBits(SRP::getSensorEventGroup(), SystemEvents::SensorUpdate::DATA_ERROR);
                    // Note: Direct read fallback removed - caused bus contention with coordinator
                    // Recovery requires coordinator restart or system reboot
                } else {
                    LOG_WARN(TAG, "No coordinator notification in 30s (timeout %d/%d)",
                             consecutiveCoordinatorTimeouts, MAX_COORDINATOR_TIMEOUTS);
                }
            } else {
                // Reset timeout counter on successful notification
                consecutiveCoordinatorTimeouts = 0;
            }
        } else {
            // Standalone mode - check time elapsed
            if ((currentTime - xLastRequestTime) >= REQUEST_INTERVAL) {
                notified = pdTRUE;
                xLastRequestTime = currentTime;
            }
        }
        
        if (notified) {
            // Request temperatures from all sensors
            // This may block waiting for bus mutex, so feed watchdog after
            auto reqResult = mb8art->requestTemperatures();
            (void)SRP::getTaskManager().feedWatchdog();  // Feed after Modbus operation
            if (!reqResult) {
                LOG_WARN(TAG, "Failed to request temperatures");
                errorCount++;
                errorsThisPeriod++;
            }
        }

        // Check for temperature updates (with a short delay to allow processing)
        vTaskDelay(pdMS_TO_TICKS(100));  // Check every 100ms
        
        // Check if MB8ART has new temperature data available
        if (mb8art->hasAnyUpdatePending()) {
            // Get the temperature data
            auto result = mb8art->getData(IDeviceInstance::DeviceDataType::TEMPERATURE);
            
            if (result.isOk() && !result.value().empty()) {
                updateSensorData(result.value());
                readCount++;
                xLastDataTime = currentTime;

                // Only log every 10th successful read to reduce log spam
                if ((readCount % 10) == 0) {
                    LOG_DEBUG(TAG, "Processed %zu temperature values (read #%lu)",
                              result.value().size(), readCount);
                }

                // Clear any error bits since we got good data
                xEventGroupClearBits(SRP::getSensorEventGroup(), SystemEvents::SensorUpdate::DATA_ERROR);
            } else {
                errorCount++;
                errorsThisPeriod++;
                LOG_ERROR(TAG, "Failed to get temperature data - error: %d",
                          static_cast<int>(result.error()));
                xEventGroupSetBits(SRP::getSensorEventGroup(), SystemEvents::SensorUpdate::DATA_ERROR);
            }
        }
        
        // Check for stale data (no updates for > 5 seconds)
        if ((currentTime - xLastDataTime) > pdMS_TO_TICKS(5000)) {
            LOG_WARN(TAG, "No temperature updates for 5 seconds - data may be stale");
            xEventGroupSetBits(SRP::getSensorEventGroup(), SystemEvents::SensorUpdate::DATA_ERROR);
            errorCount++;
            errorsThisPeriod++;
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
            
            // Report device health
            if (errorCount > readCount / 10) {  // More than 10% errors
                LOG_WARN(TAG, "High error rate detected");
                xEventGroupSetBits(SRP::getSensorEventGroup(), SystemEvents::SensorUpdate::DATA_ERROR);
            }
            
            // Reset period counter
            errorsThisPeriod = 0;
        }

        // Feed watchdog
        (void)SRP::getTaskManager().feedWatchdog();
    }
    
    // Cleanup (should never reach here)
    if (USE_MODBUS_COORDINATOR) {
        ModbusCoordinator::getInstance().unregisterSensor(ModbusCoordinator::SensorType::MB8ART);
    }
    vTaskDelete(NULL);
}

/**
 * @brief Request an immediate sensor read outside of the normal timer interval
 * @return true if request was sent successfully
 */
bool requestImmediateSensorRead() {
    if (mb8artTaskHandle != nullptr) {
        xTaskNotifyGive(mb8artTaskHandle);
        return true;
    }
    return false;
}

/**
 * @brief Change the sensor read interval dynamically
 * @param newIntervalMs New interval in milliseconds
 * @return true if successful (not applicable in async mode)
 */
bool changeSensorReadInterval(uint32_t newIntervalMs) {
    // In async mode, MB8ART controls its own timing
    // This function is kept for API compatibility
    LOG_WARN(TAG, "Cannot change interval - MB8ART is in async mode");
    return false;
}

#ifdef USE_REAL_PRESSURE_SENSOR
/**
 * @brief Convert 4-20mA current reading to pressure in BAR
 * @param currentMA Current reading in milliamps
 * @return Pressure in BAR (-1.0 if sensor disconnected/failed)
 */
static float convertCurrentToPressure(float currentMA) {
    using namespace SystemConstants::Hardware::PressureSensor;

    // Industrial standard 4-20mA pressure sensor
    // Current range and pressure range defined in SystemConstants

    // Check for sensor disconnection (open circuit typically shows <3.5mA)
    if (currentMA < CURRENT_FAULT_THRESHOLD_MA) {
        static uint32_t lastDisconnectLog = 0;
        if (millis() - lastDisconnectLog > 10000) {  // Log every 10s to avoid spam
            LOG_ERROR("MB8ARTTask", "Pressure sensor DISCONNECTED - current %.2f mA (threshold %.1f mA)",
                     currentMA, CURRENT_FAULT_THRESHOLD_MA);
            ErrorHandler::logError("MB8ARTTask", SystemError::SENSOR_FAILURE,
                                  "Pressure sensor disconnected (open circuit)");
            lastDisconnectLog = millis();
        }
        return -1.0f;  // Invalid reading
    }

    // Check for short circuit (>20.5mA indicates wiring fault)
    constexpr float SHORT_CIRCUIT_THRESHOLD_MA = 20.5f;
    if (currentMA > SHORT_CIRCUIT_THRESHOLD_MA) {
        static uint32_t lastShortLog = 0;
        if (millis() - lastShortLog > 10000) {
            LOG_ERROR("MB8ARTTask", "Pressure sensor SHORT CIRCUIT - current %.2f mA (max %.1f mA)",
                     currentMA, SHORT_CIRCUIT_THRESHOLD_MA);
            ErrorHandler::logError("MB8ARTTask", SystemError::SENSOR_FAILURE,
                                  "Pressure sensor short circuit");
            lastShortLog = millis();
        }
        return -1.0f;  // Invalid reading
    }

    // Linear conversion: pressure = (current - 4mA) / (16mA) * (5 BAR)
    float pressure = ((currentMA - CURRENT_MIN_MA) / CURRENT_RANGE_MA) * PRESSURE_RANGE_BAR;

    // Validate against physical system limits (typical heating system 0-6 BAR)
    if (pressure < 0.0f || pressure > 6.0f) {
        static uint32_t lastRangeLog = 0;
        if (millis() - lastRangeLog > 30000) {  // Log every 30s
            LOG_WARN("MB8ARTTask", "Pressure out of physical range: %.2f BAR", pressure);
            lastRangeLog = millis();
        }
        // Still return the clamped value - may be calibration issue
    }

    // Clamp to configured sensor range
    if (pressure < PRESSURE_AT_MIN_CURRENT) pressure = PRESSURE_AT_MIN_CURRENT;
    if (pressure > PRESSURE_AT_MAX_CURRENT) pressure = PRESSURE_AT_MAX_CURRENT;

    return pressure;
}
#endif // USE_REAL_PRESSURE_SENSOR

/**
 * @brief Update shared sensor data from MB8ART temperature readings
 * @param temperatureData Vector of temperature values from sensor
 */
void updateSensorData(const std::vector<float>& temperatureData) {
    static std::atomic<bool> firstRead{true};
    bool anySensorError = false;
    
    
    // Acquire mutex to update shared sensor readings
    auto guard = MutexRetryHelper::acquireGuard(
        SRP::getSensorReadingsMutex(),
        "SensorReadings-MB8ART"
    );
    if (guard) {
        // NOTE: With unified mapping architecture, MB8ART library writes DIRECTLY
        // to SharedSensorReadings via bound pointers during sensor reads.
        // This code is now redundant - data is already in SharedSensorReadings.
        // Keeping minimal logging for monitoring purposes.

        LOG_DEBUG("MB8ARTTask", "Temperature data available (%d channels)", temperatureData.size());

        // Handle channel 7 as pressure sensor (if present in data)
#ifdef USE_REAL_PRESSURE_SENSOR
        const size_t PRESSURE_CHANNEL = 7;
        if (temperatureData.size() > PRESSURE_CHANNEL) {
            // MB8ART channel 7 should return current in mA for 4-20mA sensors
            float currentMA = temperatureData[PRESSURE_CHANNEL];
            
            // Convert 4-20mA current to pressure
            float pressure = convertCurrentToPressure(currentMA);
            
            // Check if sensor is connected and working
            if (pressure >= 0.0f) {
                // Valid pressure reading - convert to fixed-point and apply offset
                Pressure_t rawPressure = pressureFromFloat(pressure);
                int16_t pressureOffset = SRP::getSystemSettings().pressureOffset;
                SRP::getSensorReadings().systemPressure = rawPressure + pressureOffset;
                SRP::getSensorReadings().isSystemPressureValid = true;
                SRP::getSensorReadings().lastPressureUpdateTimestamp = millis();
                
                Pressure_t fixedPressure = SRP::getSensorReadings().systemPressure;
                LOG_DEBUG("MB8ARTTask", "Ch7 (Pressure): %d.%02d BAR (%d.%02d mA)", 
                          fixedPressure / 100, abs(fixedPressure % 100),
                          (int)currentMA, (int)(currentMA * 100) % 100);
                
                // Set pressure update event bit
                xEventGroupSetBits(SRP::getSensorEventGroup(), SystemEvents::SensorUpdate::PRESSURE);
                
                // Check for pressure alarms (wider alarm thresholds than operating limits)
                using namespace SystemConstants::Safety::Pressure;
                Pressure_t pressureFixed = SRP::getSensorReadings().systemPressure;
                if (pressureFixed < ALARM_MIN || pressureFixed > ALARM_MAX) {
                    LOG_WARN("MB8ARTTask", "Pressure alarm: %d.%02d BAR (alarm limits: %d.%02d-%d.%02d BAR)",
                             pressureFixed / 100, abs(pressureFixed % 100),
                             ALARM_MIN / 100, abs(ALARM_MIN % 100),
                             ALARM_MAX / 100, abs(ALARM_MAX % 100));
                    xEventGroupSetBits(SRP::getBurnerEventGroup(), SystemEvents::Burner::ERROR_PRESSURE);
                } else {
                    // Clear pressure alarm bit if pressure is normal
                    xEventGroupClearBits(SRP::getBurnerEventGroup(), SystemEvents::Burner::ERROR_PRESSURE);
                    // Set pressure OK bit
                    xEventGroupSetBits(SRP::getBurnerEventGroup(), SystemEvents::Burner::PRESSURE_OK);
                }
            } else {
                // Sensor disconnected or failed (current < 4mA)
                LOG_ERROR("MB8ARTTask", "Pressure sensor fault detected (%d.%02d mA)", 
                          (int)currentMA, (int)(currentMA * 100) % 100);
                SRP::getSensorReadings().isSystemPressureValid = false;
                SRP::getSensorReadings().systemPressure = PRESSURE_INVALID;
                
                // Set pressure error event
                xEventGroupSetBits(SRP::getSensorEventGroup(), SystemEvents::SensorUpdate::PRESSURE_ERROR);
                anySensorError = true;
            }
        }
#else
        // Fake pressure data for testing - USE_REAL_PRESSURE_SENSOR not defined
        {
            using namespace SystemConstants::Simulation;

            // THREAD-SAFE: Only accessed by MB8ARTTask (single task)
            static Pressure_t fakePressure = FAKE_PRESSURE_NOMINAL;
            static uint32_t lastFakeUpdate = 0;
            uint32_t now = millis();

            // Update periodically with small variations to simulate real sensor
            if (now - lastFakeUpdate > FAKE_PRESSURE_UPDATE_INTERVAL_MS) {
                // Add random variation to simulate sensor noise
                int variation = (rand() % (FAKE_PRESSURE_VARIATION * 2 + 1)) - FAKE_PRESSURE_VARIATION;
                fakePressure += variation;

                // Clamp to realistic range
                if (fakePressure < FAKE_PRESSURE_MIN) fakePressure = FAKE_PRESSURE_MIN;
                if (fakePressure > FAKE_PRESSURE_MAX) fakePressure = FAKE_PRESSURE_MAX;

                lastFakeUpdate = now;
            }

            // Set fake but safe pressure value
            SRP::getSensorReadings().systemPressure = fakePressure;
            SRP::getSensorReadings().isSystemPressureValid = true;
            SRP::getSensorReadings().lastPressureUpdateTimestamp = now;

            // Log once at startup to indicate fake sensor mode
            static bool firstFakeLog = true;
            if (firstFakeLog) {
                LOG_INFO("MB8ARTTask", "Using FAKE pressure data: %d.%02d BAR (sensor not installed)",
                         fakePressure / 100, abs(fakePressure % 100));
                firstFakeLog = false;
            }
            
            // Set pressure update event bit
            xEventGroupSetBits(SRP::getSensorEventGroup(), SystemEvents::SensorUpdate::PRESSURE);
            
            // Clear any pressure alarm bits (fake data is always in safe range)
            xEventGroupClearBits(SRP::getBurnerEventGroup(), SystemEvents::Burner::ERROR_PRESSURE);
            // Set pressure OK bit for fake data
            xEventGroupSetBits(SRP::getBurnerEventGroup(), SystemEvents::Burner::PRESSURE_OK);
        }
#endif
        
        // Update timestamp
        SRP::getSensorReadings().lastUpdateTimestamp = millis();
        
        // Set event bits BEFORE releasing mutex to ensure data consistency
        // This prevents other tasks from seeing the event before data is ready
        if (firstRead.exchange(false)) {
            xEventGroupSetBits(SRP::getSensorEventGroup(), SystemEvents::SensorUpdate::FIRST_READ_COMPLETE);
        }
        
        // Always set data available bit after successful update
        xEventGroupSetBits(SRP::getSensorEventGroup(), SystemEvents::SensorUpdate::DATA_AVAILABLE);

        // Mutex is automatically released when guard goes out of scope (RAII)

        // Log after mutex release to avoid holding mutex during logging
        static std::atomic<bool> firstReadLogged{false};
        if (!firstReadLogged.load() && !firstRead.load()) {  // Only log once on first read completion
            if (firstReadLogged.exchange(true) == false) {
                LOG_INFO(TAG, "First sensor read completed successfully - sensors initialized");
            }
        }
    } else {
        LOG_ERROR(TAG, "Failed to lock sensor readings mutex");
        anySensorError = true;
    }
    
    // Set error bit if needed
    if (anySensorError) {
        xEventGroupSetBits(SRP::getSensorEventGroup(), SystemEvents::SensorUpdate::DATA_ERROR);
    }
}

// Wrapper function for compatibility with standard task interface
void MB8ARTTask(void* parameter) {
    MB8ARTCombinedTaskEventDriven(parameter);
}