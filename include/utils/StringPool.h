// include/utils/StringPool.h
#pragma once

#include <array>
#include <cstring>

/**
 * @brief Pre-allocated string pool for MQTT operations
 * 
 * This pool avoids dynamic string allocations for common MQTT operations,
 * reducing heap fragmentation and improving performance.
 */
class StringPool {
public:
    // Buffer sizes
    static constexpr size_t SMALL_BUFFER_SIZE = 64;
    static constexpr size_t MEDIUM_BUFFER_SIZE = 128;
    static constexpr size_t LARGE_BUFFER_SIZE = 512;
    
    // Pool sizes
    static constexpr size_t SMALL_POOL_SIZE = 8;
    static constexpr size_t MEDIUM_POOL_SIZE = 4;
    static constexpr size_t LARGE_POOL_SIZE = 2;
    
    // Buffer types
    template<size_t SIZE>
    struct Buffer {
        char data[SIZE];
        bool inUse;
        
        Buffer() : inUse(false) {
            data[0] = '\0';
        }
        
        void clear() {
            data[0] = '\0';
            inUse = false;
        }
    };
    
    // Pool allocation
    template<size_t SIZE, size_t COUNT>
    class Pool {
    private:
        std::array<Buffer<SIZE>, COUNT> buffers;
        
    public:
        Buffer<SIZE>* allocate() {
            for (auto& buffer : buffers) {
                if (!buffer.inUse) {
                    buffer.inUse = true;
                    return &buffer;
                }
            }
            return nullptr;
        }
        
        void release(Buffer<SIZE>* buffer) {
            if (buffer) {
                buffer->clear();
            }
        }
        
        void releaseAll() {
            for (auto& buffer : buffers) {
                buffer.clear();
            }
        }
    };
    
    // RAII wrapper for automatic buffer release
    template<size_t SIZE>
    class ScopedBuffer {
    private:
        Buffer<SIZE>* buffer;
        Pool<SIZE, 1>* pool;  // Simplified - actual implementation would track pool
        
    public:
        ScopedBuffer(Buffer<SIZE>* buf, Pool<SIZE, 1>* p) : buffer(buf), pool(p) {}
        
        ~ScopedBuffer() {
            if (buffer && pool) {
                pool->release(buffer);
            }
        }
        
        // Prevent copying
        ScopedBuffer(const ScopedBuffer&) = delete;
        ScopedBuffer& operator=(const ScopedBuffer&) = delete;
        
        // Allow moving
        ScopedBuffer(ScopedBuffer&& other) noexcept 
            : buffer(other.buffer), pool(other.pool) {
            other.buffer = nullptr;
            other.pool = nullptr;
        }
        
        char* data() { return buffer ? buffer->data : nullptr; }
        const char* c_str() const { return buffer ? buffer->data : ""; }
        size_t size() const { return SIZE; }
        bool valid() const { return buffer != nullptr; }
        
        operator bool() const { return valid(); }
    };
    
    // Static pools
    static Pool<SMALL_BUFFER_SIZE, SMALL_POOL_SIZE> smallPool;
    static Pool<MEDIUM_BUFFER_SIZE, MEDIUM_POOL_SIZE> mediumPool;
    static Pool<LARGE_BUFFER_SIZE, LARGE_POOL_SIZE> largePool;
    
    // Helper to get appropriate pool based on size
    template<typename PoolType>
    static auto getBuffer(PoolType& pool) {
        using BufferType = decltype(pool.allocate());
        using BufferSize = std::integral_constant<size_t, 
            sizeof(std::declval<BufferType>()->data)>;
        
        auto* buffer = pool.allocate();
        return ScopedBuffer<BufferSize::value>(buffer, 
            reinterpret_cast<Pool<BufferSize::value, 1>*>(&pool));
    }
    
    // Convenience functions
    static auto getSmallBuffer() { return getBuffer(smallPool); }
    static auto getMediumBuffer() { return getBuffer(mediumPool); }
    static auto getLargeBuffer() { return getBuffer(largePool); }
    
    // Format helpers
    template<typename... Args>
    static auto format(const char* fmt, Args... args) {
        // Estimate size needed
        int size = snprintf(nullptr, 0, fmt, args...);
        
        if (size < 0) return ScopedBuffer<SMALL_BUFFER_SIZE>(nullptr, nullptr);
        
        // Select appropriate pool
        if (size < SMALL_BUFFER_SIZE) {
            auto buffer = getSmallBuffer();
            if (buffer) {
                snprintf(buffer.data(), buffer.size(), fmt, args...);
            }
            return buffer;
        } else if (size < MEDIUM_BUFFER_SIZE) {
            auto buffer = getMediumBuffer();
            if (buffer) {
                snprintf(buffer.data(), buffer.size(), fmt, args...);
            }
            return buffer;
        } else {
            auto buffer = getLargeBuffer();
            if (buffer) {
                snprintf(buffer.data(), buffer.size(), fmt, args...);
            }
            return buffer;
        }
    }
};

// Define static members
inline StringPool::Pool<StringPool::SMALL_BUFFER_SIZE, StringPool::SMALL_POOL_SIZE> 
    StringPool::smallPool;
inline StringPool::Pool<StringPool::MEDIUM_BUFFER_SIZE, StringPool::MEDIUM_POOL_SIZE> 
    StringPool::mediumPool;
inline StringPool::Pool<StringPool::LARGE_BUFFER_SIZE, StringPool::LARGE_POOL_SIZE> 
    StringPool::largePool;