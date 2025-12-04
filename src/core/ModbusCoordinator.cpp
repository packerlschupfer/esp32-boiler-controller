// src/core/ModbusCoordinator.cpp
#include "ModbusCoordinator.h"
#include "utils/MutexRetryHelper.h"
#include <algorithm>

static const char* TAG = "ModbusCoord";

// Define static members
constexpr uint32_t ModbusCoordinator::ANDRTF3_TICKS[];
constexpr uint32_t ModbusCoordinator::MB8ART_TICKS[];

ModbusCoordinator::ModbusCoordinator() 
    : coordinatorTimer(nullptr)
    , mutex(nullptr)
    , currentTick(0)
    , running(false) {
    
    // Create mutex for thread safety
    mutex = xSemaphoreCreateMutex();
    if (!mutex) {
        LOG_ERROR(TAG, "Failed to create mutex");
    }
}

ModbusCoordinator::~ModbusCoordinator() {
    stop();
    if (mutex) {
        vSemaphoreDelete(mutex);
    }
}

ModbusCoordinator& ModbusCoordinator::getInstance() {
    static ModbusCoordinator instance;
    return instance;
}

bool ModbusCoordinator::registerSensor(SensorType type, TaskHandle_t taskHandle) {
    if (!mutex || !taskHandle) {
        return false;
    }

    auto guard = MutexRetryHelper::acquireGuard(mutex, "ModbusCoord-Register");
    if (guard) {
        registeredSensors[type] = taskHandle;

        const char* sensorName = (type == SensorType::MB8ART) ? "MB8ART" :
                                (type == SensorType::ANDRTF3) ? "ANDRTF3" : "Unknown";
        LOG_INFO(TAG, "Registered sensor: %s", sensorName);
        return true;
    }

    return false;
}

void ModbusCoordinator::unregisterSensor(SensorType type) {
    if (!mutex) {
        return;
    }

    auto guard = MutexRetryHelper::acquireGuard(mutex, "ModbusCoord-Unregister");
    if (guard) {
        registeredSensors.erase(type);

        const char* sensorName = (type == SensorType::MB8ART) ? "MB8ART" :
                                (type == SensorType::ANDRTF3) ? "ANDRTF3" : "Unknown";
        LOG_INFO(TAG, "Unregistered sensor: %s", sensorName);
    }
}

bool ModbusCoordinator::start() {
    if (running) {
        LOG_WARN(TAG, "Coordinator already running");
        return true;
    }
    
    // Create the coordination timer
    coordinatorTimer = xTimerCreate(
        "ModbusCoordTimer",
        pdMS_TO_TICKS(TICK_INTERVAL_MS),
        pdTRUE,  // Auto-reload
        this,    // Timer ID (pass this pointer)
        timerCallback
    );
    
    if (!coordinatorTimer) {
        LOG_ERROR(TAG, "Failed to create coordinator timer");
        return false;
    }
    
    // Start the timer
    if (xTimerStart(coordinatorTimer, pdMS_TO_TICKS(100)) != pdPASS) {
        LOG_ERROR(TAG, "Failed to start coordinator timer");
        xTimerDelete(coordinatorTimer, 0);
        coordinatorTimer = nullptr;
        return false;
    }
    
    running = true;
    currentTick = 0;
    LOG_INFO(TAG, "Modbus coordinator started - tick interval: %lums", TICK_INTERVAL_MS);
    
    return true;
}

void ModbusCoordinator::stop() {
    if (!running) {
        return;
    }
    
    if (coordinatorTimer) {
        xTimerStop(coordinatorTimer, pdMS_TO_TICKS(100));
        xTimerDelete(coordinatorTimer, pdMS_TO_TICKS(100));
        coordinatorTimer = nullptr;
    }
    
    running = false;
    LOG_INFO(TAG, "Modbus coordinator stopped");
}

void ModbusCoordinator::timerCallback(TimerHandle_t xTimer) {
    // Get the coordinator instance from timer ID
    ModbusCoordinator* coordinator = static_cast<ModbusCoordinator*>(pvTimerGetTimerID(xTimer));
    if (coordinator) {
        coordinator->processTick();
    }
}

void ModbusCoordinator::processTick() {
    // Check which sensor should read at this tick
    SensorType nextSensor = getNextSensor(currentTick);

    // Notify the appropriate task if registered
    // Use shorter timeout for tick processing to avoid blocking
    auto guard = MutexRetryHelper::acquireGuard(
        mutex,
        "ModbusCoord-Tick",
        pdMS_TO_TICKS(10)  // Short timeout for tick processing
    );
    if (guard) {
        auto it = registeredSensors.find(nextSensor);
        if (it != registeredSensors.end() && it->second != nullptr) {
            // Round 15 Issue #7: Verify task is still valid before notifying
            // This prevents notifying a crashed/deleted task whose handle is stale
            eTaskState taskState = eTaskGetState(it->second);
            if (taskState == eDeleted || taskState == eInvalid) {
                // Task no longer exists - remove from registry
                const char* sensorName = (nextSensor == SensorType::MB8ART) ? "MB8ART" :
                                        (nextSensor == SensorType::ANDRTF3) ? "ANDRTF3" : "Unknown";
                LOG_WARN(TAG, "Task for %s is no longer valid (state=%d) - unregistering",
                        sensorName, (int)taskState);
                registeredSensors.erase(nextSensor);
            } else {
                // Send notification to the task
                xTaskNotifyGive(it->second);

                const char* sensorName = (nextSensor == SensorType::MB8ART) ? "MB8ART" :
                                        (nextSensor == SensorType::ANDRTF3) ? "ANDRTF3" : "None";
                LOG_DEBUG(TAG, "Tick %lu: Notifying %s", currentTick, sensorName);
            }
        }
    }

    // Advance tick counter
    currentTick = (currentTick + 1) % TICKS_PER_CYCLE;
}

ModbusCoordinator::SensorType ModbusCoordinator::getNextSensor(uint32_t tick) {
    // Round 15 Issue #12: Linear search is intentional here
    // The ANDRTF3_TICKS and MB8ART_TICKS arrays are small (<10 elements each).
    // Linear search O(n) with small n is faster than hash table or binary search
    // due to cache locality and no overhead from complex data structures.
    // If arrays grow significantly, consider using std::unordered_set.

    // Check if this tick is for ANDRTF3
    for (uint32_t i = 0; i < sizeof(ANDRTF3_TICKS)/sizeof(ANDRTF3_TICKS[0]); i++) {
        if (ANDRTF3_TICKS[i] == tick) {
            return SensorType::ANDRTF3;
        }
    }

    // Check if this tick is for MB8ART
    for (uint32_t i = 0; i < sizeof(MB8ART_TICKS)/sizeof(MB8ART_TICKS[0]); i++) {
        if (MB8ART_TICKS[i] == tick) {
            return SensorType::MB8ART;
        }
    }

    // No sensor scheduled for this tick
    return static_cast<SensorType>(-1);
}