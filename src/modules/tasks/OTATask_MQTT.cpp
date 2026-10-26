// OTATask_MQTT.cpp - Extension for MQTT status reporting
#include "OTATask.h"
#include "MQTTTask.h"
#include "core/SystemResourceProvider.h"
#include <ArduinoJson.h>

static const char* TAG = "OTA";

// Static storage for progress tracking
static unsigned int lastProgress = 0;
static unsigned int lastTotal = 0;
static TickType_t lastProgressTime = 0;

/**
 * @brief Publish OTA status via MQTT
 */
void publishOTAStatus(const char* status, unsigned int progress = 0, unsigned int total = 0) {
    // Create JSON document
    JsonDocument doc;  // ArduinoJson v7
    doc["status"] = status;
    doc["timestamp"] = millis();
    
    if (total > 0) {
        doc["progress"] = progress;
        doc["total"] = total;
        doc["percent"] = (progress * 100) / total;
        
        // Calculate speed if we have previous data
        if (lastProgress > 0 && lastProgressTime > 0) {
            TickType_t currentTime = xTaskGetTickCount();
            TickType_t timeDiff = currentTime - lastProgressTime;
            
            if (timeDiff > 0) {
                unsigned int bytesDiff = progress - lastProgress;
                float secondsDiff = (float)timeDiff / configTICK_RATE_HZ;
                unsigned int bytesPerSecond = (unsigned int)(bytesDiff / secondsDiff);
                doc["speed"] = bytesPerSecond;
            }
        }
        
        lastProgress = progress;
        lastProgressTime = xTaskGetTickCount();
    }
    
    // Serialize to string
    char buffer[192];  // Optimized: sufficient for OTA status JSON
    serializeJson(doc, buffer);
    
    // Publish via MQTT if available
    if (MQTTTask::isConnected()) {
        MQTTTask::publish("state/ota", buffer, 0, false);
    }
}

/**
 * @brief Enhanced OTA start callback with MQTT reporting
 */
void OTATask::onOTAStartMQTT() {
    // Call original handler
    onOTAStart();
    
    // Reset progress tracking
    lastProgress = 0;
    lastTotal = 0;
    lastProgressTime = xTaskGetTickCount();
    
    // Publish start status
    publishOTAStatus("starting");
    
    // Request memory diagnostic before update
    if (MQTTTask::isConnected()) {
        MQTTTask::publish("diagnostics/memory", "", 0, false);
    }
}

/**
 * @brief Enhanced OTA end callback with MQTT reporting
 */
void OTATask::onOTAEndMQTT() {
    // Publish completion status
    publishOTAStatus("completed", lastTotal, lastTotal);
    
    // Give time for MQTT message to send
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Call original handler (will reboot)
    onOTAEnd();
}

/**
 * @brief Enhanced OTA progress callback with MQTT reporting
 */
void OTATask::onOTAProgressMQTT(unsigned int progress, unsigned int total) {
    // Call original handler
    onOTAProgress(progress, total);
    
    // Update total if changed
    if (total != lastTotal) {
        lastTotal = total;
    }
    
    // Publish progress every 5% or at least every 5 seconds
    static int lastPercent = 0;
    int percent = (progress * 100) / total;
    
    TickType_t currentTime = xTaskGetTickCount();
    bool timeElapsed = (currentTime - lastProgressTime) > pdMS_TO_TICKS(5000);
    bool percentChanged = (percent >= lastPercent + 5);
    
    if (percentChanged || timeElapsed || percent == 100) {
        publishOTAStatus("updating", progress, total);
        lastPercent = percent;
    }
}

/**
 * @brief Enhanced OTA error callback with MQTT reporting
 */
void OTATask::onOTAErrorMQTT(ota_error_t error) {
    // Create detailed error status
    JsonDocument doc;  // ArduinoJson v7
    doc["status"] = "error";
    doc["error_code"] = error;
    
    switch (error) {
        case OTA_AUTH_ERROR:
            doc["error_message"] = "Authentication Failed";
            break;
        case OTA_BEGIN_ERROR:
            doc["error_message"] = "Begin Failed";
            break;
        case OTA_CONNECT_ERROR:
            doc["error_message"] = "Connection Failed";
            break;
        case OTA_RECEIVE_ERROR:
            doc["error_message"] = "Receive Failed";
            break;
        case OTA_END_ERROR:
            doc["error_message"] = "End Failed";
            break;
        default:
            doc["error_message"] = "Unknown Error";
    }
    
    char buffer[192];  // Optimized: sufficient for error message JSON
    serializeJson(doc, buffer);
    
    // Publish error status
    if (MQTTTask::isConnected()) {
        MQTTTask::publish("state/ota", buffer, 0, true);  // Retain error
    }
    
    // Call original handler
    onOTAError(error);
}

/**
 * @brief Initialize OTA with MQTT status reporting
 * 
 * Call this instead of regular init() to enable MQTT status
 */
bool OTATask::initWithMQTT() {
    // First do regular initialization
    if (!init()) {
        return false;
    }
    
    // Set enhanced callbacks
    OTAManager::setStartCallback(onOTAStartMQTT);
    OTAManager::setEndCallback(onOTAEndMQTT);
    OTAManager::setProgressCallback(onOTAProgressMQTT);
    OTAManager::setErrorCallback(onOTAErrorMQTT);
    
    // Publish initial status
    publishOTAStatus("ready");
    
    LOG_INFO(TAG, "OTA initialized with MQTT status reporting");
    return true;
}