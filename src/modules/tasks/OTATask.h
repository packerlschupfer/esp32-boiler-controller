// OTATask.h
#pragma once

#include <Arduino.h>
#include "../../config/ProjectConfig.h"
#include <EthernetManager.h>
#include <OTAManager.h>

// #include "../tasks/SensorTask.h"
// #include "../utils/StatusLed.h"

/**
 * @brief Task for handling OTA (Over-The-Air) updates
 */
class OTATask {
   public:
    /**
     * @brief Initialize the OTA task
     *
     * @return true if initialization was successful
     */
    static bool init();

    /**
     * @brief Start the OTA task
     *
     * @return true if task started successfully
     */
    static bool start();

    /**
     * @brief FreeRTOS task function
     *
     * @param pvParameters Task parameters (not used)
     */
    static void taskFunction(void* pvParameters);

    // Task handle exposed for watchdog monitoring
    static TaskHandle_t taskHandle;
    static TaskHandle_t getTaskHandle() { return taskHandle; }

    static bool isRunning() { return taskHandle != nullptr; }
    
    // MQTT-enhanced OTA support (from OTATask_MQTT.cpp)
    static bool initWithMQTT();

   private:
    static bool otaUpdateInProgress;
    static SemaphoreHandle_t otaStatusMutex;

    // Network check callback for OTAManager
    static bool isNetworkConnected();

    // OTA update event callbacks
    static void onOTAStart();
    static void onOTAEnd();
    static void onOTAProgress(unsigned int progress, unsigned int total);
    static void onOTAError(ota_error_t error);
    
    // MQTT-enhanced OTA callbacks (from OTATask_MQTT.cpp)
    static void onOTAStartMQTT();
    static void onOTAEndMQTT();
    static void onOTAProgressMQTT(unsigned int progress, unsigned int total);
    static void onOTAErrorMQTT(ota_error_t error);

    static TimerHandle_t adaptiveTimer;
    static void adaptiveTimerCallback(TimerHandle_t xTimer);
    static bool performOptimizedScan(uint32_t scanDurationMs);
};