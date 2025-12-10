// src/utils/StringUtils.h
#ifndef STRING_UTILS_H
#define STRING_UTILS_H

#include <cstring>
#include <cstdio>

/**
 * @brief Utility functions for efficient string handling
 * 
 * These utilities help reduce memory usage by avoiding std::string
 * allocations in critical paths.
 */
class StringUtils {
public:
    /**
     * @brief Safe string concatenation with buffer size checking
     * 
     * @param dest Destination buffer
     * @param destSize Size of destination buffer
     * @param src1 First string to concatenate
     * @param src2 Second string to concatenate (optional)
     * @param src3 Third string to concatenate (optional)
     * @return true if successful, false if buffer too small
     */
    static bool safeConcat(char* dest, size_t destSize, 
                          const char* src1, 
                          const char* src2 = nullptr, 
                          const char* src3 = nullptr) {
        size_t totalLen = strlen(src1);
        if (src2) totalLen += strlen(src2);
        if (src3) totalLen += strlen(src3);
        
        if (totalLen >= destSize) {
            return false; // Would overflow
        }
        
        strncpy(dest, src1, destSize - 1);
        if (src2) strncat(dest, src2, destSize - strlen(dest) - 1);
        if (src3) strncat(dest, src3, destSize - strlen(dest) - 1);
        dest[destSize - 1] = '\0'; // Ensure null termination
        
        return true;
    }
    
    /**
     * @brief Build MQTT topic efficiently
     * 
     * @param dest Destination buffer
     * @param destSize Size of destination buffer
     * @param baseTopic Base topic (e.g., "ESPlan")
     * @param subtopic Subtopic (e.g., "diagnostics")
     * @param endpoint Endpoint (e.g., "memory")
     * @return true if successful, false if buffer too small
     */
    static bool buildMqttTopic(char* dest, size_t destSize,
                              const char* baseTopic,
                              const char* subtopic,
                              const char* endpoint = nullptr) {
        int written;
        if (endpoint) {
            written = snprintf(dest, destSize, "%s/%s/%s", baseTopic, subtopic, endpoint);
        } else {
            written = snprintf(dest, destSize, "%s/%s", baseTopic, subtopic);
        }
        
        return (written > 0 && written < (int)destSize);
    }
    
    /**
     * @brief Format float to string with fixed precision
     * 
     * @param dest Destination buffer
     * @param destSize Size of destination buffer  
     * @param value Float value to format
     * @param precision Number of decimal places
     * @return Pointer to dest for chaining
     */
    static char* formatFloat(char* dest, size_t destSize, float value, int precision = 1) {
        snprintf(dest, destSize, "%.*f", precision, value);
        return dest;
    }
    
    /**
     * @brief Format memory size in human-readable form
     * 
     * @param dest Destination buffer
     * @param destSize Size of destination buffer
     * @param bytes Number of bytes
     * @return Pointer to dest for chaining
     */
    static char* formatBytes(char* dest, size_t destSize, size_t bytes) {
        if (bytes >= 1024 * 1024) {
            // Integer math: MB with 1 decimal place
            uint32_t mb = bytes / (1024 * 1024);
            uint32_t frac = ((bytes % (1024 * 1024)) * 10) / (1024 * 1024);
            snprintf(dest, destSize, "%lu.%lu MB", (unsigned long)mb, (unsigned long)frac);
        } else if (bytes >= 1024) {
            // Integer math: KB with 1 decimal place
            uint32_t kb = bytes / 1024;
            uint32_t frac = ((bytes % 1024) * 10) / 1024;
            snprintf(dest, destSize, "%lu.%lu KB", (unsigned long)kb, (unsigned long)frac);
        } else {
            snprintf(dest, destSize, "%zu B", bytes);
        }
        return dest;
    }
    
    /**
     * @brief Format time duration in human-readable form
     * 
     * @param dest Destination buffer
     * @param destSize Size of destination buffer
     * @param milliseconds Duration in milliseconds
     * @return Pointer to dest for chaining
     */
    static char* formatDuration(char* dest, size_t destSize, uint32_t milliseconds) {
        if (milliseconds >= 60000) {
            snprintf(dest, destSize, "%lu min", (unsigned long)(milliseconds / 60000));
        } else if (milliseconds >= 1000) {
            // Integer math: seconds with 1 decimal place
            uint32_t sec = milliseconds / 1000;
            uint32_t frac = (milliseconds % 1000) / 100;
            snprintf(dest, destSize, "%lu.%lu sec", (unsigned long)sec, (unsigned long)frac);
        } else {
            snprintf(dest, destSize, "%lu ms", (unsigned long)milliseconds);
        }
        return dest;
    }
    
    /**
     * @brief Fixed-size string buffer pool for temporary strings
     * 
     * Use this to avoid dynamic allocations for temporary strings.
     * NOTE: These buffers are shared - copy data out if needed long-term.
     */
    class TempBuffer {
    public:
        static constexpr size_t BUFFER_SIZE = 128;
        static constexpr size_t POOL_SIZE = 4;
        
        static char* get() {
            static char buffers[POOL_SIZE][BUFFER_SIZE];
            static uint8_t currentIndex = 0;
            
            char* buffer = buffers[currentIndex];
            currentIndex = (currentIndex + 1) % POOL_SIZE;
            buffer[0] = '\0'; // Clear buffer
            return buffer;
        }
    };
};

#endif // STRING_UTILS_H