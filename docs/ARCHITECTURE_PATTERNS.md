# Architecture Patterns and Best Practices

This document describes the architectural patterns and best practices used in the ESPlan Boiler Controller project.

## Table of Contents
- [System Resource Provider (SRP) Pattern](#system-resource-provider-srp-pattern)
- [Module Extraction Pattern](#module-extraction-pattern)
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

## Module Extraction Pattern

When files grow too large (>800 lines), extract focused helper classes to improve maintainability and testability.

### When to Extract

Extract helper logic when:
1. File exceeds 800-1000 lines
2. Multiple distinct responsibilities in one file
3. Helper functions can be logically grouped
4. Testing individual components would benefit from isolation

### Extraction Patterns

#### Pattern 1: Namespace with Static Functions

Best for stateless utility functions that don't need shared state:

```cpp
// BurnerSafetyChecks.h
#pragma once

class BurnerSafetyChecks {
public:
    // Flame detection (proxy via relay state)
    static bool isFlameDetected();

    // Run safety validation
    static bool checkSafetyConditions();

    // Check if seamless mode switch is safe
    static bool canSeamlesslySwitch(BurnerSMState currentState);
};
```

**Usage:**
```cpp
// In BurnerStateMachine.cpp
#include "BurnerSafetyChecks.h"

if (!BurnerSafetyChecks::checkSafetyConditions()) {
    // Handle safety failure
}
```

**Benefits:**
- No instantiation needed (static functions)
- Clear separation of concerns
- Easy to test in isolation
- No shared state to manage

#### Pattern 2: Static Functions with Internal State

For helpers that need minimal state tracking:

```cpp
// BurnerRuntimeTracker.h
#pragma once
#include <atomic>

class BurnerRuntimeTracker {
public:
    // Record burner start time
    static void recordStartTime();

    // Update runtime counters (called on burner stop)
    static void updateRuntimeCounters();

    // Get start time for diagnostics
    static uint32_t getStartTime();

private:
    static std::atomic<uint32_t> burnerStartTime;
};
```

**Implementation:**
```cpp
// BurnerRuntimeTracker.cpp
std::atomic<uint32_t> BurnerRuntimeTracker::burnerStartTime{0};

void BurnerRuntimeTracker::recordStartTime() {
    burnerStartTime = millis();
}

void BurnerRuntimeTracker::updateRuntimeCounters() {
    uint32_t startTime = burnerStartTime.load();
    uint32_t elapsedMs = millis() - startTime;
    // Update FRAM counters...
}
```

**Benefits:**
- Encapsulated state management
- Thread-safe with atomics
- Simple interface
- No complex lifecycle management

#### Pattern 3: Delegation with Callback

For helpers that need to call back into the parent class:

```cpp
// RelayCommandProcessor.h
#pragma once
#include <freertos/event_groups.h>

class RelayCommandProcessor {
public:
    // Process relay requests with callback for actual relay control
    static void processRelayRequests(
        EventGroupHandle_t relayRequestEventGroup,
        bool (*setRelayStateFunc)(uint8_t relayIndex, bool state)
    );
};
```

**Usage:**
```cpp
// In RelayControlTask.cpp
void RelayControlTask::processRelayRequests() {
    auto& resourceManager = SharedResourceManager::getInstance();
    EventGroupHandle_t relayRequestEventGroup =
        resourceManager.getEventGroup(SharedResourceManager::EventGroups::RELAY_REQUEST);

    // Delegate with callback
    RelayCommandProcessor::processRelayRequests(
        relayRequestEventGroup,
        &RelayControlTask::setRelayState  // Callback function pointer
    );
}
```

**Benefits:**
- Separates event processing from control logic
- Parent retains control over actual operations
- Helper can be tested with mock callbacks
- Clean separation of concerns

### Extraction Guidelines

**What to Extract:**
- ✅ Safety check functions (validation logic)
- ✅ Power control decision logic
- ✅ Runtime tracking and counters
- ✅ Event processing loops
- ✅ Pump protection timing
- ✅ Health monitoring and escalation

**What NOT to Extract:**
- ❌ Core state machine logic (FSM should stay together)
- ❌ Task lifecycle functions (init, start, stop)
- ❌ Single-use helper functions (<20 lines)
- ❌ Functions tightly coupled to parent state

### Real-World Examples from Round 21 Refactoring

#### Example 1: BurnerStateMachine.cpp (1040 → 800 lines)

**Extracted:**
- `BurnerSafetyChecks` - 160 lines of safety validation
- `BurnerPowerController` - 32 lines of power decision logic
- `BurnerRuntimeTracker` - 48 lines of FRAM counter management

**Result:**
- 23% reduction in file size
- Each component testable in isolation
- Clearer separation of concerns
- Easier to locate and modify specific logic

#### Example 2: RelayControlTask.cpp (989 → 790 lines)

**Extracted:**
- `RelayVerificationManager` - Pump protection and health monitoring
- `RelayCommandProcessor` - Event processing with callback pattern

**Result:**
- 20% reduction in file size
- Pump protection logic isolated for testing
- Event processing separated from control logic

### Testing Extracted Modules

Each extracted module should have corresponding unit tests:

```cpp
// test/test_native/test_burner_safety_checks.cpp
#include <unity.h>
#include "modules/control/BurnerSafetyChecks.h"

void test_safety_conditions_pass() {
    // Mock sensor data
    // ...

    bool result = BurnerSafetyChecks::checkSafetyConditions();
    TEST_ASSERT_TRUE(result);
}

void test_80c_safety_limit() {
    // Set temperature to 81°C
    // ...

    bool canIncrease = BurnerSafetyChecks::shouldIncreasePower();
    TEST_ASSERT_FALSE(canIncrease);  // Should block high power
}
```

### Naming Conventions

Follow these naming patterns for extracted modules:

| Type | Pattern | Example |
|------|---------|---------|
| Safety logic | `*SafetyChecks` | `BurnerSafetyChecks` |
| Control logic | `*Controller` | `BurnerPowerController` |
| Tracking/monitoring | `*Tracker`, `*Monitor` | `BurnerRuntimeTracker`, `RelayHealthMonitor` |
| Processing | `*Processor`, `*Manager` | `RelayCommandProcessor`, `RelayVerificationManager` |

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

### Memory Management Patterns

See [MEMORY_OPTIMIZATION.md](MEMORY_OPTIMIZATION.md) for complete rationale and measurements.

#### Static Buffers (ESP32 Optimization)

Static buffers are used strategically to save scarce task stack space on ESP32:

```cpp
const char* getFailureReason() const {
    // THREAD-SAFETY: Static buffer safe - single-threaded access
    // See docs/MEMORY_OPTIMIZATION.md for complete rationale
    static char buffer[192];  // Uses abundant global RAM (282KB free)
    // ...
    return buffer;  // Safe: points to persistent static memory
}
```

**When to use static buffers:**
- Buffer is large (>64B) AND task stack is tight (<1KB free)
- Single-threaded access pattern (task-local function)
- Immediate use only (not stored across function calls)
- Function returns pointer to buffer (can't use stack-local)
- Library requires persistent storage (e.g., MQTTConfig pointers)

**When to use mutex-protected static:**
```cpp
void publishMetrics() {
    MutexGuard guard(mutex_);  // ✅ Mutex protects static buffer
    static char buffer[256];
    snprintf(buffer, sizeof(buffer), "{...}");
    publish(buffer);
}
```

**Pattern decision matrix:**

| Pattern | Use When | Example | RAM Cost |
|---------|----------|---------|----------|
| **Static single-thread** | Called from one task only | SafetyInterlocks::getFailureReason() | 192B global |
| **Static + mutex** | Multiple tasks, mutex exists | QueueManager::publishMetrics() | 256B global |
| **Rotating pool** | Short-lived temp strings | StringUtils::TempBuffer | 512B global |
| **Stack-local** | Small buffer (<32B), plenty of stack | Temp variables | 0B global |

**Anti-patterns (DO NOT USE):**
- ❌ `thread_local` - Wastes RAM (buffer × number of tasks = 192B × 18 = 3.5KB)
- ❌ Large stack buffers on tight tasks - Risks stack overflow (some tasks have <500B free)
- ❌ Globals without thread-safety docs - Unclear safety assumptions

**Measured constraints (DEBUG_SELECTIVE mode):**
```
Task stack free space:
- ModbusControl: 448B free   → 192B stack buffer would be DANGEROUS
- MQTT: 712B free            → 448B total buffers would be FATAL
- Heating: 800B free         → 192B risky
- BurnerControl: 1200B free  → 192B acceptable but wasteful

Global RAM available: 282KB → Static buffers use 1.2KB total (0.4%)
```

**Conclusion**: Static buffers are the CORRECT design for ESP32. See [MEMORY_OPTIMIZATION.md](MEMORY_OPTIMIZATION.md) for complete analysis.

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