// include/utils/SafeFormatter.h
#ifndef SAFE_FORMATTER_H
#define SAFE_FORMATTER_H

#include <cstdio>
#include <cstdarg>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config/SystemConstants.h"

/**
 * @brief Thread-safe string formatter with reduced memory usage
 * 
 * Simple implementation using a shared buffer with mutex protection.
 * Reduces stack usage in logging operations.
 */
class SafeFormatter {
private:
    // Shared buffer with mutex protection
    static char buffer[256];
    static char smallBuffer[64];
    static SemaphoreHandle_t bufferMutex;
    static bool initialized;
    
    static void ensureInit() {
        if (!initialized) {
            bufferMutex = xSemaphoreCreateMutex();
            initialized = true;
        }
    }
    
public:
    /**
     * @brief Format a string with printf-style arguments
     * @param fmt Format string
     * @param args Variable arguments
     * @return Pointer to formatted string (valid until next call)
     */
    template<typename... Args>
    static const char* format(const char* fmt, Args... args) {
        ensureInit();
        
        if (bufferMutex && xSemaphoreTake(bufferMutex, pdMS_TO_TICKS(SystemConstants::Timing::MUTEX_SAFELOG_TIMEOUT_MS)) == pdTRUE) {
            vsnprintf(buffer, sizeof(buffer), fmt, args...);
            xSemaphoreGive(bufferMutex);
        }
        return buffer;
    }
    
    /**
     * @brief Format a small string (numbers, short messages)
     * @param fmt Format string
     * @param args Variable arguments
     * @return Pointer to formatted string (valid until next call)
     */
    template<typename... Args>
    static const char* formatSmall(const char* fmt, Args... args) {
        ensureInit();
        
        if (bufferMutex && xSemaphoreTake(bufferMutex, pdMS_TO_TICKS(SystemConstants::Timing::MUTEX_SAFELOG_TIMEOUT_MS)) == pdTRUE) {
            vsnprintf(smallBuffer, sizeof(smallBuffer), fmt, args...);
            xSemaphoreGive(bufferMutex);
        }
        return smallBuffer;
    }
    
    /**
     * @brief Get a temporary buffer for string operations
     * @return Pointer to 256-byte buffer
     */
    static char* getTempBuffer() {
        ensureInit();
        return buffer;
    }
    
    /**
     * @brief Get buffer size
     * @return Size of main buffer
     */
    static constexpr size_t getBufferSize() {
        return sizeof(buffer);
    }
};

#endif // SAFE_FORMATTER_H