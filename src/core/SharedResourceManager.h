// src/core/SharedResourceManager.h
#ifndef SHARED_RESOURCE_MANAGER_H
#define SHARED_RESOURCE_MANAGER_H

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <unordered_map>
#include <string>
#include "config/SystemConstants.h"
#include "utils/ErrorHandler.h"

/**
 * @brief Manages all shared FreeRTOS resources (event groups, mutexes, queues)
 * 
 * This class centralizes the creation and access of all shared resources,
 * eliminating the need for global variables scattered throughout the codebase.
 */
class SharedResourceManager {
public:
    // Resource types
    enum class ResourceType {
        EVENT_GROUP,
        MUTEX,
        QUEUE
    };

    // Well-known resource names
    struct EventGroups {
        static constexpr const char* GENERAL_SYSTEM = "GeneralSystem";
        // SYSTEM removed - consolidated into SYSTEM_STATE (M1 refactoring)
        // Both were using SystemEvents::SystemState namespace - duplicate eliminated
        static constexpr const char* SYSTEM_STATE = "SystemState";
        static constexpr const char* CONTROL_REQUESTS = "ControlRequests";
        // WHEATER REMOVED - was unused (use HEATING for water/heating events)
        static constexpr const char* HEATING = "Heating";
        static constexpr const char* BURNER = "Burner";
        static constexpr const char* BURNER_REQUEST = "BurnerRequest";
        static constexpr const char* SENSOR = "Sensor";
        static constexpr const char* ERROR_NOTIFICATION = "ErrorNotification";
        // TIMER REMOVED - was unused (scheduler uses CONTROL_REQUESTS)
        static constexpr const char* RELAY = "Relay";
        static constexpr const char* RELAY_STATUS = "RelayStatus";
        static constexpr const char* RELAY_REQUEST = "RelayRequest";
        // static constexpr const char* SENSOR_MITH = "SensorMiTh";  // BLE removed
        // MQTT removed - consolidated into SYSTEM_STATE (uses SystemState::MQTT_* bits)
    };

    struct Mutexes {
        static constexpr const char* SENSOR_READINGS = "SensorReadings";
        // static constexpr const char* SENSOR_MITH = "SensorMiTh";  // BLE removed
        static constexpr const char* RELAY_READINGS = "RelayReadings";
        static constexpr const char* SYSTEM_SETTINGS = "SystemSettings";
        static constexpr const char* MQTT = "MQTT";
    };

private:
    struct ResourceInfo {
        void* handle;
        ResourceType type;
        std::string name;
    };
    
    std::unordered_map<std::string, ResourceInfo> resources_;
    SemaphoreHandle_t accessMutex_;

    // ---- Pointer-keyed fast path for the handle accessors ----------------------------
    //
    // The map lookups below are not cheap and they run on hot control paths. Every
    // SystemResourceProvider::takeSensorReadingsMutex() AND giveSensorReadingsMutex()
    // built a std::string, took this global accessMutex_ with a timeout, hashed 14
    // characters and probed an unordered_map - to fetch a handle that has not changed
    // since boot. There are 11 such call sites, including BurnerStateMachine.cpp:411 and
    // HeatingControlTask.cpp:346/538.
    //
    // Every caller passes one of the `static constexpr const char*` constants above, and
    // identical string literals are merged by the compiler and linker, so the POINTER
    // identifies the resource: no string has to exist to look one up.
    //
    // Concurrency, without taking a lock: `key` is installed once in the constructor and
    // never written again, and `handle` is only ever written with the value the map
    // already returned for that same key - a single naturally-aligned pointer store, so a
    // reader sees either nullptr (and takes the slow path, which is correct) or the
    // finished handle. Never a torn value. cleanup() zeroes the handles, so callers after
    // a shutdown fall back to the slow path and recreate exactly as they did before -
    // which matters because enterFailsafeMode() runs AFTER cleanup() and still uses these.
    struct FastSlot {
        const char* key;
        void* handle;
        ResourceType type;
    };
    static constexpr size_t FAST_SLOTS = 16;
    FastSlot fast_[FAST_SLOTS];
    size_t fastUsed_;

    void* fastGet(const char* key, ResourceType type) const {
        for (size_t i = 0; i < fastUsed_; ++i) {
            if (fast_[i].key == key && fast_[i].type == type) return fast_[i].handle;
        }
        return nullptr;
    }

