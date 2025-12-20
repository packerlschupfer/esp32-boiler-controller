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

    SharedResourceManager() {
        accessMutex_ = xSemaphoreCreateMutex();
        if (!accessMutex_) {
            ErrorHandler::logError("SharedResourceManager", SystemError::MEMORY_ALLOCATION_FAILED,
                                 "Failed to create access mutex");
        }
    }

public:
    static SharedResourceManager& getInstance() {
        // Thread-safe initialization using C++11 static local variable
        // The standard guarantees this is initialized exactly once
        static SharedResourceManager instance;
        return instance;
    }

    #ifdef UNIT_TEST
    /**
     * @brief Reset singleton for testing - NOT IMPLEMENTED
     *
     * DESIGN NOTE: SharedResourceManager manages FreeRTOS primitives (event groups,
     * mutexes, queues) that are used throughout the system. Deleting and recreating
     * these resources would be extremely dangerous:
     *
     * 1. Other components may hold handles to these resources
     * 2. Tasks may be blocked on mutexes/event groups
     * 3. Queues may contain data
     * 4. Race conditions during deletion could cause crashes
     *
     * RECOMMENDED APPROACH: Use dependency injection and mocks in tests instead:
     *   - Tests should create mock event groups/mutexes
     *   - Don't call production SharedResourceManager in unit tests
     *   - Use test doubles for components that depend on these resources
     *
     * If full system reset is needed (integration tests), restart the test process.
     */
    static void resetForTesting() {
        // Intentionally not implemented - see comment above
        // Use mocks/dependency injection in tests instead
    }
    #endif

    // Delete copy constructor and assignment
    SharedResourceManager(const SharedResourceManager&) = delete;
    SharedResourceManager& operator=(const SharedResourceManager&) = delete;

    /**
     * @brief Create or get an event group
     * @param name Event group name
     * @return EventGroupHandle_t or nullptr on failure
     */
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