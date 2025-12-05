// include/utils/MemoryManager.h
#ifndef MEMORY_MANAGER_H
#define MEMORY_MANAGER_H

#include <cstddef>
#include "config/SystemConstants.h"

/**
 * @brief Memory management utilities for ESP32
 * 
 * Provides memory checking and cleanup utilities to prevent
 * out-of-memory crashes during critical operations.
 */
class MemoryManager {
public:
    /**
     * @brief Check if there's enough memory for an operation
     * @param operation Name of the operation (for logging)
     * @param requiredBytes Minimum bytes required
     * @return true if enough memory is available
     */
    static bool checkMemoryForOperation(const char* operation, size_t requiredBytes);
    
    /**
     * @brief Perform emergency memory cleanup
     * @return true if any memory was freed
     */
    static bool performEmergencyCleanup();
    
    /**
     * @brief Log current memory status
     * @param context Context string for the log
     */
    static void logMemoryStatus(const char* context);
    
    // Memory thresholds from SystemConstants
    static constexpr size_t MIN_HEAP_FOR_MQTT = SystemConstants::System::MIN_HEAP_FOR_MQTT;
    static constexpr size_t MIN_HEAP_FOR_OPERATION = SystemConstants::System::MIN_HEAP_FOR_OPERATION;
    static constexpr size_t CRITICAL_HEAP_THRESHOLD = SystemConstants::System::CRITICAL_HEAP_THRESHOLD;
};

#endif // MEMORY_MANAGER_H