// src/core/ModbusCoordinator.cpp
#include "ModbusCoordinator.h"
#include "utils/MutexRetryHelper.h"
#include <algorithm>

static const char* TAG = "ModbusCoordinator";

// Define static members
constexpr uint32_t ModbusCoordinator::ANDRTF3_TICKS[];
constexpr uint32_t ModbusCoordinator::RYN4_SET_TICKS[];
constexpr uint32_t ModbusCoordinator::MB8ART_TICKS[];
constexpr uint32_t ModbusCoordinator::RYN4_READ_TICKS[];

// Helper to get sensor name for logging
static const char* getSensorName(ModbusCoordinator::SensorType type) {
    switch (type) {
        case ModbusCoordinator::SensorType::MB8ART:    return "MB8ART";
        case ModbusCoordinator::SensorType::ANDRTF3:   return "ANDRTF3";
        case ModbusCoordinator::SensorType::RYN4_SET:  return "RYN4_SET";
        case ModbusCoordinator::SensorType::RYN4_READ: return "RYN4_READ";
        default: return "Unknown";
    }
}

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
        LOG_INFO(TAG, "Registered: %s", getSensorName(type));
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
        LOG_INFO(TAG, "Unregistered: %s", getSensorName(type));
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
    // Check which device should operate at this tick
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
            // Verify task is still valid before notifying
            eTaskState taskState = eTaskGetState(it->second);
            if (taskState == eDeleted || taskState == eInvalid) {
                // Task no longer exists - remove from registry
                LOG_WARN(TAG, "Task for %s is no longer valid (state=%d) - unregistering",
                        getSensorName(nextSensor), (int)taskState);
                registeredSensors.erase(nextSensor);
            } else {
                // Send notification to the task with SensorType as value
                // This allows RYN4ProcessingTask to distinguish between SET and READ operations
                xTaskNotify(it->second, static_cast<uint32_t>(nextSensor), eSetValueWithOverwrite);
                LOG_DEBUG(TAG, "Tick %lu: %s", currentTick, getSensorName(nextSensor));
            }
        }
    }

    // Advance tick counter
    currentTick = (currentTick + 1) % TICKS_PER_CYCLE;
}

ModbusCoordinator::SensorType ModbusCoordinator::getNextSensor(uint32_t tick) {
    // Linear search is intentional - arrays are small (<4 elements each)
    // and cache locality makes this faster than hash lookups.

    // Check if this tick is for ANDRTF3 (room temp)
    for (uint32_t i = 0; i < sizeof(ANDRTF3_TICKS)/sizeof(ANDRTF3_TICKS[0]); i++) {
        if (ANDRTF3_TICKS[i] == tick) {
            return SensorType::ANDRTF3;
        }
    }

    // Check if this tick is for RYN4 SET (relay write)
    for (uint32_t i = 0; i < sizeof(RYN4_SET_TICKS)/sizeof(RYN4_SET_TICKS[0]); i++) {
        if (RYN4_SET_TICKS[i] == tick) {
            return SensorType::RYN4_SET;
        }
    }

    // Check if this tick is for MB8ART (boiler temps)
    for (uint32_t i = 0; i < sizeof(MB8ART_TICKS)/sizeof(MB8ART_TICKS[0]); i++) {
        if (MB8ART_TICKS[i] == tick) {
            return SensorType::MB8ART;
        }
    }

    // Check if this tick is for RYN4 READ (relay verify)
    for (uint32_t i = 0; i < sizeof(RYN4_READ_TICKS)/sizeof(RYN4_READ_TICKS[0]); i++) {
        if (RYN4_READ_TICKS[i] == tick) {
            return SensorType::RYN4_READ;
        }
    }

    // No device scheduled for this tick
    return SensorType::NONE;
}