# Architecture Patterns and Best Practices

This document describes the architectural patterns and best practices used in the ESPlan Boiler Controller project.

## Table of Contents
- [System Resource Provider (SRP) Pattern](#system-resource-provider-srp-pattern)
- [Thread Safety Guidelines](#thread-safety-guidelines)
- [Event-Driven Communication](#event-driven-communication)
- [Error Handling with Result<T>](#error-handling-with-resultt)
- [Task Design Patterns](#task-design-patterns)

## System Resource Provider (SRP) Pattern

The System Resource Provider (SRP) is a centralized access point for all shared resources, providing a consistent interface and reducing coupling between components.

### Usage

```cpp
// Instead of direct global access:
// extern EventGroupHandle_t xSystemStateEventGroup;
// xEventGroupSetBits(xSystemStateEventGroup, BIT);

// Use SRP:
SRP::setSystemStateEventBits(BIT);
```

### Service Access

```cpp
// Get services through SRP
auto mqttManager = SRP::getMQTTManager();
auto ethManager = SRP::getEthernetManager();
auto mb8art = SRP::getMB8ART();
```

### Benefits
- Single point of access for all shared resources
- Easier to mock for testing
- Reduced coupling between components
- Consistent error handling

## Thread Safety Guidelines

### Mutex Usage

All shared data structures must be protected by mutexes:

```cpp
// Taking mutex with timeout
if (SRP::takeSensorReadingsMutex(TaskTimeouts::MUTEX_WAIT)) {
    // Access shared data
    auto& readings = SRP::getSensorReadings();
    readings.boilerTempOutput = temperature;
    
    // Always give mutex back
    SRP::giveSensorReadingsMutex();
} else {
    LOG_ERROR(TAG, "Failed to acquire mutex");
}
```

### Event Groups

Event groups are inherently thread-safe but follow these patterns:

```cpp
// Use batch operations from EventUtils
EventUtils::clearAllSensorUpdateBits(eventGroup);

// Wait with timeout
EventBits_t bits = EventUtils::waitForAnySensorUpdate(
    eventGroup, 
    TaskTimeouts::SENSOR_WAIT
);
```

### MQTTManager Thread Safety

The MQTTManager is designed to be thread-safe:
- All public methods are protected by internal mutex
- `isConnected()` uses atomic event group operations
- Publishing and subscribing are safe from any task

## Event-Driven Communication

### Event Group Organization

Events are organized by functional area:
- System events (GeneralSystemEventGroup)
- System state (SystemStateEventGroup)
- Sensor updates (SensorEventGroup)
- Relay updates (RelayEventGroup)
- Control requests (ControlRequestsEventGroup)

### Event Patterns

```cpp
// Producer pattern
void onSensorUpdate() {
    // Update shared data
    if (SRP::takeSensorReadingsMutex(TaskTimeouts::MUTEX_WAIT)) {
        SRP::getSensorReadings().temperature = newValue;
        SRP::giveSensorReadingsMutex();
    }
    
    // Signal update
    SRP::setSensorEventBits(TEMPERATURE_UPDATE_BIT);
}

// Consumer pattern
void waitForSensorData() {
    EventBits_t bits = SRP::waitSensorEventBits(
        TEMPERATURE_UPDATE_BIT,
        pdTRUE,  // Clear on exit
        pdFALSE, // Wait for any bit
        TaskTimeouts::SENSOR_WAIT
    );
    
    if (bits & TEMPERATURE_UPDATE_BIT) {
        processSensorData();
    }
}
```

## Error Handling with Result<T>

All operations that can fail should return `Result<T>`:

```cpp
Result<float> readTemperature() {
    if (!sensorReady) {
        return Result<float>(SystemError::SENSOR_NOT_READY);
    }
    
    float temp = sensor.read();
    if (isnan(temp)) {
        return Result<float>(SystemError::SENSOR_READ_FAILED);
    }
    
    return Result<float>(temp);
}

// Usage
auto result = readTemperature();
if (result.isError()) {
    ErrorHandler::handleError(result.error());
} else {
    float temp = result.value();
}
```

## Task Design Patterns

### Base Task Pattern

All tasks should inherit from BaseTask:

```cpp
class MyTask : public BaseTask {
public:
    MyTask() : BaseTask("MyTask", 4096, 5, 1) {}
    
protected:
    void taskFunction() override {
        // Initialize
        initialize();
        
        // Main loop
        while (!shouldStop()) {
            // Feed watchdog
            SRP::getTaskManager().feedWatchdog();
            
            // Do work
            processData();
            
            // Wait for next cycle
            vTaskDelay(TaskTimeouts::EVENT_WAIT);
        }
    }
};
```

### Singleton Task Pattern

For tasks that should have only one instance:

```cpp
class MQTTTask : public BaseTask {
public:
    static bool init();
    static bool start();
    static void stop();
    static bool isRunning();
    
private:
    static MQTTTask* instance_;
    static SemaphoreHandle_t mutex_;
    
    MQTTTask();  // Private constructor
};
```

### Connection Retry with Backoff

```cpp
void handleReconnection() {
    if (millis() - lastAttempt < currentDelay) {
        return;
    }
    
    lastAttempt = millis();
    attempts++;
    
    if (connect()) {
        // Reset on success
        currentDelay = MIN_DELAY;
        attempts = 0;
    } else {
        // Exponential backoff
        currentDelay = std::min(currentDelay * 2, MAX_DELAY);
    }
}
```

## Common Timeout Constants

Use standardized timeouts from TaskTimeouts namespace:

```cpp
namespace TaskTimeouts {
    constexpr TickType_t MUTEX_WAIT = pdMS_TO_TICKS(100);
    constexpr TickType_t EVENT_WAIT = pdMS_TO_TICKS(1000);
    constexpr TickType_t SENSOR_WAIT = pdMS_TO_TICKS(2000);
    constexpr TickType_t NETWORK_WAIT = pdMS_TO_TICKS(5000);
    constexpr TickType_t MODBUS_WAIT = pdMS_TO_TICKS(500);
    constexpr TickType_t MQTT_WAIT = pdMS_TO_TICKS(10000);
}
```

## Best Practices Summary

1. **Always use SRP** for accessing shared resources
2. **Protect shared data** with mutexes and proper timeout handling
3. **Use event groups** for inter-task communication
4. **Return Result<T>** for operations that can fail
5. **Implement exponential backoff** for network operations
6. **Use standardized timeouts** for consistency
7. **Follow singleton pattern** for system-wide services
8. **Batch event operations** when possible
9. **Feed watchdog regularly** in long-running operations
10. **Log errors appropriately** using ErrorHandler