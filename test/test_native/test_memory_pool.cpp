/**
 * @file test_memory_pool.cpp
 * @brief Unit tests for MemoryPool implementation
 */

#include <unity.h>
#include <cstring>
#include <set>

// Mock dependencies for native testing
#ifdef NATIVE_TEST

// Simple memory pool implementation for testing
template<size_t BlockSize, size_t BlockCount>
class MemoryPool {
private:
    struct Block {
        union {
            uint8_t data[BlockSize];
            Block* next;
        };
    };
    
    Block blocks_[BlockCount];
    Block* freeList_;
    size_t allocatedCount_;
    size_t peakAllocated_;
    size_t totalAllocations_;
    size_t totalDeallocations_;
    
public:
    MemoryPool() : freeList_(nullptr), allocatedCount_(0), 
                   peakAllocated_(0), totalAllocations_(0), 
                   totalDeallocations_(0) {
        // Initialize free list
        for (size_t i = 0; i < BlockCount - 1; ++i) {
            blocks_[i].next = &blocks_[i + 1];
        }
        blocks_[BlockCount - 1].next = nullptr;
        freeList_ = &blocks_[0];
    }
    
    void* allocate() {
        if (freeList_ == nullptr) {
            return nullptr;  // Pool exhausted
        }
        
        Block* block = freeList_;
        freeList_ = freeList_->next;
        
        allocatedCount_++;
        totalAllocations_++;
        
        if (allocatedCount_ > peakAllocated_) {
            peakAllocated_ = allocatedCount_;
        }
        
        return block->data;
    }
    
    bool deallocate(void* ptr) {
        if (ptr == nullptr) {
            return false;
        }
        
        // Check if pointer is within our pool
        uint8_t* p = static_cast<uint8_t*>(ptr);
        uint8_t* poolStart = reinterpret_cast<uint8_t*>(&blocks_[0]);
        uint8_t* poolEnd = reinterpret_cast<uint8_t*>(&blocks_[BlockCount]);
        
        if (p < poolStart || p >= poolEnd) {
            return false;  // Not from this pool
        }
        
        // Check alignment
        size_t offset = p - poolStart;
        if (offset % sizeof(Block) != 0) {
            return false;  // Misaligned
        }
        
        Block* block = reinterpret_cast<Block*>(ptr);
        block->next = freeList_;
        freeList_ = block;
        
        allocatedCount_--;
        totalDeallocations_++;
        
        return true;
    }
    
    size_t getBlockSize() const { return BlockSize; }
    size_t getBlockCount() const { return BlockCount; }
    size_t getAllocatedCount() const { return allocatedCount_; }
    size_t getAvailableCount() const { return BlockCount - allocatedCount_; }
    size_t getPeakAllocated() const { return peakAllocated_; }
    size_t getTotalAllocations() const { return totalAllocations_; }
    size_t getTotalDeallocations() const { return totalDeallocations_; }
    
    void resetStatistics() {
        peakAllocated_ = allocatedCount_;
        totalAllocations_ = 0;
        totalDeallocations_ = 0;
    }
    
    bool contains(void* ptr) const {
        if (ptr == nullptr) return false;
        
        uint8_t* p = static_cast<uint8_t*>(ptr);
        uint8_t* poolStart = reinterpret_cast<uint8_t*>(const_cast<Block*>(&blocks_[0]));
        uint8_t* poolEnd = reinterpret_cast<uint8_t*>(const_cast<Block*>(&blocks_[BlockCount]));
        
        return p >= poolStart && p < poolEnd;
    }
};

// RAII wrapper for pool allocations
template<typename Pool>
class PooledPtr {
private:
    Pool* pool_;
    void* ptr_;
    
public:
    explicit PooledPtr(Pool* pool) : pool_(pool), ptr_(nullptr) {
        if (pool_) {
            ptr_ = pool_->allocate();
        }
    }
    
    ~PooledPtr() {
        if (pool_ && ptr_) {
            pool_->deallocate(ptr_);
        }
    }
    
    // Disable copy
    PooledPtr(const PooledPtr&) = delete;
    PooledPtr& operator=(const PooledPtr&) = delete;
    
    // Enable move
    PooledPtr(PooledPtr&& other) noexcept 
        : pool_(other.pool_), ptr_(other.ptr_) {
        other.pool_ = nullptr;
        other.ptr_ = nullptr;
    }
    
    PooledPtr& operator=(PooledPtr&& other) noexcept {
        if (this != &other) {
            if (pool_ && ptr_) {
                pool_->deallocate(ptr_);
            }
            pool_ = other.pool_;
            ptr_ = other.ptr_;
            other.pool_ = nullptr;
            other.ptr_ = nullptr;
        }
        return *this;
    }
    
    void* get() { return ptr_; }
    const void* get() const { return ptr_; }
    bool isValid() const { return ptr_ != nullptr; }
    
