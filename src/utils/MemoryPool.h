// src/utils/MemoryPool.h
#ifndef MEMORY_POOL_H
#define MEMORY_POOL_H

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdint>
#include <cstring>
#include "LoggingMacros.h"

/**
 * @brief Thread-safe memory pool for frequent small allocations
 * 
 * This pool helps reduce memory fragmentation by reusing fixed-size blocks
 * instead of repeatedly calling malloc/free.
 * 
 * @tparam T Type of object to pool
 * @tparam N Number of objects in the pool
 */
template<typename T, size_t N>
class MemoryPool {
public:
    MemoryPool() : blocks_(nullptr), freeList_(nullptr), mutex_(nullptr), allocCount_(0), freeCount_(0), initialized_(false) {
        static_assert(sizeof(T) >= sizeof(void*), "Type too small for free list");
        // Defer initialization until first use to avoid FreeRTOS calls before scheduler starts
    }
    
    void lazyInit() {
        if (initialized_) return;
        
        // Create mutex for thread safety
        mutex_ = xSemaphoreCreateMutex();
        if (mutex_ == nullptr) {
            LOG_ERROR("MemoryPool", "Failed to create mutex");
            return;
        }
        
        // Allocate memory dynamically on first use
        blocks_ = static_cast<Block*>(malloc(N * sizeof(Block)));
        if (blocks_ == nullptr) {
            LOG_ERROR("MemoryPool", "Failed to allocate %d bytes", N * sizeof(Block));
            vSemaphoreDelete(mutex_);
            mutex_ = nullptr;
            return;
        }
        
        // Initialize free list
        for (size_t i = 0; i < N; ++i) {
            blocks_[i].next = freeList_;
            freeList_ = &blocks_[i];
        }
        
        LOG_INFO("MemoryPool", "Initialized pool with %d blocks of %d bytes", N, sizeof(T));
        initialized_ = true;
    }
    
    ~MemoryPool() {
        if (mutex_ != nullptr) {
            vSemaphoreDelete(mutex_);
        }
        if (blocks_ != nullptr) {
            free(blocks_);
        }
    }
    
    /**
     * @brief Allocate an object from the pool
     * @return Pointer to allocated object, or nullptr if pool is empty
     */
    T* allocate() {
        lazyInit();  // Ensure pool is initialized
        
        if (!initialized_ || mutex_ == nullptr) {
            return nullptr;
        }
        
        if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(10)) != pdTRUE) {
            return nullptr;
        }
        
        Block* block = freeList_;
        if (block != nullptr) {
            freeList_ = block->next;
            allocCount_++;
            xSemaphoreGive(mutex_);
            
            // Clear the memory
            memset(&block->data, 0, sizeof(T));
            return reinterpret_cast<T*>(&block->data);
        }
        
        xSemaphoreGive(mutex_);
        LOG_WARN("MemoryPool", "Pool exhausted - %d allocations, %d frees", allocCount_, freeCount_);
        return nullptr;
    }
    
    /**
     * @brief Return an object to the pool
     * @param ptr Pointer to object to deallocate
     */
    void deallocate(T* ptr) {
        if (ptr == nullptr) return;
        
        // Verify pointer is within our pool
        Block* block = reinterpret_cast<Block*>(ptr);
        if (block < blocks_ || block >= blocks_ + N) {
            LOG_ERROR("MemoryPool", "Invalid pointer - not from this pool");
            return;
        }
        
        if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(10)) != pdTRUE) {
            LOG_ERROR("MemoryPool", "Failed to acquire mutex for deallocation");
            return;
        }
        
        // Call destructor
        ptr->~T();
        
        // Add back to free list
        block->next = freeList_;
        freeList_ = block;
        freeCount_++;
        
        xSemaphoreGive(mutex_);
    }
    
    /**
     * @brief Get pool statistics
     */
    struct Stats {
        size_t totalBlocks;
        size_t usedBlocks;
        size_t freeBlocks;
        uint32_t totalAllocations;
        uint32_t totalDeallocations;
    };
    
    Stats getStats() {
        lazyInit();  // Ensure pool is initialized
        
        Stats stats;
        stats.totalBlocks = N;
        stats.totalAllocations = allocCount_;
        stats.totalDeallocations = freeCount_;
        
        // Count free blocks
        if (initialized_ && mutex_ != nullptr && xSemaphoreTake(mutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
            size_t freeCount = 0;
            Block* current = freeList_;
            while (current != nullptr) {
                freeCount++;
                current = current->next;
            }
            stats.freeBlocks = freeCount;
            stats.usedBlocks = N - freeCount;
            xSemaphoreGive(mutex_);
        } else {
            stats.freeBlocks = 0;
            stats.usedBlocks = N;
        }
        
        return stats;
    }
    
private:
    // Use union to ensure proper alignment and size
    union Block {
        alignas(T) char data[sizeof(T)];
        Block* next;
    };
    
    Block* blocks_;  // Changed from static array to pointer
    Block* freeList_;
    SemaphoreHandle_t mutex_;
    uint32_t allocCount_;
    uint32_t freeCount_;
    bool initialized_;
    
    // Disable copy and assignment
    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
};

/**
 * @brief Specialized memory pools for common allocations
 */
namespace MemoryPools {
    // Pool for MQTT message buffers (256 bytes each, reduced from 10 to 4)
    struct MqttBuffer {
        char data[256];
    };
    extern MemoryPool<MqttBuffer, 4> mqttBufferPool;
    
    // Pool for sensor reading structures (32 bytes each, reduced from 20 to 8)
    struct SensorReading {
        float value;
        uint32_t timestamp;
        uint8_t sensorId;
        bool valid;
        char padding[18]; // Align to 32 bytes
    };
    extern MemoryPool<SensorReading, 8> sensorReadingPool;
    
    // Pool for small JSON documents (512 bytes each, reduced from 5 to 3)
    struct JsonDocBuffer {
        char data[512];
    };
    extern MemoryPool<JsonDocBuffer, 3> jsonBufferPool;
    
    // Pool for string formatting buffers (128 bytes each, reduced from 8 to 4)
    struct StringBuffer {
        char data[128];
    };
    extern MemoryPool<StringBuffer, 4> stringBufferPool;
    
    // Pool for log message buffers (256 bytes each, reduced from 6 to 3)
    struct LogBuffer {
        char data[256];
    };
    extern MemoryPool<LogBuffer, 3> logBufferPool;
    
    // Pool for small temporary buffers (64 bytes each, reduced from 12 to 6)
    struct TempBuffer {
        char data[64];
    };
    extern MemoryPool<TempBuffer, 6> tempBufferPool;
}

#endif // MEMORY_POOL_H