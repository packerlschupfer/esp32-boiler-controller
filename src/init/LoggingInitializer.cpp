// src/init/LoggingInitializer.cpp
#include "LoggingInitializer.h"

#include <Arduino.h>
#include <esp_log.h>
#include "LoggingMacros.h"
#include "config/ProjectConfig.h"

#ifndef LOG_NO_CUSTOM_LOGGER
#include <Logger.h>
#endif

static const char* TAG = "LoggingInitializer";

Result<void> LoggingInitializer::initialize() {
    LOG_DEBUG(TAG, "LoggingInitializer::initialize called at %lu ms", millis());
    LOG_INFO(TAG, "Initializing logging system...");

#if defined(LOG_MODE_DEBUG_FULL) && !defined(LOG_MODE_DEBUG_SELECTIVE)
    esp_log_level_set("*", ESP_LOG_DEBUG);
    LOG_INFO(TAG, "Log mode: DEBUG FULL");
#elif defined(LOG_MODE_DEBUG_SELECTIVE)
    esp_log_level_set("*", ESP_LOG_INFO);
    LOG_INFO(TAG, "Log mode: DEBUG SELECTIVE");

    suppressVerboseLogs();
    configureSelectiveDebug();

    // Ensure control task logs are visible
    esp_log_level_set("HeatingControlTask", ESP_LOG_INFO);
    esp_log_level_set("WheaterControlTask", ESP_LOG_INFO);
    esp_log_level_set("BurnerControlTask", ESP_LOG_INFO);
    esp_log_level_set("HeatingControl", ESP_LOG_INFO);
    esp_log_level_set("WheaterControl", ESP_LOG_INFO);
    esp_log_level_set("BurnerControl", ESP_LOG_INFO);

#ifndef LOG_NO_CUSTOM_LOGGER
    Logger::getInstance().setTagLevel("HeatingControlTask", ESP_LOG_INFO);
    Logger::getInstance().setTagLevel("WheaterControlTask", ESP_LOG_INFO);
    Logger::getInstance().setTagLevel("BurnerControlTask", ESP_LOG_INFO);
    Logger::getInstance().setTagLevel("HeatingControl", ESP_LOG_INFO);
    Logger::getInstance().setTagLevel("WheaterControl", ESP_LOG_INFO);
    Logger::getInstance().setTagLevel("BurnerControl", ESP_LOG_INFO);
#endif

#else
    // Release mode - minimal logging
    esp_log_level_set("*", ESP_LOG_INFO);
    LOG_INFO(TAG, "Log mode: RELEASE");

    // Suppress verbose ESP32 HAL logs in release mode
    esp_log_level_set("esp32-hal-uart", ESP_LOG_ERROR);
    esp_log_level_set("esp32-hal-periman", ESP_LOG_ERROR);
    esp_log_level_set("esp32-hal-gpio", ESP_LOG_ERROR);
    esp_log_level_set("esp.emac", ESP_LOG_ERROR);
    esp_log_level_set("esp-tls", ESP_LOG_ERROR);
    // Suppress ESP-IDF MQTT/transport internal logs (we have our own MQTT logging)
    esp_log_level_set("transport_base", ESP_LOG_NONE);
    esp_log_level_set("transport", ESP_LOG_NONE);
    esp_log_level_set("TRANS_TCP", ESP_LOG_NONE);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_NONE);
    esp_log_level_set("mqtt_client", ESP_LOG_NONE);
#endif

    // Suppress task_wdt "already subscribed" errors from libraries
    esp_log_level_set("task_wdt", ESP_LOG_NONE);

#ifndef LOG_NO_CUSTOM_LOGGER
    Logger::getInstance().setTagLevel(TAG, ESP_LOG_INFO);
#else
    esp_log_level_set(TAG, ESP_LOG_INFO);
#endif

    return Result<void>();
}