    void* release() {
        void* p = ptr_;
        ptr_ = nullptr;
        pool_ = nullptr;
        return p;
    }
};

#endif // NATIVE_TEST

// Test pools
static MemoryPool<64, 8>* smallPool = nullptr;
static MemoryPool<256, 4>* mediumPool = nullptr;
static MemoryPool<1024, 2>* largePool = nullptr;

void setupMemoryPool(void) {
    smallPool = new MemoryPool<64, 8>();
    mediumPool = new MemoryPool<256, 4>();
    largePool = new MemoryPool<1024, 2>();
}

void tearDownMemoryPool(void) {
    delete smallPool;
    delete mediumPool;
    delete largePool;
}

// Test basic allocation and deallocation
void test_basic_allocation() {
    setupMemoryPool();
    // Test small pool
    void* ptr1 = smallPool->allocate();
    TEST_ASSERT_NOT_NULL(ptr1);
    TEST_ASSERT_EQUAL(1, smallPool->getAllocatedCount());
    TEST_ASSERT_EQUAL(7, smallPool->getAvailableCount());
    
    void* ptr2 = smallPool->allocate();
    TEST_ASSERT_NOT_NULL(ptr2);
    TEST_ASSERT_NOT_EQUAL(ptr1, ptr2);  // Different blocks
    TEST_ASSERT_EQUAL(2, smallPool->getAllocatedCount());
    
    // Deallocate
    TEST_ASSERT_TRUE(smallPool->deallocate(ptr1));
    TEST_ASSERT_EQUAL(1, smallPool->getAllocatedCount());
    
    TEST_ASSERT_TRUE(smallPool->deallocate(ptr2));
    TEST_ASSERT_EQUAL(0, smallPool->getAllocatedCount());
    tearDownMemoryPool();
}

// Test pool exhaustion
void test_pool_exhaustion() {
    setupMemoryPool();
    void* ptrs[8];
    
    // Allocate all blocks
    for (int i = 0; i < 8; i++) {
        ptrs[i] = smallPool->allocate();
        TEST_ASSERT_NOT_NULL(ptrs[i]);
    }
    
    TEST_ASSERT_EQUAL(8, smallPool->getAllocatedCount());
    TEST_ASSERT_EQUAL(0, smallPool->getAvailableCount());
    
    // Try to allocate one more
    void* extra = smallPool->allocate();
    TEST_ASSERT_NULL(extra);  // Should fail
    
    // Free one and try again
    smallPool->deallocate(ptrs[0]);
    extra = smallPool->allocate();
    TEST_ASSERT_NOT_NULL(extra);
    TEST_ASSERT_EQUAL(ptrs[0], extra);  // Should reuse the freed block
    tearDownMemoryPool();
}

// Test invalid deallocations
void test_invalid_deallocation() {
    setupMemoryPool();
    // Null pointer
    TEST_ASSERT_FALSE(smallPool->deallocate(nullptr));
    
    // Pointer not from pool
    int stackVar = 42;
    TEST_ASSERT_FALSE(smallPool->deallocate(&stackVar));
    
    // Pointer from different pool
    void* mediumPtr = mediumPool->allocate();
    TEST_ASSERT_FALSE(smallPool->deallocate(mediumPtr));
    mediumPool->deallocate(mediumPtr);
    tearDownMemoryPool();
}

// Test statistics tracking
void test_statistics() {
    setupMemoryPool();
    smallPool->resetStatistics();
    
    // Initial state
    TEST_ASSERT_EQUAL(0, smallPool->getPeakAllocated());
    TEST_ASSERT_EQUAL(0, smallPool->getTotalAllocations());
    TEST_ASSERT_EQUAL(0, smallPool->getTotalDeallocations());
    
    // Allocate and track
    void* ptr1 = smallPool->allocate();
    void* ptr2 = smallPool->allocate();
    void* ptr3 = smallPool->allocate();
    
    TEST_ASSERT_EQUAL(3, smallPool->getAllocatedCount());
    TEST_ASSERT_EQUAL(3, smallPool->getPeakAllocated());
    TEST_ASSERT_EQUAL(3, smallPool->getTotalAllocations());
    
    // Deallocate some
    smallPool->deallocate(ptr2);
    TEST_ASSERT_EQUAL(2, smallPool->getAllocatedCount());
    TEST_ASSERT_EQUAL(3, smallPool->getPeakAllocated());  // Peak unchanged
    TEST_ASSERT_EQUAL(1, smallPool->getTotalDeallocations());
    
    // Allocate again
    void* ptr4 = smallPool->allocate();
    TEST_ASSERT_EQUAL(3, smallPool->getAllocatedCount());
    TEST_ASSERT_EQUAL(3, smallPool->getPeakAllocated());
    TEST_ASSERT_EQUAL(4, smallPool->getTotalAllocations());
    tearDownMemoryPool();
}

