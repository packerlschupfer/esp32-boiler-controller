// modules/SensorTask.cpp
#include "SensorTask.h"
#include "events/SystemEventsGenerated.h"
#include "shared/SharedSensorReadings.h"
#include "config/SystemConstants.h"
#include "LoggingMacros.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "utils/Utils.h"
#include "core/SystemResourceProvider.h"

// Bounded mutex timeout - NEVER use MUTEX_TIMEOUT to prevent deadlock
static constexpr TickType_t MUTEX_TIMEOUT = pdMS_TO_TICKS(SystemConstants::Timing::MUTEX_DEFAULT_TIMEOUT_MS);

// Note: All resources are now accessed through SystemResourceProvider (SRP)

// No external declarations needed - using SRP methods

// Function to process Modbus sensor data
void handleModbusSensorData() {
    MB8ART* mb8art = SRP::getMB8ART();
    if (!mb8art) {
        LOG_ERROR("SensorTask", "MB8ART instance not available");
        return;
    }
    
    if (mb8art->requestData()) {
        auto dataResult = mb8art->getData(IDeviceInstance::DeviceDataType::TEMPERATURE);

        if (dataResult.isOk()) {
            // NOTE: With unified mapping architecture, MB8ART library writes DIRECTLY
            // to SharedSensorReadings via bound pointers. This task is now redundant.
            // Data is already in SharedSensorReadings - no copy needed.
            LOG_DEBUG("SensorTask", "Sensor data updated (via MB8ART direct binding)");
        } else {
            LOG_ERROR("SensorTask", "Modbus data retrieval failed");
            xEventGroupSetBits(SRP::getSensorEventGroup(), SystemEvents::SensorUpdate::DATA_ERROR);
        }
    }
}

// BLE thermometer data processing removed - using MB8ART channel 7 for inside temperature

// Sensor task logic
void SensorTask(void* parameter) {
    const TickType_t xDelay = pdMS_TO_TICKS(5000); // 5-second delay between reads

    while (true) {
        // Instead of directly handling Modbus data, wait for the event bit
        // that indicates data is available from the ModbusRequestTask/SensorDataProcessTask
        EventBits_t bits = xEventGroupWaitBits(
            SRP::getSensorEventGroup(),
            SystemEvents::SensorUpdate::DATA_AVAILABLE | SystemEvents::SensorUpdate::DATA_ERROR,
            pdTRUE,  // Clear bits after reading
            pdFALSE, // Don't wait for all bits
            xDelay
        );

        if (bits & SystemEvents::SensorUpdate::DATA_AVAILABLE) {
            // Data is already processed by SensorDataProcessTask
            // Just log success if needed
            LOG_DEBUG("SensorTask", "New sensor data available");
        }
        else if (bits & SystemEvents::SensorUpdate::DATA_ERROR) {
            LOG_WARN("SensorTask", "Sensor data error reported");
        }
        else {
            // Timeout occurred - this is normal and allows watchdog feeding
            LOG_DEBUG("SensorTask", "Event wait timeout - feeding watchdog");
        }

        // No additional delay needed - xEventGroupWaitBits already blocks
    }
}
