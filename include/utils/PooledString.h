// include/utils/PooledString.h
#ifndef POOLED_STRING_H
#define POOLED_STRING_H

#include "utils/MemoryPool.h"
#include <cstring>
#include <cstdio>
#include <cstdarg>

/**
 * @brief RAII wrapper for pooled string buffers
 * 
 * Automatically allocates from pool on construction and
 * returns to pool on destruction. Provides printf-style
 * formatting for convenience.
 * 
 * Usage:
 * {
 *     auto str = MemoryPools::getString();
 *     if (str) {
 *         str.printf("Temperature: %.1f°C", temp);
 *         publish(str.c_str());
 *     }
 * } // Automatically returned to pool
 */
template<typename BufferType, typename PoolType>
class PooledString {
public:
    PooledString() : buffer_(nullptr), pool_(nullptr) {}
    
    PooledString(BufferType* buffer, PoolType* pool)
        : buffer_(buffer), pool_(pool) {
        if (buffer_) {
            buffer_->data[0] = '\0';  // Initialize as empty string
        }
    }
    
    ~PooledString() {
        if (buffer_ && pool_) {
            pool_->deallocate(buffer_);
        }
    }
    
    // Move constructor
    PooledString(PooledString&& other) noexcept 
        : buffer_(other.buffer_), pool_(other.pool_) {
        other.buffer_ = nullptr;
        other.pool_ = nullptr;
    }
    
    // Move assignment
    PooledString& operator=(PooledString&& other) noexcept {
        if (this != &other) {
            if (buffer_ && pool_) {
                pool_->deallocate(buffer_);
            }
            buffer_ = other.buffer_;
            pool_ = other.pool_;
            other.buffer_ = nullptr;
            other.pool_ = nullptr;
        }
        return *this;
    }
    
    // Delete copy operations
    PooledString(const PooledString&) = delete;
    PooledString& operator=(const PooledString&) = delete;
    
    // Check if allocation succeeded
    operator bool() const { return buffer_ != nullptr; }
    
    // Get C string
    const char* c_str() const {
        return buffer_ ? buffer_->data : "";
    }
    
    char* data() {
        return buffer_ ? buffer_->data : nullptr;
    }
    
    // Get buffer size
    size_t size() const {
        return buffer_ ? sizeof(buffer_->data) : 0;
    }
    
    // Printf-style formatting
    int printf(const char* format, ...) {
        if (!buffer_) return -1;
        
        va_list args;
        va_start(args, format);
        int result = vsnprintf(buffer_->data, sizeof(buffer_->data), format, args);
        va_end(args);
        
        return result;
    }
    
    // Copy string
    void copy(const char* str) {
        if (buffer_ && str) {
            strncpy(buffer_->data, str, sizeof(buffer_->data) - 1);
            buffer_->data[sizeof(buffer_->data) - 1] = '\0';
        }
    }
    
private:
    BufferType* buffer_;
    PoolType* pool_;
};

// Convenience typedefs for specific pool types (updated to match new pool sizes)
using StringPooledString = PooledString<MemoryPools::StringBuffer, MemoryPool<MemoryPools::StringBuffer, 4>>;
using LogPooledString = PooledString<MemoryPools::LogBuffer, MemoryPool<MemoryPools::LogBuffer, 3>>;
using TempPooledString = PooledString<MemoryPools::TempBuffer, MemoryPool<MemoryPools::TempBuffer, 6>>;

// Convenience factory functions
namespace MemoryPools {
    inline StringPooledString getString() {
        auto* buffer = stringBufferPool.allocate();
        return StringPooledString(buffer, &stringBufferPool);
    }
    
    inline LogPooledString getLogBuffer() {
        auto* buffer = logBufferPool.allocate();
        return LogPooledString(buffer, &logBufferPool);
    }
    
    inline TempPooledString getTempBuffer() {
        auto* buffer = tempBufferPool.allocate();
        return TempPooledString(buffer, &tempBufferPool);
    }
}

#endif // POOLED_STRING_H