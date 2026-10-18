// src/core/ModbusCoordinator.h
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <freertos/semphr.h>
#include <atomic>
#include <map>
#include <string>
#include "config/SystemConstants.h"
#include "LoggingMacros.h"

/**
 * @brief Coordinates Modbus operations to prevent bus contention
 *
 * This class manages timing for multiple Modbus devices to ensure they
 * don't try to access the bus simultaneously. It uses a single timer
 * and notifies registered tasks when their turn comes.
 *
 * Tick Schedule (500ms per tick, 10 ticks = 5s cycle):
 *   Tick 0: ANDRTF3 (room temp)
 *   Tick 1: RYN4_SET (batch write relay changes)
 *   Tick 2: MB8ART (boiler temps)
 *   Tick 3: RYN4_READ (verify relay states)
 *   Tick 4: (idle)
 *   Tick 5: MB8ART (boiler temps)
 *   Tick 6: RYN4_SET (batch write relay changes)
 *   Tick 7: (idle)
 *   Tick 8: RYN4_READ (verify relay states)
 *   Tick 9: (idle)
 */
class ModbusCoordinator {
public:
    // Device/operation types that can be registered
    enum class SensorType {
        NONE = -1,      // No device scheduled
        MB8ART = 0,     // Temperature sensor read
        ANDRTF3 = 1,    // Room temperature sensor read
        RYN4_SET = 2,   // Relay batch write
        RYN4_READ = 3   // Relay state verification
    };
    
    // Singleton instance
    static ModbusCoordinator& getInstance();

    // Deliberately NO resetForTesting(). Resetting this singleton is genuinely unsafe:
    // it owns a FreeRTOS timer that may be mid-callback, registered tasks may be
    // waiting on notifications, the mutex may be held by another task, and tick state
    // drives time-critical Modbus scheduling.
    //
    // Removed 2026-09-16. It used to exist as an EMPTY body under #ifdef UNIT_TEST,
    // which is worse than having no function at all: a caller compiles cleanly,
    // believes the singleton was reset, and gets a silent no-op with no runtime signal.
    //
    // Use mocks instead - mock the coordinator interface, test components
    // independently, and do not test real Modbus timing in unit tests. For integration
    // tests that need real coordination, restart the test process.

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

    // Tick schedule (which device operates at which tick)
    static constexpr uint32_t ANDRTF3_TICKS[] = {0};       // Room temp every 5s
    static constexpr uint32_t RYN4_SET_TICKS[] = {1, 6};   // Relay write every 2.5s
    static constexpr uint32_t MB8ART_TICKS[] = {2, 5};     // Boiler temps every 2.5s
    static constexpr uint32_t RYN4_READ_TICKS[] = {3, 8};  // Relay verify every 2.5s
};