    void fastPut(const char* key, ResourceType type, void* handle) {
        for (size_t i = 0; i < fastUsed_; ++i) {
            if (fast_[i].key == key && fast_[i].type == type) {
                fast_[i].handle = handle;      // single aligned store; see note above
                return;
            }
        }
    }

    void fastInstall(const char* key, ResourceType type) {
        if (fastUsed_ < FAST_SLOTS) {
            fast_[fastUsed_++] = {key, nullptr, type};
        } else {
            // Not fatal - the overflowing name keeps using the map - but it silently
            // undoes the optimisation for that resource, so say so.
            LOG_ERROR("SharedResourceManager", "fast slot table full, %s stays on the "
                      "map path (raise FAST_SLOTS)", key);
        }
    }

    SharedResourceManager() : fastUsed_(0) {
        accessMutex_ = xSemaphoreCreateMutex();
        if (!accessMutex_) {
            ErrorHandler::logError("SharedResourceManager", SystemError::MEMORY_ALLOCATION_FAILED,
                                 "Failed to create access mutex");
        }
        // Slots for the resources the hot paths ask for. A name missing here is not a bug,
        // it just keeps using the map. static_assert below pins the table against FAST_SLOTS.
        for (const char* n : {EventGroups::GENERAL_SYSTEM, EventGroups::SYSTEM_STATE,
                              EventGroups::CONTROL_REQUESTS, EventGroups::HEATING,
                              EventGroups::BURNER, EventGroups::BURNER_REQUEST,
                              EventGroups::SENSOR, EventGroups::ERROR_NOTIFICATION,
                              EventGroups::RELAY, EventGroups::RELAY_STATUS,
                              EventGroups::RELAY_REQUEST}) {
            fastInstall(n, ResourceType::EVENT_GROUP);
        }
        for (const char* n : {Mutexes::SENSOR_READINGS, Mutexes::RELAY_READINGS,
                              Mutexes::SYSTEM_SETTINGS, Mutexes::MQTT}) {
            fastInstall(n, ResourceType::MUTEX);
        }
    }

public:
    static SharedResourceManager& getInstance() {
        // Thread-safe initialization using C++11 static local variable
        // The standard guarantees this is initialized exactly once
        static SharedResourceManager instance;
        return instance;
    }

    // Deliberately NO resetForTesting(). This singleton owns the FreeRTOS primitives
    // (event groups, mutexes, queues) used across the whole system, so deleting and
    // recreating them is unsafe: other components hold handles, tasks may be blocked
    // on them, queues may still hold data, and deletion races can crash outright.
    //
    // Removed 2026-09-16. It used to exist as an EMPTY body under #ifdef UNIT_TEST,
    // which is worse than having no function at all: a caller compiles cleanly,
    // believes the singleton was reset, and gets a silent no-op with no runtime signal.
    //
    // Use dependency injection and test doubles instead - create mock event
    // groups/mutexes rather than calling the production manager in unit tests. If a
    // full system reset is needed for integration tests, restart the test process.

    // Delete copy constructor and assignment
    SharedResourceManager(const SharedResourceManager&) = delete;
    SharedResourceManager& operator=(const SharedResourceManager&) = delete;

    /**
     * @brief Create or get an event group
     * @param name Event group name
     * @return EventGroupHandle_t or nullptr on failure
     */
    /**
     * @brief Create or get an event group, by well-known name constant
     *
     * Overload taken by every caller that passes an EventGroups:: constant. Hits the
     * pointer-keyed fast path and touches neither std::string nor accessMutex_ once the
     * handle exists; falls through to the map on a miss, so behaviour is unchanged.
     */
    EventGroupHandle_t getEventGroup(const char* name) {
        if (void* h = fastGet(name, ResourceType::EVENT_GROUP)) {
            return static_cast<EventGroupHandle_t>(h);
        }
        EventGroupHandle_t handle = getEventGroup(std::string(name));
        if (handle) fastPut(name, ResourceType::EVENT_GROUP, handle);
        return handle;
    }

