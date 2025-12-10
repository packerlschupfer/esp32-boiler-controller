#ifndef RESOURCE_GUARD_H
#define RESOURCE_GUARD_H

#include <functional>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "LoggingMacros.h"

/**
 * @brief RAII guard for automatic resource cleanup
 * 
 * Ensures resources are properly released even in error conditions.
 * Supports multiple cleanup actions and exception safety.
 */
template<typename T>
class ResourceGuard {
public:
    using CleanupFunc = std::function<void(T&)>;
    
    /**
     * @brief Construct guard with resource and cleanup function
     */
    ResourceGuard(T& resource, CleanupFunc cleanup)
        : resource_(resource)
        , cleanup_(cleanup)
        , released_(false) {
    }
    
    /**
     * @brief Destructor performs cleanup if not released
     */
    ~ResourceGuard() {
        if (!released_ && cleanup_) {
            cleanup_(resource_);
        }
    }
    
    /**
     * @brief Release the guard without cleanup
     */
    void release() {
        released_ = true;
    }
    
    /**
     * @brief Force cleanup immediately
     */
    void cleanup() {
        if (!released_ && cleanup_) {
            cleanup_(resource_);
            released_ = true;
        }
    }
    
    // Disable copy
    ResourceGuard(const ResourceGuard&) = delete;
    ResourceGuard& operator=(const ResourceGuard&) = delete;
    
    // Enable move
    ResourceGuard(ResourceGuard&& other) noexcept
        : resource_(other.resource_)
        , cleanup_(std::move(other.cleanup_))
        , released_(other.released_) {
        other.released_ = true;
    }
    
private:
    T& resource_;
    CleanupFunc cleanup_;
    bool released_;
};

/**
 * @brief Scoped cleanup manager for multiple resources
 * 
 * Allows registration of multiple cleanup actions that are
 * executed in reverse order (LIFO) on scope exit.
 */
class ScopedCleanup {
public:
    using CleanupAction = std::function<void()>;
    
    ScopedCleanup() = default;
    
    ~ScopedCleanup() {
        // Execute cleanups in reverse order
        for (auto it = cleanups_.rbegin(); it != cleanups_.rend(); ++it) {
            // ESP32 doesn't have exception support by default
            // Just execute the cleanup action
            (*it)();
        }
    }
    
    /**
     * @brief Add cleanup action
     */
    void add(CleanupAction action) {
        cleanups_.push_back(action);
    }
    
    /**
     * @brief Release all cleanup actions without executing
     */
    void release() {
        cleanups_.clear();
    }
    
    // Disable copy
    ScopedCleanup(const ScopedCleanup&) = delete;
    ScopedCleanup& operator=(const ScopedCleanup&) = delete;
    
private:
    std::vector<CleanupAction> cleanups_;
};

/**
 * @brief Enhanced mutex guard with timeout and error handling
 */
class SafeMutexGuard {
public:
    SafeMutexGuard(SemaphoreHandle_t mutex, TickType_t timeout = portMAX_DELAY)
        : mutex_(mutex)
        , locked_(false) {
        if (mutex_ && xSemaphoreTake(mutex_, timeout) == pdTRUE) {
            locked_ = true;
        }
    }
    
    ~SafeMutexGuard() {
        unlock();
    }
    
    bool isLocked() const { return locked_; }
    
    void unlock() {
        if (locked_ && mutex_) {
            xSemaphoreGive(mutex_);
            locked_ = false;
        }
    }
    
    // Disable copy
    SafeMutexGuard(const SafeMutexGuard&) = delete;
    SafeMutexGuard& operator=(const SafeMutexGuard&) = delete;
    
private:
    SemaphoreHandle_t mutex_;
    bool locked_;
};

/**
 * @brief Task cleanup handler for proper task termination
 */
class TaskCleanupHandler {
public:
    using CleanupFunc = std::function<void()>;
    
    /**
     * @brief Register cleanup function for current task
     */
    static void registerCleanup(CleanupFunc cleanup) {
        // Store cleanup function in task local storage
        vTaskSetThreadLocalStoragePointer(nullptr, 0, new CleanupFunc(cleanup));
    }
    
    /**
     * @brief Execute cleanup for current task
     */
    static void executeCleanup() {
        void* ptr = pvTaskGetThreadLocalStoragePointer(nullptr, 0);
        if (ptr) {
            CleanupFunc* cleanup = static_cast<CleanupFunc*>(ptr);
            (*cleanup)();
            delete cleanup;
            vTaskSetThreadLocalStoragePointer(nullptr, 0, nullptr);
        }
    }
    
    /**
     * @brief Task deletion hook
     */
    static void taskDeletionHook(TaskHandle_t xTask) {
        void* ptr = pvTaskGetThreadLocalStoragePointer(xTask, 0);
        if (ptr) {
            CleanupFunc* cleanup = static_cast<CleanupFunc*>(ptr);
            (*cleanup)();
            delete cleanup;
        }
    }
};

/**
 * @brief Event bit guard for automatic clearing
 */
class EventBitGuard {
public:
    EventBitGuard(EventGroupHandle_t group, EventBits_t bits)
        : group_(group)
        , bits_(bits)
        , shouldClear_(true) {
    }
    
    ~EventBitGuard() {
        if (shouldClear_ && group_) {
            xEventGroupClearBits(group_, bits_);
        }
    }
    
    void release() { shouldClear_ = false; }
    
private:
    EventGroupHandle_t group_;
    EventBits_t bits_;
    bool shouldClear_;
};

// Utility macros for common patterns
#define SCOPED_MUTEX(mutex) SafeMutexGuard _guard(mutex)
#define SCOPED_MUTEX_TIMEOUT(mutex, timeout) SafeMutexGuard _guard(mutex, timeout)

#define ON_SCOPE_EXIT(code) \
    ScopedCleanup _cleanup; \
    _cleanup.add([&]() { code; })

#endif // RESOURCE_GUARD_H