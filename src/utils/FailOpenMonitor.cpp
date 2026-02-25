// src/utils/FailOpenMonitor.cpp
#include "FailOpenMonitor.h"
#include "LoggingMacros.h"
#include "modules/tasks/MQTTTask.h"
#include "MQTTTopics.h"
#include <ArduinoJson.h>

static const char* TAG = "FailOpenMonitor";

// Initialize static members
FailOpenMonitor::FailOpenStats FailOpenMonitor::stats[4];
const char* FailOpenMonitor::checkTypeNames[4] = {
    "water_flow",
    "pressure",
    "hardware_interlocks",
    "flame"
};

void FailOpenMonitor::initialize() {
    for (int i = 0; i < 4; i++) {
        stats[i].consecutiveFailOpens = 0;
        stats[i].totalFailOpens = 0;
        stats[i].firstFailOpenTime = 0;
        stats[i].mqttAlertSent = false;
    }
    LOG_INFO(TAG, "Fail-open monitor initialized");
}

FailOpenMonitor::FailOpenStats& FailOpenMonitor::getStats(CheckType type) {
    return stats[static_cast<int>(type)];
}

const char* FailOpenMonitor::checkTypeToString(CheckType type) {
    return checkTypeNames[static_cast<int>(type)];
}

void FailOpenMonitor::recordFailOpen(CheckType type, const char* reason) {
    FailOpenStats& s = getStats(type);

    s.consecutiveFailOpens++;
    s.totalFailOpens++;

    // Start timing on first fail-open
    if (s.firstFailOpenTime == 0) {
        s.firstFailOpenTime = millis();
    }

    // MQTT alert after threshold
    if (s.consecutiveFailOpens == CONSECUTIVE_ALERT_THRESHOLD && !s.mqttAlertSent) {
        JsonDocument doc;
        doc["check"] = checkTypeToString(type);
        doc["reason"] = reason;
        doc["consecutive_count"] = s.consecutiveFailOpens.load();

        // Calculate duration in hours
        uint32_t durationMs = millis() - s.firstFailOpenTime;
        doc["duration_hours"] = durationMs / 3600000.0f;

        char payload[192];
        serializeJson(doc, payload, sizeof(payload));

        MQTTTask::publish(MQTT_ALERT_DEGRADED_OPERATION, payload, 0, true,
                         MQTTPriority::PRIORITY_HIGH);

        s.mqttAlertSent = true;
        LOG_WARN(TAG, "DEGRADED: %s failed open for %lu consecutive checks - %s",
                checkTypeToString(type), s.consecutiveFailOpens.load(), reason);
    }
}

void FailOpenMonitor::recordNormalOperation(CheckType type) {
    FailOpenStats& s = getStats(type);

    // Only log if recovering from fail-open state
    if (s.consecutiveFailOpens > 0) {
        LOG_INFO(TAG, "Recovered: %s now operating normally (was %lu consecutive fail-opens)",
                checkTypeToString(type), s.consecutiveFailOpens.load());
    }

    // Reset counters
    s.consecutiveFailOpens = 0;
    s.firstFailOpenTime = 0;
    s.mqttAlertSent = false;
}

void FailOpenMonitor::publishDegradedStatus() {
    JsonDocument doc;
    bool hasDegradedChecks = false;

    for (int i = 0; i < 4; i++) {
        FailOpenStats& s = stats[i];
        if (s.consecutiveFailOpens > 0) {
            JsonObject check = doc[checkTypeNames[i]].to<JsonObject>();
            check["consecutive"] = s.consecutiveFailOpens.load();
            check["total"] = s.totalFailOpens.load();

            // Calculate duration in hours
            uint32_t durationMs = millis() - s.firstFailOpenTime;
            check["duration_hours"] = durationMs / 3600000.0f;

            hasDegradedChecks = true;
        }
    }

    if (hasDegradedChecks) {
        char payload[256];
        serializeJson(doc, payload, sizeof(payload));
        MQTTTask::publish(MQTT_STATUS_DEGRADED_CHECKS, payload, 0, true,
                         MQTTPriority::PRIORITY_LOW);
    }
}

uint32_t FailOpenMonitor::getConsecutiveCount(CheckType type) {
    return getStats(type).consecutiveFailOpens.load();
}
