// src/utils/ErrorContextCapture.cpp
#include "ErrorContextSnapshot.h"
#include "LoggingMacros.h"
#include "modules/tasks/MQTTTask.h"
#include "MQTTTopics.h"
#include "core/SystemResourceProvider.h"
#include "events/SystemEventsGenerated.h"  // C5: SystemState bit constants
#include "shared/SharedSensorReadings.h"
#include "shared/RelayState.h"
#include <ArduinoJson.h>
#include <esp_system.h>

static const char* TAG = "ErrorContext";

bool ErrorContextCapture::isCriticalError(SystemError error) {
    return (error == SystemError::SYSTEM_FAILSAFE_TRIGGERED ||
            error == SystemError::IGNITION_FAILURE ||
            error == SystemError::RELAY_SAFETY_INTERLOCK ||
            error == SystemError::TEMPERATURE_CRITICAL ||
            error == SystemError::SYSTEM_OVERHEATED ||
            error == SystemError::EMERGENCY_STOP);
}

ErrorContextSnapshot ErrorContextCapture::captureSnapshot(
    SystemError errorCode,
    const char* component,
    const char* description
) {
    ErrorContextSnapshot snapshot = {};

    // Error identification
    snapshot.errorCode = errorCode;
    strncpy(snapshot.component, component ? component : "UNKNOWN", sizeof(snapshot.component) - 1);
    strncpy(snapshot.description, description ? description : "", sizeof(snapshot.description) - 1);
    snapshot.timestamp = millis();

    // Task context
    TaskHandle_t currentTask = xTaskGetCurrentTaskHandle();
    const char* name = pcTaskGetName(currentTask);
    strncpy(snapshot.taskName, name ? name : "UNKNOWN", sizeof(snapshot.taskName) - 1);
    snapshot.taskPriority = static_cast<uint8_t>(uxTaskPriorityGet(currentTask));

    // Memory context
    snapshot.freeHeap = esp_get_free_heap_size();
    snapshot.minFreeHeap = esp_get_minimum_free_heap_size();
    snapshot.largestFreeBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

    // Event groups snapshot (use SRP to access)
    auto systemStateEG = SRP::getSystemStateEventGroup();
    auto burnerRequestEG = SRP::getBurnerRequestEventGroup();
    auto sensorEG = SRP::getSensorEventGroup();
    auto relayEG = SRP::getRelayEventGroup();

    snapshot.systemStateBits = systemStateEG ? xEventGroupGetBits(systemStateEG) : 0;
    snapshot.burnerRequestBits = burnerRequestEG ? xEventGroupGetBits(burnerRequestEG) : 0;
    snapshot.sensorEventBits = sensorEG ? xEventGroupGetBits(sensorEG) : 0;
    snapshot.relayEventBits = relayEG ? xEventGroupGetBits(relayEG) : 0;

    // Sensor snapshot (with mutex protection)
    if (SRP::takeSensorReadingsMutex(pdMS_TO_TICKS(10))) {
        const auto& readings = SRP::getSensorReadings();
        snapshot.boilerTempOutput = readings.boilerTempOutput;
        snapshot.boilerTempReturn = readings.boilerTempReturn;
        snapshot.waterHeaterTempTank = readings.waterHeaterTempTank;
        snapshot.systemPressure = readings.systemPressure;
        snapshot.sensorsValid = readings.isBoilerTempOutputValid;
        SRP::giveSensorReadingsMutex();
    } else {
        snapshot.sensorsValid = false;
    }

    // Relay snapshot (global relay state, atomics are thread-safe)
    snapshot.relayDesiredState = g_relayState.desired.load();
    snapshot.relayActualState = g_relayState.actual.load();
    snapshot.relayMismatchMask = snapshot.relayDesiredState ^ snapshot.relayActualState;

    // Burner and system state (from event bits using SystemState constants)
    // C5 fix: use proper constants instead of hardcoded positions (old code read wrong bits)
    snapshot.burnerActive = (snapshot.systemStateBits & SystemEvents::SystemState::BOILER_ENABLED) != 0;
    snapshot.heatingActive = (snapshot.systemStateBits & SystemEvents::SystemState::HEATING_ON) != 0;
    snapshot.waterActive = (snapshot.systemStateBits & SystemEvents::SystemState::WATER_ON) != 0;

    return snapshot;
}

void ErrorContextCapture::publishErrorContext(const ErrorContextSnapshot& snapshot) {
    JsonDocument doc;

    // Error info
    doc["error_code"] = static_cast<int>(snapshot.errorCode);
    doc["component"] = snapshot.component;
    doc["description"] = snapshot.description;
    doc["timestamp"] = snapshot.timestamp;

    // Task context
    JsonObject task = doc["task"].to<JsonObject>();
    task["name"] = snapshot.taskName;
    task["priority"] = snapshot.taskPriority;

    // Memory
    JsonObject mem = doc["memory"].to<JsonObject>();
    mem["free"] = snapshot.freeHeap;
    mem["min_free"] = snapshot.minFreeHeap;
    mem["largest_block"] = snapshot.largestFreeBlock;

    // System state bits
    JsonObject state = doc["system_state"].to<JsonObject>();
    state["burner_active"] = snapshot.burnerActive;
    state["heating_on"] = snapshot.heatingActive;
    state["water_on"] = snapshot.waterActive;
    state["state_bits"] = snapshot.systemStateBits;

    // Sensors
    if (snapshot.sensorsValid) {
        JsonObject sensors = doc["sensors"].to<JsonObject>();
        sensors["boiler_output"] = tempToFloat(snapshot.boilerTempOutput);
        sensors["boiler_return"] = tempToFloat(snapshot.boilerTempReturn);
        sensors["water_tank"] = tempToFloat(snapshot.waterHeaterTempTank);
        sensors["pressure"] = snapshot.systemPressure / 100.0f;
    } else {
        doc["sensors"] = "unavailable";
    }

    // Relays
    JsonObject relays = doc["relays"].to<JsonObject>();
    relays["desired"] = snapshot.relayDesiredState;
    relays["actual"] = snapshot.relayActualState;
    relays["mismatch_mask"] = snapshot.relayMismatchMask;

    char payload[768];
    size_t len = serializeJson(doc, payload, sizeof(payload));

    if (len < sizeof(payload)) {
        MQTTTask::publish(MQTT_ERROR_CONTEXT, payload, 0, true,
                         MQTTPriority::PRIORITY_CRITICAL);

        LOG_INFO(TAG, "Published error context: %s in %s (heap: %lu bytes)",
                 snapshot.component, snapshot.taskName, snapshot.freeHeap);
    } else {
        LOG_ERROR(TAG, "Error context too large: %zu bytes", len);
    }
}
