// src/core/ModbusCoordinator.h
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <freertos/semphr.h>
#include <map>
#include <string>
#include "config/SystemConstants.h"
#include "LoggingMacros.h"

/**
 * @brief Coordinates Modbus sensor reads to prevent bus contention
 * 
 * This class manages timing for multiple Modbus devices to ensure they
 * don't try to access the bus simultaneously. It uses a single timer
 * and notifies registered tasks when their turn comes.
 */
class ModbusCoordinator {
public:
    // Sensor types that can be registered
    enum class SensorType {
        MB8ART,
        ANDRTF3,
        RYN4  // Future expansion
    };
    
    // Singleton instance
    static ModbusCoordinator& getInstance();
    
    // Register a task to receive notifications
    bool registerSensor(SensorType type, TaskHandle_t taskHandle);
    
    // Unregister a task
    void unregisterSensor(SensorType type);
    
    // Start the coordination timer
    bool start();
    
    // Stop the coordination timer
    void stop();
    
    // Check if coordinator is running
    bool isRunning() const { return running; }
    
private:
    // Private constructor for singleton
    ModbusCoordinator();
    ~ModbusCoordinator();
    
    // Prevent copying
    ModbusCoordinator(const ModbusCoordinator&) = delete;
    ModbusCoordinator& operator=(const ModbusCoordinator&) = delete;
    
    // Timer callback
    static void timerCallback(TimerHandle_t xTimer);
    
    // Process tick and notify appropriate tasks
    void processTick();
    
    // Calculate next sensor to read based on tick
    SensorType getNextSensor(uint32_t tick);
    
    // Member variables
    TimerHandle_t coordinatorTimer;
    std::map<SensorType, TaskHandle_t> registeredSensors;
    SemaphoreHandle_t mutex;
    uint32_t currentTick;
    bool running;
    
    // Timing configuration
    static constexpr uint32_t TICK_INTERVAL_MS = 500;  // 500ms per tick
    static constexpr uint32_t TICKS_PER_CYCLE = 10;    // 10 ticks = 5 seconds
    
    // Tick schedule (which sensor reads at which tick)
    // ANDRTF3: tick 0 (every 5s)
    // MB8ART: ticks 2, 5 (every 2.5s)
    static constexpr uint32_t ANDRTF3_TICKS[] = {0};
    static constexpr uint32_t MB8ART_TICKS[] = {2, 5};
};