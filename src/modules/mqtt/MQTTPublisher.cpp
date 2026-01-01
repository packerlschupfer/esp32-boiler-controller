// src/modules/mqtt/MQTTPublisher.cpp
#include "MQTTPublisher.h"
#include "MQTTTopics.h"
#include "modules/tasks/MQTTTask.h"
#include "core/SystemResourceProvider.h"
#include "shared/SharedSensorReadings.h"
#include "shared/SharedRelayReadings.h"
#include "shared/Temperature.h"
#include "events/SystemEventsGenerated.h"
#include "modules/control/TemperatureSensorFallback.h"
#include "modules/control/BurnerStateMachine.h"
#include "utils/MemoryPool.h"
#include <SemaphoreGuard.h>
#include <ArduinoJson.h>
#include <esp_log.h>
#include <ESP.h>

static const char* TAG = "MQTTPub";

namespace MQTTPublisher {

void publishSystemStatus() {
    auto mqttManager = SRP::getMQTTManager();
    if (!mqttManager || !mqttManager->isConnected()) {
        return;
    }

    auto mqttMutex = SRP::getMQTTMutex();
    SemaphoreGuard guard(mqttMutex, pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "Failed to acquire mutex for status publish");
        return;
    }

    // Create JSON status message
    JsonDocument doc;  // ArduinoJson v7
    doc["timestamp"] = millis();

    uint32_t heapFree = ESP.getFreeHeap();
    uint32_t heapMaxBlock = ESP.getMaxAllocHeap();
    doc["heap_free"] = heapFree;
    doc["heap_min"] = ESP.getMinFreeHeap();
    doc["heap_max_blk"] = heapMaxBlock;  // Largest free block (for fragmentation analysis)

    // Round 16 Issue #5: Add heap fragmentation percentage
    // Fragmentation = 100 - (max_block * 100 / free_heap)
    // Lower is better: 0% = no fragmentation, 100% = completely fragmented
    // Use 64-bit arithmetic to prevent overflow on large heap values
    uint8_t fragPct = (heapFree > 0) ?
        (100 - static_cast<uint8_t>((static_cast<uint64_t>(heapMaxBlock) * 100ULL) / heapFree)) : 100;
    doc["heap_frag"] = fragPct;

    doc["uptime"] = millis() / 1000;

    // Add health monitor data
    // Add basic health data - HealthMonitor not available in this version
    doc["health"]["tasks"] = uxTaskGetNumberOfTasks();
    doc["health"]["stack_hwm"] = uxTaskGetStackHighWaterMark(NULL);

    auto buffer = MemoryPools::getLogBuffer();
    if (!buffer) {
        LOG_ERROR(TAG, "Failed to allocate buffer for health data");
        return;
    }

    size_t written = serializeJson(doc, buffer.data(), buffer.size());
    if (written == 0 || written >= buffer.size()) {
        LOG_ERROR(TAG, "JSON serialization failed or truncated for health data");
        return;
    }

    // Queue for publishing with MEDIUM priority
    MQTTTask::publish(MQTT_STATUS_HEALTH, buffer.c_str(), 0, false, MQTTPriority::PRIORITY_MEDIUM);
}

void publishSensorData() {
    auto mqttManager = SRP::getMQTTManager();
    if (!mqttManager || !mqttManager->isConnected()) {
        return;
    }

    // Get sensor data with timeout to avoid blocking
    SemaphoreGuard guard(SRP::getSensorReadingsMutex(), pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "Failed to acquire sensor mutex for MQTT publish");
        return;
    }

    // Create local copy of sensor data
    SharedSensorReadings sensors = SRP::getSensorReadings();

    // SemaphoreGuard releases automatically when it goes out of scope

    // Create JSON message
    JsonDocument doc;  // ArduinoJson v7

    // Temperature values are in tenths of degrees Celsius (int16_t)
    // Example: 273 = 27.3°C, -50 = -5.0°C

    // Compact format: Use shorter keys for smaller payload
    JsonObject temps = doc["t"].to<JsonObject>();  // temperatures
    temps["bo"] = sensors.boilerTempOutput;     // boiler output
    temps["br"] = sensors.boilerTempReturn;     // boiler return
    temps["wt"] = sensors.waterHeaterTempTank;  // water tank
    temps["o"] = sensors.outsideTemp;           // outside

    // Optional sensors (enable via ENABLE_SENSOR_* flags)
#ifdef ENABLE_SENSOR_WATER_TANK_TOP
    temps["wtt"] = sensors.waterTankTopTemp;    // water tank top
#endif
#ifdef ENABLE_SENSOR_WATER_RETURN
    temps["wr"] = sensors.waterHeaterTempReturn; // water return
#endif
#ifdef ENABLE_SENSOR_HEATING_RETURN
    temps["hr"] = sensors.heatingTempReturn;    // heating return
