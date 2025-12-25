// DebugMacros.h
#pragma once

// Use conditional logger inclusion
#ifndef LOG_NO_CUSTOM_LOGGER
    #include <Logger.h>
#endif
#include <esp_log.h>
#include "core/SystemResourceProvider.h"

// Optimize log buffer size based on build mode
#ifndef CONFIG_LOG_BUFFER_SIZE
    #if defined(LOG_MODE_DEBUG_FULL)
        #define CONFIG_LOG_BUFFER_SIZE 512  // Full logging needs larger buffer
    #elif defined(LOG_MODE_DEBUG_SELECTIVE)
        #define CONFIG_LOG_BUFFER_SIZE 384  // Moderate buffer for selective logging
    #else  // LOG_MODE_RELEASE
        #define CONFIG_LOG_BUFFER_SIZE 256  // Reasonable buffer for release
    #endif
#endif

// ==========================
// Ethernet Manager Macros
// ==========================

// DEBUG
#ifdef ETH_DEBUG
    #ifndef LOG_NO_CUSTOM_LOGGER
        #define DEBUG_PRINT_ETH(...)      SRP::getLogger().logNnL(ESP_LOG_DEBUG, "ETH", __VA_ARGS__)
        #define DEBUG_PRINTLN_ETH(...)    SRP::getLogger().log(ESP_LOG_DEBUG, "ETH", __VA_ARGS__)
        #define DEBUG_PRINTINL_ETH(...)   SRP::getLogger().logInL(__VA_ARGS__)
    #else
        #define DEBUG_PRINT_ETH(...)      ((void)0)  // No direct equivalent for no-newline in ESP-IDF
        #define DEBUG_PRINTLN_ETH(...)    ESP_LOGD("ETH", __VA_ARGS__)
        #define DEBUG_PRINTINL_ETH(...)   ((void)0)  // No direct equivalent for inline in ESP-IDF
    #endif
#else
    #define DEBUG_PRINT_ETH(...)
    #define DEBUG_PRINTLN_ETH(...)
    #define DEBUG_PRINTINL_ETH(...)
#endif

// INFO
#ifndef LOG_NO_CUSTOM_LOGGER
    #define INFO_PRINT_ETH(...)           SRP::getLogger().logNnL(ESP_LOG_INFO, "ETH", __VA_ARGS__)
    #define INFO_PRINTLN_ETH(...)         SRP::getLogger().log(ESP_LOG_INFO, "ETH", __VA_ARGS__)
    #define INFO_PRINTINL_ETH(...)        SRP::getLogger().logInL(__VA_ARGS__)
#else
    #define INFO_PRINT_ETH(...)           ((void)0)
    #define INFO_PRINTLN_ETH(...)         ESP_LOGI("ETH", __VA_ARGS__)
    #define INFO_PRINTINL_ETH(...)        ((void)0)
#endif

// WARN
#ifndef LOG_NO_CUSTOM_LOGGER
    #define WARN_PRINT_ETH(...)           SRP::getLogger().logNnL(ESP_LOG_WARN, "ETH", __VA_ARGS__)
    #define WARN_PRINTLN_ETH(...)         SRP::getLogger().log(ESP_LOG_WARN, "ETH", __VA_ARGS__)
    #define WARN_PRINTINL_ETH(...)        SRP::getLogger().logInL(__VA_ARGS__)
#else
    #define WARN_PRINT_ETH(...)           ((void)0)
    #define WARN_PRINTLN_ETH(...)         ESP_LOGW("ETH", __VA_ARGS__)
    #define WARN_PRINTINL_ETH(...)        ((void)0)
#endif

// ERROR
#ifndef LOG_NO_CUSTOM_LOGGER
    #define ERROR_PRINT_ETH(...)          SRP::getLogger().logNnL(ESP_LOG_ERROR, "ETH", __VA_ARGS__)
    #define ERROR_PRINTLN_ETH(...)        SRP::getLogger().log(ESP_LOG_ERROR, "ETH", __VA_ARGS__)
    #define ERROR_PRINTINL_ETH(...)       SRP::getLogger().logInL(__VA_ARGS__)
#else
    #define ERROR_PRINT_ETH(...)          ((void)0)
    #define ERROR_PRINTLN_ETH(...)        ESP_LOGE("ETH", __VA_ARGS__)
    #define ERROR_PRINTINL_ETH(...)       ((void)0)
#endif

// ==========================
// OTA Manager Macros
// ==========================

// DEBUG
#ifdef OTA_DEBUG
    #ifndef LOG_NO_CUSTOM_LOGGER
        #define DEBUG_PRINT_OTA(...)      SRP::getLogger().logNnL(ESP_LOG_DEBUG, "OTA", __VA_ARGS__)
        #define DEBUG_PRINTLN_OTA(...)    SRP::getLogger().log(ESP_LOG_DEBUG, "OTA", __VA_ARGS__)
        #define DEBUG_PRINTINL_OTA(...)   SRP::getLogger().logInL(__VA_ARGS__)
    #else
        #define DEBUG_PRINT_OTA(...)      ((void)0)
        #define DEBUG_PRINTLN_OTA(...)    ESP_LOGD("OTA", __VA_ARGS__)
        #define DEBUG_PRINTINL_OTA(...)   ((void)0)
    #endif
#else
    #define DEBUG_PRINT_OTA(...)
    #define DEBUG_PRINTLN_OTA(...)
    #define DEBUG_PRINTINL_OTA(...)
#endif

