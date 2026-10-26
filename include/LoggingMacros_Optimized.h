// LoggingMacros_Optimized.h
#pragma once

#include <esp_log.h>

// When using custom logger, we can disable ESP-IDF's logging backend to save memory
#ifdef LOG_NO_CUSTOM_LOGGER
    // Use ESP-IDF logging directly when custom logger is disabled
    #define LOG_ERROR(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
    #define LOG_WARN(tag, fmt, ...)  ESP_LOGW(tag, fmt, ##__VA_ARGS__)
    #define LOG_INFO(tag, fmt, ...)  ESP_LOGI(tag, fmt, ##__VA_ARGS__)
    #define LOG_DEBUG(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)
    #define LOG_VERBOSE(tag, fmt, ...) ESP_LOGV(tag, fmt, ##__VA_ARGS__)
#else
    // Using custom logger - include it
    #include "Logger.h"
    
    // Disable ESP-IDF's internal logging to save memory
    #ifdef CONFIG_LOG_DEFAULT_LEVEL
        #undef CONFIG_LOG_DEFAULT_LEVEL
        #define CONFIG_LOG_DEFAULT_LEVEL 0  // Disable ESP-IDF logging
    #endif
    
    // Get logger instance efficiently
    inline Logger& getLoggerInstance() {
        return Logger::getInstance();
    }
    
    // In release mode, remove debug and verbose logs at compile time
    #ifdef LOG_MODE_RELEASE
        #define LOG_ERROR(tag, fmt, ...) getLoggerInstance().log(ESP_LOG_ERROR, tag, fmt, ##__VA_ARGS__)
        #define LOG_WARN(tag, fmt, ...)  getLoggerInstance().log(ESP_LOG_WARN, tag, fmt, ##__VA_ARGS__)
        #define LOG_INFO(tag, fmt, ...)  getLoggerInstance().log(ESP_LOG_INFO, tag, fmt, ##__VA_ARGS__)
        #define LOG_DEBUG(tag, fmt, ...) ((void)0)  // Compiled out in release
        #define LOG_VERBOSE(tag, fmt, ...) ((void)0) // Compiled out in release
    #else
        #define LOG_ERROR(tag, fmt, ...) getLoggerInstance().log(ESP_LOG_ERROR, tag, fmt, ##__VA_ARGS__)
        #define LOG_WARN(tag, fmt, ...)  getLoggerInstance().log(ESP_LOG_WARN, tag, fmt, ##__VA_ARGS__)
        #define LOG_INFO(tag, fmt, ...)  getLoggerInstance().log(ESP_LOG_INFO, tag, fmt, ##__VA_ARGS__)
        #define LOG_DEBUG(tag, fmt, ...) getLoggerInstance().log(ESP_LOG_DEBUG, tag, fmt, ##__VA_ARGS__)
        #define LOG_VERBOSE(tag, fmt, ...) getLoggerInstance().log(ESP_LOG_VERBOSE, tag, fmt, ##__VA_ARGS__)
    #endif
#endif

// Memory-efficient logging helpers
#ifdef LOG_MODE_RELEASE
    // In release mode, use simplified messages to save flash memory
    #define LOG_FUNC_ENTRY() ((void)0)
    #define LOG_FUNC_EXIT() ((void)0)
    #define LOG_HEAP_INFO() ((void)0)
#else
    #define LOG_FUNC_ENTRY() LOG_DEBUG(__func__, "Entry")
    #define LOG_FUNC_EXIT() LOG_DEBUG(__func__, "Exit")
    #define LOG_HEAP_INFO() LOG_DEBUG(__func__, "Heap: %d", ESP.getFreeHeap())
#endif

// Conditional format string storage
#ifdef LOG_MODE_RELEASE
    // Store format strings in flash to save RAM
    #define LOG_FMT(fmt) PSTR(fmt)
#else
    #define LOG_FMT(fmt) fmt
#endif