#endif

    // Only include inside temp if valid
    if (tempIsValid(sensors.insideTemp)) {
        temps["i"] = sensors.insideTemp;        // inside
    }

    // Add burner target temperature (from BurnerStateMachine)
    bool burnerDemand = false;
    Temperature_t burnerTarget = 0;
    if (BurnerStateMachine::getHeatDemandState(burnerDemand, burnerTarget)) {
        temps["bt"] = burnerTarget;             // burner target
    }

    // Include system pressure if valid (in hundredths of BAR for precision)
    if (sensors.isSystemPressureValid) {
        doc["p"] = sensors.systemPressure;      // pressure in hundredths of BAR
    }

    // Get relay status
    if (xSemaphoreTake(SRP::getRelayReadingsMutex(), pdMS_TO_TICKS(50)) == pdTRUE) {
        SharedRelayReadings relays = SRP::getRelayReadings();
        xSemaphoreGive(SRP::getRelayReadingsMutex());

        // Compact format: combine relay states into a single byte
        // Bit 0: burner, 1: heating_pump, 2: water_pump, 3: half_power, 4: water_mode
        uint8_t relayBits = 0;
        if (relays.relayBurnerEnable) relayBits |= 0x01;
        if (relays.relayHeatingPump) relayBits |= 0x02;
        if (relays.relayWaterPump) relayBits |= 0x04;
        if (relays.relayPowerBoost) relayBits |= 0x08;
        if (relays.relayWaterMode) relayBits |= 0x10;

        doc["r"] = relayBits;  // relays as single byte
    }

    // Add system state as compact byte (like relays)
    // s = system state: bit0=system_enabled, bit1=heating_enabled, bit2=heating_on,
    //                   bit3=water_enabled, bit4=water_on, bit5=water_priority
    EventBits_t systemState = SRP::getSystemStateEventBits();
    uint8_t stateBits = 0;
    if (systemState & SystemEvents::SystemState::BOILER_ENABLED) stateBits |= 0x01;
    if (systemState & SystemEvents::SystemState::HEATING_ENABLED) stateBits |= 0x02;
    if (systemState & SystemEvents::SystemState::HEATING_ON) stateBits |= 0x04;
    if (systemState & SystemEvents::SystemState::WATER_ENABLED) stateBits |= 0x08;
    if (systemState & SystemEvents::SystemState::WATER_ON) stateBits |= 0x10;
    if (systemState & SystemEvents::SystemState::WATER_PRIORITY) stateBits |= 0x20;
    doc["s"] = stateBits;

    // Add sensor fallback status for degraded mode notification
    // sf = sensor fallback: 0=STARTUP (waiting), 1=NORMAL (OK), 2=SHUTDOWN (degraded)
    auto fallbackMode = TemperatureSensorFallback::getCurrentMode();
    doc["sf"] = static_cast<uint8_t>(fallbackMode);

    // If in degraded mode, add which sensors are missing
    if (fallbackMode != TemperatureSensorFallback::FallbackMode::NORMAL) {
        const auto& status = TemperatureSensorFallback::getStatus();
        uint8_t missingSensors = 0;
        if (status.missingBoilerOutput) missingSensors |= 0x01;
        if (status.missingBoilerReturn) missingSensors |= 0x02;
        if (status.missingWaterTemp) missingSensors |= 0x04;
        if (status.missingRoomTemp) missingSensors |= 0x08;
        doc["sm"] = missingSensors;  // sensor missing bits
    }

    // Use the larger JSON buffer pool for sensor data
    auto buffer = MemoryPools::jsonBufferPool.allocate();
    if (!buffer) {
        LOG_ERROR(TAG, "Failed to allocate buffer for sensor data");
        return;
    }

    size_t written = serializeJson(doc, buffer->data, sizeof(buffer->data));
    if (written == 0 || written >= sizeof(buffer->data)) {
        LOG_ERROR(TAG, "JSON serialization failed or truncated (wrote %zu/%zu bytes)",
                 written, sizeof(buffer->data));
        MemoryPools::jsonBufferPool.deallocate(buffer);
        return;
    }

    // Queue for publishing with HIGH priority
    MQTTTask::publish(MQTT_STATUS_SENSORS, buffer->data, 0, false, MQTTPriority::PRIORITY_HIGH);

    // Return buffer to pool
    MemoryPools::jsonBufferPool.deallocate(buffer);
}

void publishSystemState() {
    // System state is now included in sensor data as compact byte "s"
    // s = bit0:system_enabled, bit1:heating_enabled, bit2:heating_on,
    //     bit3:water_enabled, bit4:water_on, bit5:water_priority
    // Individual state topics are published by handleControlCommand() when state changes
    // This function is kept for compatibility but does nothing - state is in sensor data
    LOG_DEBUG(TAG, "System state included in sensor data 's' field");
}

} // namespace MQTTPublisher
