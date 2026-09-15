// src/utils/ErrorContextCapture.cpp
#include "ErrorContextSnapshot.h"
#include "LoggingMacros.h"
#include "modules/tasks/MQTTTask.h"
#include "MQTTTopics.h"
#include "core/SystemResourceProvider.h"
#include "events/SystemEventsGenerated.h"  // C5: SystemState bit constants
#include "shared/SharedSensorReadings.h"
#include "shared/RelayState.h"
#include "utils/ErrorContextFormat.h"
#include <Arduino.h>
#include <atomic>
#include <cstring>
#include <esp_system.h>
#include <esp_heap_caps.h>

static const char* TAG = "ErrorContext";

namespace {
    // Single snapshot slot: EMPTY -> WRITING (erroring task) -> READY -> PUBLISHING (MQTT task) -> EMPTY
    enum SlotState : uint8_t { SLOT_EMPTY, SLOT_WRITING, SLOT_READY, SLOT_PUBLISHING };

    ErrorContextSnapshot s_snapshot = {};
    std::atomic<uint8_t> s_slotState{SLOT_EMPTY};
    std::atomic<uint32_t> s_lastRecordMs{0};
    std::atomic<bool> s_hasRecorded{false};
    char s_payload[sizeof(MQTTPublishRequest::payload)];
}

bool ErrorContextCapture::isCriticalError(SystemError error) {
    return (error == SystemError::SYSTEM_FAILSAFE_TRIGGERED ||
            error == SystemError::IGNITION_FAILURE ||
            error == SystemError::RELAY_SAFETY_INTERLOCK ||
            error == SystemError::TEMPERATURE_CRITICAL ||
            error == SystemError::SYSTEM_OVERHEATED ||
            error == SystemError::EMERGENCY_STOP);
}

void ErrorContextCapture::recordCriticalError(SystemError errorCode, const char* component,
                                              const char* description) {
    if (!isCriticalError(errorCode)) {
        return;
    }

    const uint32_t now = millis();
    if (s_hasRecorded.load() && (now - s_lastRecordMs.load()) < MIN_INTERVAL_MS) {
        return;
    }

    // Claim the slot; if a snapshot is still being written or not yet published, keep it
    uint8_t expected = SLOT_EMPTY;
    if (!s_slotState.compare_exchange_strong(expected, SLOT_WRITING)) {
        return;
    }

    captureInto(s_snapshot, errorCode, component, description);
    s_lastRecordMs.store(now);
    s_hasRecorded.store(true);
    s_slotState.store(SLOT_READY);
}

void ErrorContextCapture::captureInto(ErrorContextSnapshot& snapshot, SystemError errorCode,
                                      const char* component, const char* description) {
    memset(&snapshot, 0, sizeof(snapshot));

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

    // Sensor snapshot (short mutex wait; a caller already holding the mutex gets sensorsValid=false)
    if (SRP::getSensorReadingsMutex() != nullptr && SRP::takeSensorReadingsMutex(pdMS_TO_TICKS(10))) {
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
}

void ErrorContextCapture::publishPending() {
    uint8_t expected = SLOT_READY;
    if (!s_slotState.compare_exchange_strong(expected, SLOT_PUBLISHING)) {
        return;
    }

    const ErrorContextSnapshot& snapshot = s_snapshot;
    ErrorContextFormat::Fields fields = {};
    fields.errorCode = static_cast<int32_t>(snapshot.errorCode);
    fields.component = snapshot.component;
    fields.description = snapshot.description;
    fields.timestampMs = snapshot.timestamp;
    fields.taskName = snapshot.taskName;
    fields.taskPriority = snapshot.taskPriority;
    fields.freeHeap = snapshot.freeHeap;
    fields.minFreeHeap = snapshot.minFreeHeap;
    fields.largestFreeBlock = snapshot.largestFreeBlock;
    fields.systemStateBits = snapshot.systemStateBits;
    fields.burnerRequestBits = snapshot.burnerRequestBits;
    fields.sensorsValid = snapshot.sensorsValid;
    fields.boilerOutput = snapshot.boilerTempOutput;
    fields.boilerReturn = snapshot.boilerTempReturn;
    fields.waterTank = snapshot.waterHeaterTempTank;
    fields.pressure = snapshot.systemPressure;
    fields.relayDesired = snapshot.relayDesiredState;
    fields.relayActual = snapshot.relayActualState;

    if (ErrorContextFormat::format(s_payload, sizeof(s_payload), fields) > 0) {
        MQTTTask::publish(MQTT_ERROR_CONTEXT, s_payload, 0, true, MQTTPriority::PRIORITY_CRITICAL);
        LOG_INFO(TAG, "Published error context: %s in %s (heap: %lu bytes)",
                 snapshot.component, snapshot.taskName, static_cast<unsigned long>(snapshot.freeHeap));
    } else {
        LOG_ERROR(TAG, "Error context does not fit %u bytes", static_cast<unsigned>(sizeof(s_payload)));
    }

    s_slotState.store(SLOT_EMPTY);
}