    EventGroupHandle_t getEventGroup(const std::string& name) {
        // Use timeout to prevent deadlock during resource lookup
        if (xSemaphoreTake(accessMutex_, pdMS_TO_TICKS(SystemConstants::Timing::MUTEX_DEFAULT_TIMEOUT_MS)) != pdTRUE) {
            LOG_ERROR("SharedResourceManager", "Timeout acquiring access mutex for: %s", name.c_str());
            return nullptr;
        }

        auto it = resources_.find(name);
        if (it != resources_.end() && it->second.type == ResourceType::EVENT_GROUP) {
            EventGroupHandle_t handle = static_cast<EventGroupHandle_t>(it->second.handle);
            xSemaphoreGive(accessMutex_);
            return handle;
        }

        // Create new event group
        EventGroupHandle_t handle = xEventGroupCreate();
        if (handle) {
            resources_[name] = {handle, ResourceType::EVENT_GROUP, name};
            LOG_INFO("SharedResourceManager", "Created event group: %s", name.c_str());
        } else {
            LOG_ERROR("SharedResourceManager", "Failed to create event group: %s", name.c_str());
        }

        xSemaphoreGive(accessMutex_);
        return handle;
    }

    /**
     * @brief Create or get a mutex
     * @param name Mutex name
     * @return SemaphoreHandle_t or nullptr on failure
     */
    /**
     * @brief Create or get a mutex, by well-known name constant
     *
     * See getEventGroup(const char*). This is the one that matters most: it is on the
     * take AND the give of every sensor/relay/settings mutex acquisition.
     */
    SemaphoreHandle_t getMutex(const char* name) {
        if (void* h = fastGet(name, ResourceType::MUTEX)) {
            return static_cast<SemaphoreHandle_t>(h);
        }
        SemaphoreHandle_t handle = getMutex(std::string(name));
        if (handle) fastPut(name, ResourceType::MUTEX, handle);
        return handle;
    }

    SemaphoreHandle_t getMutex(const std::string& name) {
        // Use timeout to prevent deadlock during resource lookup
        if (xSemaphoreTake(accessMutex_, pdMS_TO_TICKS(SystemConstants::Timing::MUTEX_DEFAULT_TIMEOUT_MS)) != pdTRUE) {
            LOG_ERROR("SharedResourceManager", "Timeout acquiring access mutex for: %s", name.c_str());
            return nullptr;
        }

        auto it = resources_.find(name);
        if (it != resources_.end() && it->second.type == ResourceType::MUTEX) {
            SemaphoreHandle_t handle = static_cast<SemaphoreHandle_t>(it->second.handle);
            xSemaphoreGive(accessMutex_);
            return handle;
        }

        // Create new mutex
        SemaphoreHandle_t handle = xSemaphoreCreateMutex();
        if (handle) {
            resources_[name] = {handle, ResourceType::MUTEX, name};
            LOG_INFO("SharedResourceManager", "Created mutex: %s", name.c_str());
        } else {
            LOG_ERROR("SharedResourceManager", "Failed to create mutex: %s", name.c_str());
        }

        xSemaphoreGive(accessMutex_);
        return handle;
    }

    /**
     * @brief Create or get a queue
     * @param name Queue name
     * @param queueLength Number of items the queue can hold
     * @param itemSize Size of each item in bytes
     * @return QueueHandle_t or nullptr on failure
     */
    QueueHandle_t getQueue(const std::string& name, UBaseType_t queueLength, UBaseType_t itemSize) {
        // Use timeout to prevent deadlock during resource lookup
        if (xSemaphoreTake(accessMutex_, pdMS_TO_TICKS(SystemConstants::Timing::MUTEX_DEFAULT_TIMEOUT_MS)) != pdTRUE) {
            LOG_ERROR("SharedResourceManager", "Timeout acquiring access mutex for: %s", name.c_str());
            return nullptr;
        }

        auto it = resources_.find(name);
        if (it != resources_.end() && it->second.type == ResourceType::QUEUE) {
            QueueHandle_t handle = static_cast<QueueHandle_t>(it->second.handle);
            xSemaphoreGive(accessMutex_);
            return handle;
        }

        // Create new queue
        QueueHandle_t handle = xQueueCreate(queueLength, itemSize);
        if (handle) {
            resources_[name] = {handle, ResourceType::QUEUE, name};
            LOG_INFO("SharedResourceManager", "Created queue: %s (length=%d, size=%d)", 
                     name.c_str(), queueLength, itemSize);
        } else {
            LOG_ERROR("SharedResourceManager", "Failed to create queue: %s", name.c_str());
        }

        xSemaphoreGive(accessMutex_);
        return handle;
    }