// INFO
#ifndef LOG_NO_CUSTOM_LOGGER
    #define INFO_PRINT_OTA(...)           SRP::getLogger().logNnL(ESP_LOG_INFO, "OTA", __VA_ARGS__)
    #define INFO_PRINTLN_OTA(...)         SRP::getLogger().log(ESP_LOG_INFO, "OTA", __VA_ARGS__)
    #define INFO_PRINTINL_OTA(...)        SRP::getLogger().logInL(__VA_ARGS__)
#else
    #define INFO_PRINT_OTA(...)           ((void)0)
    #define INFO_PRINTLN_OTA(...)         ESP_LOGI("OTA", __VA_ARGS__)
    #define INFO_PRINTINL_OTA(...)        ((void)0)
#endif

// WARN
#ifndef LOG_NO_CUSTOM_LOGGER
    #define WARN_PRINT_OTA(...)           SRP::getLogger().logNnL(ESP_LOG_WARN, "OTA", __VA_ARGS__)
    #define WARN_PRINTLN_OTA(...)         SRP::getLogger().log(ESP_LOG_WARN, "OTA", __VA_ARGS__)
    #define WARN_PRINTINL_OTA(...)        SRP::getLogger().logInL(__VA_ARGS__)
#else
    #define WARN_PRINT_OTA(...)           ((void)0)
    #define WARN_PRINTLN_OTA(...)         ESP_LOGW("OTA", __VA_ARGS__)
    #define WARN_PRINTINL_OTA(...)        ((void)0)
#endif

// ERROR
#ifndef LOG_NO_CUSTOM_LOGGER
    #define ERROR_PRINT_OTA(...)          SRP::getLogger().logNnL(ESP_LOG_ERROR, "OTA", __VA_ARGS__)
    #define ERROR_PRINTLN_OTA(...)        SRP::getLogger().log(ESP_LOG_ERROR, "OTA", __VA_ARGS__)
    #define ERROR_PRINTINL_OTA(...)       SRP::getLogger().logInL(__VA_ARGS__)
#else
    #define ERROR_PRINT_OTA(...)          ((void)0)
    #define ERROR_PRINTLN_OTA(...)        ESP_LOGE("OTA", __VA_ARGS__)
    #define ERROR_PRINTINL_OTA(...)       ((void)0)
#endif

// ==========================
// Main Module Macros
// ==========================

// DEBUG
#ifdef MAIN_DEBUG
    #ifndef LOG_NO_CUSTOM_LOGGER
        #define DEBUG_PRINT_MAIN(...)     SRP::getLogger().logNnL(ESP_LOG_DEBUG, "Main", __VA_ARGS__)
        #define DEBUG_PRINTLN_MAIN(...)   SRP::getLogger().log(ESP_LOG_DEBUG, "Main", __VA_ARGS__)
        #define DEBUG_PRINTINL_MAIN(...)  SRP::getLogger().logInL(__VA_ARGS__)
    #else
        #define DEBUG_PRINT_MAIN(...)     ((void)0)
        #define DEBUG_PRINTLN_MAIN(...)   ESP_LOGD("Main", __VA_ARGS__)
        #define DEBUG_PRINTINL_MAIN(...)  ((void)0)
    #endif
#else
    #define DEBUG_PRINT_MAIN(...)
    #define DEBUG_PRINTLN_MAIN(...)
    #define DEBUG_PRINTINL_MAIN(...)
#endif

// INFO
#ifndef LOG_NO_CUSTOM_LOGGER
    #define INFO_PRINT_MAIN(...)          SRP::getLogger().logNnL(ESP_LOG_INFO, "Main", __VA_ARGS__)
    #define INFO_PRINTLN_MAIN(...)        SRP::getLogger().log(ESP_LOG_INFO, "Main", __VA_ARGS__)
    #define INFO_PRINTINL_MAIN(...)       SRP::getLogger().logInL(__VA_ARGS__)
#else
    #define INFO_PRINT_MAIN(...)          ((void)0)
    #define INFO_PRINTLN_MAIN(...)        ESP_LOGI("Main", __VA_ARGS__)
    #define INFO_PRINTINL_MAIN(...)       ((void)0)
#endif

// WARN
#ifndef LOG_NO_CUSTOM_LOGGER
    #define WARN_PRINT_MAIN(...)          SRP::getLogger().logNnL(ESP_LOG_WARN, "Main", __VA_ARGS__)
    #define WARN_PRINTLN_MAIN(...)        SRP::getLogger().log(ESP_LOG_WARN, "Main", __VA_ARGS__)
    #define WARN_PRINTINL_MAIN(...)       SRP::getLogger().logInL(__VA_ARGS__)
#else
    #define WARN_PRINT_MAIN(...)          ((void)0)
    #define WARN_PRINTLN_MAIN(...)        ESP_LOGW("Main", __VA_ARGS__)
    #define WARN_PRINTINL_MAIN(...)       ((void)0)
#endif

// ERROR
#ifndef LOG_NO_CUSTOM_LOGGER
    #define ERROR_PRINT_MAIN(...)         SRP::getLogger().logNnL(ESP_LOG_ERROR, "Main", __VA_ARGS__)
    #define ERROR_PRINTLN_MAIN(...)       SRP::getLogger().log(ESP_LOG_ERROR, "Main", __VA_ARGS__)
    #define ERROR_PRINTINL_MAIN(...)      SRP::getLogger().logInL(__VA_ARGS__)
#else
    #define ERROR_PRINT_MAIN(...)         ((void)0)
    #define ERROR_PRINTLN_MAIN(...)       ESP_LOGE("Main", __VA_ARGS__)
    #define ERROR_PRINTINL_MAIN(...)      ((void)0)
#endif