void LoggingInitializer::suppressVerboseLogs() {
    // Suppress verbose HAL and system logs
    esp_log_level_set("efuse", ESP_LOG_NONE);
    esp_log_level_set("cpu_start", ESP_LOG_NONE);
    esp_log_level_set("heap_init", ESP_LOG_NONE);
    esp_log_level_set("intr_alloc", ESP_LOG_NONE);
    esp_log_level_set("spi_flash", ESP_LOG_WARN);
    esp_log_level_set("system_api", ESP_LOG_WARN);
    esp_log_level_set("nvs", ESP_LOG_WARN);
    esp_log_level_set("phy", ESP_LOG_WARN);
    esp_log_level_set("wifi", ESP_LOG_NONE);
    esp_log_level_set("wifi_init", ESP_LOG_NONE);
    esp_log_level_set("phy_init", ESP_LOG_NONE);

    // Suppress verbose ESP32 HAL logs
    esp_log_level_set("esp32-hal-uart", ESP_LOG_WARN);
    esp_log_level_set("esp32-hal-periman", ESP_LOG_WARN);
    esp_log_level_set("esp32-hal-gpio", ESP_LOG_WARN);
    esp_log_level_set("esp32-hal-cpu", ESP_LOG_WARN);
    esp_log_level_set("esp32-hal-i2c", ESP_LOG_WARN);
    esp_log_level_set("esp32-hal-ledc", ESP_LOG_WARN);
    esp_log_level_set("esp32-hal-matrix", ESP_LOG_WARN);
    esp_log_level_set("esp32-hal-misc", ESP_LOG_WARN);
    esp_log_level_set("esp32-hal-psram", ESP_LOG_WARN);
    esp_log_level_set("esp32-hal-spi", ESP_LOG_WARN);
    esp_log_level_set("esp32-hal-timer", ESP_LOG_WARN);

    // Suppress ESP-IDF MQTT/transport internal logs (we have our own MQTT logging)
    esp_log_level_set("transport_base", ESP_LOG_NONE);
    esp_log_level_set("transport", ESP_LOG_NONE);
    esp_log_level_set("TRANS_TCP", ESP_LOG_NONE);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_NONE);
    esp_log_level_set("mqtt_client", ESP_LOG_NONE);
    esp_log_level_set("esp-tls", ESP_LOG_NONE);
}

void LoggingInitializer::configureSelectiveDebug() {
#ifdef MAIN_DEBUG
#ifndef LOG_NO_CUSTOM_LOGGER
    Logger::getInstance().setTagLevel(TAG, ESP_LOG_DEBUG);
#else
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
#endif
#endif

#ifdef ETH_DEBUG
#ifndef LOG_NO_CUSTOM_LOGGER
    Logger::getInstance().setTagLevel("EthernetManager", ESP_LOG_DEBUG);
#else
    esp_log_level_set("EthernetManager", ESP_LOG_DEBUG);
#endif
#endif

#ifdef MB8ART_DEBUG
#ifndef LOG_NO_CUSTOM_LOGGER
    Logger::getInstance().setTagLevel("MB8ART", ESP_LOG_DEBUG);
#else
    esp_log_level_set("MB8ART", ESP_LOG_DEBUG);
#endif
#endif

#ifdef RYN4_DEBUG
#ifndef LOG_NO_CUSTOM_LOGGER
    Logger::getInstance().setTagLevel("RYN4", ESP_LOG_DEBUG);
#else
    esp_log_level_set("RYN4", ESP_LOG_DEBUG);
#endif
#endif

#ifdef MODBUSDEVICE_DEBUG
#ifndef LOG_NO_CUSTOM_LOGGER
    Logger::getInstance().setTagLevel("ModbusD", ESP_LOG_DEBUG);
    Logger::getInstance().setTagLevel("ModbusDevice", ESP_LOG_DEBUG);
#else
    esp_log_level_set("ModbusD", ESP_LOG_DEBUG);
    esp_log_level_set("ModbusDevice", ESP_LOG_DEBUG);
#endif
#endif

#ifdef OTA_DEBUG
#ifndef LOG_NO_CUSTOM_LOGGER
    Logger::getInstance().setTagLevel("OTAMgr", ESP_LOG_DEBUG);
    Logger::getInstance().setTagLevel("OTAManager", ESP_LOG_DEBUG);
#else
    esp_log_level_set("OTAMgr", ESP_LOG_DEBUG);
    esp_log_level_set("OTAManager", ESP_LOG_DEBUG);
#endif
#endif

#ifdef MODBUS_RTU_DEBUG
#ifndef LOG_NO_CUSTOM_LOGGER
    Logger::getInstance().setTagLevel("ModbusRTU", ESP_LOG_DEBUG);
#else
    esp_log_level_set("ModbusRTU", ESP_LOG_DEBUG);
#endif
#endif
}