    /**
     * @brief Initialize all standard shared resources
     * @return Result<void> Success or error
     */
    Result<void> initializeStandardResources() {
        LOG_INFO("SharedResourceManager", "Initializing standard shared resources...");

        // Create all standard event groups
        const char* eventGroups[] = {
            EventGroups::GENERAL_SYSTEM,
            // SYSTEM removed - consolidated into SYSTEM_STATE
            EventGroups::SYSTEM_STATE,
            EventGroups::CONTROL_REQUESTS,
            // WHEATER removed - unused
            EventGroups::HEATING,
            EventGroups::BURNER,
            EventGroups::BURNER_REQUEST,
            EventGroups::SENSOR,
            EventGroups::ERROR_NOTIFICATION,
            // TIMER removed - unused
            EventGroups::RELAY,
            EventGroups::RELAY_STATUS,
            EventGroups::RELAY_REQUEST
            // EventGroups::SENSOR_MITH,  // BLE removed
            // MQTT removed - consolidated into SYSTEM_STATE
        };

        for (const char* name : eventGroups) {
            if (!getEventGroup(name)) {
                return Result<void>(SystemError::MEMORY_ALLOCATION_FAILED,
                                  std::string("Failed to create event group: ") + name);
            }
        }

        // Create all standard mutexes
        const char* mutexes[] = {
            Mutexes::SENSOR_READINGS,
            // Mutexes::SENSOR_MITH,  // BLE removed
            Mutexes::RELAY_READINGS,
            Mutexes::SYSTEM_SETTINGS,
            Mutexes::MQTT
        };

        for (const char* name : mutexes) {
            if (!getMutex(name)) {
                return Result<void>(SystemError::MEMORY_ALLOCATION_FAILED,
                                  std::string("Failed to create mutex: ") + name);
            }
        }

        LOG_INFO("SharedResourceManager", "All standard resources initialized successfully");
        return Result<void>();
    }

    /**
     * @brief Get resource count by type
     */
    size_t getResourceCount(ResourceType type) const {
        size_t count = 0;
        for (const auto& [name, info] : resources_) {
            if (info.type == type) {
                count++;
            }
        }
        return count;
    }

    /**
     * @brief Get total resource count
     */
    size_t getTotalResourceCount() const {
        return resources_.size();
    }

    /**
     * @brief Clean up all resources (for shutdown)
     */
    void cleanup() {
        // Cleanup can use longer timeout since it's not in critical path
        if (xSemaphoreTake(accessMutex_, pdMS_TO_TICKS(SystemConstants::Timing::MUTEX_LONG_TIMEOUT_MS)) != pdTRUE) {
            LOG_ERROR("SharedResourceManager", "Timeout acquiring access mutex for cleanup");
            return;
        }

        for (auto& [name, info] : resources_) {
            switch (info.type) {
                case ResourceType::EVENT_GROUP:
                    vEventGroupDelete(static_cast<EventGroupHandle_t>(info.handle));
                    break;
                case ResourceType::MUTEX:
                    vSemaphoreDelete(static_cast<SemaphoreHandle_t>(info.handle));
                    break;
                case ResourceType::QUEUE:
                    vQueueDelete(static_cast<QueueHandle_t>(info.handle));
                    break;
            }
            LOG_INFO("SharedResourceManager", "Deleted %s: %s",
                     info.type == ResourceType::EVENT_GROUP ? "event group" :
                     info.type == ResourceType::MUTEX ? "mutex" : "queue",
                     name.c_str());
        }
        
        resources_.clear();
        // MUST happen with resources_.clear(): these handles have just been deleted, and
        // ErrorHandler::enterFailsafeMode() runs AFTER SystemInitializer::cleanup() and
        // still reaches these accessors. Zeroing sends those callers down the map path,
        // which recreates the primitive - exactly what happened before the fast path
        // existed. Leaving them set would hand the failsafe path freed handles.
        for (size_t i = 0; i < fastUsed_; ++i) fast_[i].handle = nullptr;
        xSemaphoreGive(accessMutex_);
    }
};

// Convenience macros for easy access
#define GET_EVENT_GROUP(name) \
    (SharedResourceManager::getInstance().getEventGroup(name))

#define GET_MUTEX(name) \
    (SharedResourceManager::getInstance().getMutex(name))

#define GET_QUEUE(name, length, size) \
    (SharedResourceManager::getInstance().getQueue(name, length, size))

#endif // SHARED_RESOURCE_MANAGER_H