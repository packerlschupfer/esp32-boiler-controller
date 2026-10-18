// include/utils/MemoryPools.h
#ifndef MEMORY_POOLS_H
#define MEMORY_POOLS_H

#include "MemoryPool.h"
#include <cstdint>

/**
 * @brief Centralized memory pools for common allocations
 * 
 * Provides pre-allocated pools for frequently used buffer sizes
 * to reduce heap fragmentation and allocation overhead.
 */
namespace MemoryPools {
    
    // Buffer type for raw memory allocations
    template<size_t Size>
    struct Buffer {
        uint8_t data[Size];
        
        operator uint8_t*() { return data; }
        operator const uint8_t*() const { return data; }
        operator char*() { return reinterpret_cast<char*>(data); }
        operator const char*() const { return reinterpret_cast<const char*>(data); }
    };
    
    /**
     * @brief Pool for MQTT topic strings (64 bytes)
     * Typical usage: "esplan/ESPlan-Boiler/diagnostics/health"
     */
    inline MemoryPool<Buffer<64>, 16>& getTopicPool() {
        static MemoryPool<Buffer<64>, 16> pool;
        return pool;
    }
    
    /**
     * @brief Pool for small JSON payloads (256 bytes)
     * Typical usage: Health status, simple sensor data
     */
    inline MemoryPool<Buffer<256>, 8>& getSmallJsonPool() {
        static MemoryPool<Buffer<256>, 8> pool;
        return pool;
    }
    
    /**
     * @brief Pool for large JSON payloads (512 bytes)
     * Typical usage: Diagnostics data, system status
     */
    inline MemoryPool<Buffer<512>, 4>& getLargeJsonPool() {
        static MemoryPool<Buffer<512>, 4> pool;
        return pool;
    }
    
    /**
     * @brief Pool for log message formatting (128 bytes)
     * Typical usage: Formatted log messages
     */
    inline MemoryPool<Buffer<128>, 12>& getLogPool() {
        static MemoryPool<Buffer<128>, 12> pool;
        return pool;
    }
    
    /**
     * @brief Pool for general string operations (192 bytes)
     * Typical usage: Temporary string concatenation
     */
    inline MemoryPool<Buffer<192>, 8>& getStringPool() {
        static MemoryPool<Buffer<192>, 8> pool;
        return pool;
    }
    
    /**
     * @brief RAII wrapper for pool allocations
     */
    template<typename Pool>
    class PooledBuffer {
    private:
        Pool& pool_;
        typename Pool::value_type* buffer_;
        
    public:
        explicit PooledBuffer(Pool& pool) : pool_(pool), buffer_(pool.allocate()) {}
        
        ~PooledBuffer() {
            if (buffer_) {
                pool_.deallocate(buffer_);
            }
        }
        
        // Disable copy
        PooledBuffer(const PooledBuffer&) = delete;
        PooledBuffer& operator=(const PooledBuffer&) = delete;
        
        // Enable move
        PooledBuffer(PooledBuffer&& other) noexcept 
            : pool_(other.pool_), buffer_(other.buffer_) {
            other.buffer_ = nullptr;
        }
        
        bool isValid() const { return buffer_ != nullptr; }
        
        char* get() { 
            return buffer_ ? static_cast<char*>(*buffer_) : nullptr; 
        }
        
        const char* get() const { 
            return buffer_ ? static_cast<const char*>(*buffer_) : nullptr; 
        }
        
        size_t size() const {
            return buffer_ ? sizeof(*buffer_) : 0;
        }
    };
    
    // Helper functions for common operations
    template<size_t Size>
    PooledBuffer<MemoryPool<Buffer<Size>, 16>> getTopic() {
        return PooledBuffer<MemoryPool<Buffer<Size>, 16>>(getTopicPool());
    }
    
    PooledBuffer<MemoryPool<Buffer<256>, 8>> getSmallJson() {
        return PooledBuffer<MemoryPool<Buffer<256>, 8>>(getSmallJsonPool());
    }
    
    PooledBuffer<MemoryPool<Buffer<512>, 4>> getLargeJson() {
        return PooledBuffer<MemoryPool<Buffer<512>, 4>>(getLargeJsonPool());
    }
    
    /**
     * @brief Get memory pool statistics
     */
    struct PoolStats {
        size_t totalBlocks;
        size_t usedBlocks;
        size_t blockSize;
        
        size_t getTotalBytes() const { return totalBlocks * blockSize; }
        size_t getUsedBytes() const { return usedBlocks * blockSize; }
        float getUtilization() const { 
            return totalBlocks > 0 ? (100.0f * usedBlocks / totalBlocks) : 0.0f; 
        }
    };
    
    // Get statistics for monitoring
    inline void getPoolStatistics(std::vector<PoolStats>& stats) {
        stats.clear();
        
        // Topic pool
        auto& topicPool = getTopicPool();
        stats.push_back({16, topicPool.getAllocCount() - topicPool.getFreeCount(), 64});
        
        // Small JSON pool
        auto& smallJson = getSmallJsonPool();
        stats.push_back({8, smallJson.getAllocCount() - smallJson.getFreeCount(), 256});
        
        // Large JSON pool
        auto& largeJson = getLargeJsonPool();
        stats.push_back({4, largeJson.getAllocCount() - largeJson.getFreeCount(), 512});
        
        // Log pool
        auto& logPool = getLogPool();
        stats.push_back({12, logPool.getAllocCount() - logPool.getFreeCount(), 128});
        
        // String pool
        auto& stringPool = getStringPool();
        stats.push_back({8, stringPool.getAllocCount() - stringPool.getFreeCount(), 192});
    }
}

#endif // MEMORY_POOLS_H