// Test RAII wrapper
void test_raii_wrapper() {
    setupMemoryPool();
    {
        PooledPtr<MemoryPool<64, 8>> ptr(smallPool);
        TEST_ASSERT_TRUE(ptr.isValid());
        TEST_ASSERT_NOT_NULL(ptr.get());
        TEST_ASSERT_EQUAL(1, smallPool->getAllocatedCount());
        
        // Use the memory
        memset(ptr.get(), 0xAA, 64);
    }
    // ptr goes out of scope, should deallocate
    TEST_ASSERT_EQUAL(0, smallPool->getAllocatedCount());
    tearDownMemoryPool();
}

// Test move semantics of RAII wrapper
void test_raii_move() {
    setupMemoryPool();
    PooledPtr<MemoryPool<64, 8>> ptr1(smallPool);
    TEST_ASSERT_TRUE(ptr1.isValid());
    void* originalPtr = ptr1.get();
    
    // Move construct
    PooledPtr<MemoryPool<64, 8>> ptr2(std::move(ptr1));
    TEST_ASSERT_FALSE(ptr1.isValid());
    TEST_ASSERT_TRUE(ptr2.isValid());
    TEST_ASSERT_EQUAL(originalPtr, ptr2.get());
    TEST_ASSERT_EQUAL(1, smallPool->getAllocatedCount());
    
    // Move assign
    PooledPtr<MemoryPool<64, 8>> ptr3(smallPool);
    TEST_ASSERT_EQUAL(2, smallPool->getAllocatedCount());
    
    ptr3 = std::move(ptr2);
    TEST_ASSERT_FALSE(ptr2.isValid());
    TEST_ASSERT_TRUE(ptr3.isValid());
    TEST_ASSERT_EQUAL(originalPtr, ptr3.get());
    TEST_ASSERT_EQUAL(1, smallPool->getAllocatedCount());  // ptr3's original allocation freed
    tearDownMemoryPool();
}

// Test contains method
void test_contains() {
    setupMemoryPool();
    void* ptr1 = smallPool->allocate();
    void* ptr2 = mediumPool->allocate();
    
    TEST_ASSERT_TRUE(smallPool->contains(ptr1));
    TEST_ASSERT_FALSE(smallPool->contains(ptr2));
    TEST_ASSERT_FALSE(smallPool->contains(nullptr));
    
    int stackVar = 42;
    TEST_ASSERT_FALSE(smallPool->contains(&stackVar));
    
    smallPool->deallocate(ptr1);
    mediumPool->deallocate(ptr2);
    tearDownMemoryPool();
}

// Test different pool sizes
void test_different_pool_sizes() {
    setupMemoryPool();
    // Verify pool configurations
    TEST_ASSERT_EQUAL(64, smallPool->getBlockSize());
    TEST_ASSERT_EQUAL(8, smallPool->getBlockCount());
    
    TEST_ASSERT_EQUAL(256, mediumPool->getBlockSize());
    TEST_ASSERT_EQUAL(4, mediumPool->getBlockCount());
    
    TEST_ASSERT_EQUAL(1024, largePool->getBlockSize());
    TEST_ASSERT_EQUAL(2, largePool->getBlockCount());
    
    // Test allocation from each
    void* small = smallPool->allocate();
    void* medium = mediumPool->allocate();
    void* large = largePool->allocate();
    
    TEST_ASSERT_NOT_NULL(small);
    TEST_ASSERT_NOT_NULL(medium);
    TEST_ASSERT_NOT_NULL(large);
    
    // Write to each to verify size
    memset(small, 0x11, 64);
    memset(medium, 0x22, 256);
    memset(large, 0x33, 1024);
    
    smallPool->deallocate(small);
    mediumPool->deallocate(medium);
    largePool->deallocate(large);
    tearDownMemoryPool();
}

// Test allocation pattern stress
void test_allocation_pattern_stress() {
    setupMemoryPool();
    std::set<void*> allocated;
    
    // Allocate-deallocate pattern
    for (int round = 0; round < 10; round++) {
        // Allocate half
        for (int i = 0; i < 4; i++) {
            void* ptr = smallPool->allocate();
            TEST_ASSERT_NOT_NULL(ptr);
            allocated.insert(ptr);
        }
        
        // Deallocate all
        for (void* ptr : allocated) {
            TEST_ASSERT_TRUE(smallPool->deallocate(ptr));
        }
        allocated.clear();
        
        TEST_ASSERT_EQUAL(0, smallPool->getAllocatedCount());
    }
    
    // Verify statistics
    TEST_ASSERT_EQUAL(40, smallPool->getTotalAllocations());
    TEST_ASSERT_EQUAL(40, smallPool->getTotalDeallocations());
    TEST_ASSERT_EQUAL(4, smallPool->getPeakAllocated());
    tearDownMemoryPool();
}

// main() is in test_main.cpp