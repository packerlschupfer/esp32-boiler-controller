// src/utils/ErrorLogFRAM.h
// FRAM-based error logging system
#ifndef ERROR_LOG_FRAM_H
#define ERROR_LOG_FRAM_H

#include <cstdint>
#include <cstring>
#include <RuntimeStorage.h>
#include <RuntimeStorageTypes.h>
#include "config/SystemConstants.h"

// Forward declaration to avoid circular dependency
enum class SystemError : uint32_t;

/**
 * @brief FRAM-based error logging system
 * 
 * Stores error history in FRAM for high-endurance logging.
 * Designed to handle error floods without wearing out storage.
 */
class ErrorLogFRAM {
public:
    // Error log entry structure
    struct ErrorEntry {
        uint32_t timestamp;
        uint32_t errorCode;
        uint16_t count;
        char message[64];
        char context[32];
        
        ErrorEntry() : timestamp(0), errorCode(0), count(0) {
            message[0] = '\0';
            context[0] = '\0';
        }
    } __attribute__((packed));

    // Error statistics
    struct ErrorStats {
        uint32_t totalErrors;
        uint32_t criticalErrors;
        uint32_t lastErrorTime;
        uint32_t oldestErrorTime;
        uint16_t uniqueErrors;
        uint32_t crc;
    };
    
    /**
     * @brief Initialize error logging system
     */
    static bool begin(rtstorage::RuntimeStorage* storage);
    
    /**
     * @brief Log an error to FRAM
     */
    static void logError(SystemError error, const char* message = nullptr, 
                        const char* context = nullptr);
    
    /**
     * @brief Log a critical error
     */
    static void logCriticalError(SystemError error, const char* message = nullptr,
                                const char* context = nullptr);
    
    /**
     * @brief Get error at index (0 = most recent)
     */
    static bool getError(size_t index, ErrorEntry& entry);
    
    /**
     * @brief Get error statistics
     */
    static ErrorStats getStats();
    
    /**
     * @brief Clear all error logs
     */
    static void clear();
    
    /**
     * @brief Clear errors older than specified days
     */
    static void clearOldErrors(uint32_t daysOld = 30);
    
    /**
     * @brief Get number of stored errors
     */
    static size_t getErrorCount();
    
    /**
     * @brief Get last N critical errors
     */
    static size_t getCriticalErrors(ErrorEntry* buffer, size_t maxCount);
    
    /**
     * @brief Export errors as JSON string
     */
    static bool exportToJson(char* buffer, size_t bufferSize, size_t maxErrors = 10);
    
private:
    static rtstorage::RuntimeStorage* storage_;
    static bool initialized_;
    static ErrorStats cachedStats_;
    
    /**
     * @brief Update cached statistics
     */
    static void updateCachedStats();
};

#endif // ERROR_LOG_FRAM_H