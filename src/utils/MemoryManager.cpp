// src/utils/MemoryManager.cpp
#include "utils/MemoryManager.h"
#include "LoggingMacros.h"
#include <Arduino.h>
#include <esp_system.h>
#include <esp_heap_caps.h>

static const char* TAG = "MemoryManager";

bool MemoryManager::checkMemoryForOperation(const char* operation, size_t requiredBytes) {
    size_t freeHeap = ESP.getFreeHeap();
    size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    
    LOG_INFO(TAG, "Memory check for %s: Free=%d, Largest=%d, Required=%d", 
             operation, freeHeap, largestBlock, requiredBytes);
    
    if (freeHeap < requiredBytes) {
        LOG_ERROR(TAG, "Insufficient memory for %s: %d < %d", 
                  operation, freeHeap, requiredBytes);
        return false;
    }
    
    if (largestBlock < (requiredBytes / 2)) {
        LOG_WARN(TAG, "Memory fragmentation detected for %s: largest block %d", 
                 operation, largestBlock);
    }
    
    return true;
}

bool MemoryManager::performEmergencyCleanup() {
    LOG_WARN(TAG, "Performing emergency memory cleanup");
    
    size_t beforeHeap = ESP.getFreeHeap();
    
    // 1. Force heap compaction
    heap_caps_malloc_extmem_enable(0);
    heap_caps_malloc_extmem_enable(16384);
    
    // 2. Clear any caches or buffers in your application
    // This is application-specific
    
    size_t afterHeap = ESP.getFreeHeap();
    size_t freed = afterHeap - beforeHeap;
    
    LOG_INFO(TAG, "Emergency cleanup freed %d bytes", freed);
    return freed > 0;
}

void MemoryManager::logMemoryStatus(const char* context) {
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_DEFAULT);
    
    LOG_INFO(TAG, "[%s] Memory Status:", context);
    uint32_t total = info.total_free_bytes + info.total_allocated_bytes;
    uint32_t freePercent = total > 0 ? (info.total_free_bytes * 100) / total : 0;
    LOG_INFO(TAG, "  Free: %d bytes (%lu%%)", 
             info.total_free_bytes, freePercent);
    LOG_INFO(TAG, "  Allocated: %d bytes", info.total_allocated_bytes);
    LOG_INFO(TAG, "  Largest free block: %d bytes", info.largest_free_block);
    LOG_INFO(TAG, "  Min free ever: %d bytes", info.minimum_free_bytes);
    uint32_t fragmentation = info.total_free_bytes > 0 ? 
                            100 - (info.largest_free_block * 100 / info.total_free_bytes) : 0;
    LOG_INFO(TAG, "  Fragmentation: %lu%%", fragmentation);
}