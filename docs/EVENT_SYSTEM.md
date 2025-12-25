# Event System Architecture

## Overview

The boiler controller uses a zero-overhead event system based on FreeRTOS event groups. Event bits are defined as `constexpr` constants in organized namespaces, providing type safety and self-documentation with no runtime cost.

**Key File**: `include/events/SystemEventsGenerated.h` (auto-generated from script)

## Event Groups

The system uses 5 distinct event groups for different subsystems:

### 1. SystemState Event Group
**Handle**: `xSystemStateEventGroup` (via `SRP::getSystemStateEventGroup()`)
**Purpose**: Overall system operational state

| Bit | Name | Description |
|-----|------|-------------|
| 0 | BOILER_ENABLED | Boiler system enabled |
| 1 | BOILER_ON | Boiler currently firing |
| 2 | HEATING_ENABLED | Space heating enabled |
| 3 | HEATING_ON | Space heating active |
| 4 | WATER_ENABLED | Water heating enabled |
| 5 | WATER_ON | Water heating active |
| 6 | WATER_PRIORITY | Water has priority over heating |
| 7 | BURNER_ON | Burner is running |
| 8 | HEATING_PUMP_ON | Heating pump running |
| 9 | WATER_PUMP_ON | Water pump running |
| 12 | MQTT_ENABLED | MQTT communication enabled |
| 13 | MQTT_COMMAND_ENABLED | MQTT commands enabled |
| 14 | MQTT_REPORT_ENABLED | MQTT reporting enabled |
| 15 | MQTT_OPERATIONAL | MQTT fully operational |
| 16 | BURNER_OFF | Burner is off |
| 17 | BURNER_HEATING_LOW | Burner on low for heating |
| 18 | BURNER_HEATING_HIGH | Burner on high for heating |
| 19 | BURNER_WATER_LOW | Burner on low for water |
| 20 | BURNER_WATER_HIGH | Burner on high for water |
| 21 | BURNER_ERROR | Burner in error state |
| 22 | EMERGENCY_STOP | Emergency stop active |
| 23 | WARNING | System warning active |

**Usage Example**:
```cpp
// Check if heating is active
EventBits_t bits = xEventGroupGetBits(SRP::getSystemStateEventGroup());
if (bits & SystemEvents::SystemState::HEATING_ON) {
    // Heating is active
}

// Enable water heating
xEventGroupSetBits(SRP::getSystemStateEventGroup(),
                   SystemEvents::SystemState::WATER_ENABLED);
```

### 2. Burner Event Group
**Handle**: `xBurnerEventGroup` (via `SRP::getBurnerEventGroup()`)
**Purpose**: Burner-specific control and status

| Bit | Name | Description |
|-----|------|-------------|
| 0 | IDLE | Burner is idle |
| 1 | STARTING | Burner is starting |
| 2 | RUNNING | Burner is running |
| 3 | STOPPING | Burner is stopping |
| 4 | LOCKOUT | Burner in lockout state |
| 5 | ENABLE | Burner enable signal |
| 6 | LOW_POWER | Low power mode |
| 7 | HIGH_POWER | High power mode |
| 8 | PURGE | Purge cycle active |
| 9 | FLAME_DETECTED | Flame detected |
| 10 | TEMP_OK | Temperature within limits |
| 11 | PRESSURE_OK | Pressure within limits |
| 12 | SAFETY_OK | All safety checks OK |
| 13 | ERROR_IGNITION | Ignition failure |
| 14 | ERROR_FLAME_LOSS | Flame loss during operation |
| 15 | ERROR_OVERHEAT | Overheat condition |
| 16 | ERROR_PRESSURE | Pressure error |
| 17 | FLAME_STATE_CHANGED | Flame state changed |
| 18 | PRESSURE_CHANGED | Pressure sensor changed |
| 19 | FLOW_CHANGED | Flow sensor changed |
| 20 | SAFETY_EVENT | Any safety sensor changed |
| 21 | STATE_TIMEOUT | State machine timeout |

**Combinations**:
```cpp
constexpr EventBits_t ANY_ERROR = ERROR_IGNITION | ERROR_FLAME_LOSS |
                                   ERROR_OVERHEAT | ERROR_PRESSURE;
```

**Usage Example**:
```cpp
// Wait for burner to be running
EventBits_t bits = xEventGroupWaitBits(
    SRP::getBurnerEventGroup(),
    SystemEvents::Burner::RUNNING,
    pdFALSE,  // Don't clear
    pdFALSE,  // Wait for any
    pdMS_TO_TICKS(5000)  // 5 second timeout
);

// Check for any errors
if (bits & SystemEvents::Burner::ANY_ERROR) {
    // Handle error
}
```

### 3. BurnerRequest Event Group
**Handle**: `xBurnerRequestEventGroup` (via `SRP::getBurnerRequestEventGroup()`)
**Purpose**: Burner demand coordination between heating and water

| Bit | Name | Description |
|-----|------|-------------|
| 0 | HEATING | Space heating requesting burner |
| 1 | WATER | Water heating requesting burner |
| 3 | POWER_LOW | Request low power mode |
| 4 | POWER_HIGH | Request high power mode |
| 16-23 | TEMPERATURE | **Encoded temperature** (8 bits) |
| 18 | CHANGED | Any request changed |
| 19 | HEATING_CHANGED | Heating request changed |
| 20 | WATER_CHANGED | Water request changed |

#### Temperature Encoding

The target temperature is encoded in bits 16-23 (8 bits):

```cpp
// Encoding (BurnerRequestManager.cpp)
constexpr int TEMPERATURE_SHIFT = 16;
constexpr EventBits_t TEMPERATURE_MASK = 0xFF0000UL;  // 8 bits

// Encode: temperature in whole degrees (0-255°C)
EventBits_t encodedTemp = (targetTempC << TEMPERATURE_SHIFT) & TEMPERATURE_MASK;

// Decode: extract temperature
uint8_t targetTempC = (bits >> TEMPERATURE_SHIFT) & 0xFF;
```

**Example Values**:
- 55°C → 0x370000 (bits 16-23 = 0x37 = 55)
- 70°C → 0x460000 (bits 16-23 = 0x46 = 70)
- 85°C → 0x550000 (bits 16-23 = 0x55 = 85)

**Usage**:
```cpp
// Set water request with 65°C target
BurnerRequestManager::setWaterRequest(
    tempFromWhole(65),  // Temperature_t
    true,               // High power
    true                // Priority
);
// Internally encodes 65 into bits 16-23

// Read current request
EventBits_t bits = xEventGroupGetBits(SRP::getBurnerRequestEventGroup());
uint8_t targetTemp = (bits >> 16) & 0xFF;  // Extract temperature
```

### 4. SensorUpdate Event Group
**Handle**: `xSensorEventGroup` (via `SRP::getSensorEventGroup()`)
**Purpose**: Sensor reading notifications

| Bit | Name | Description |
|-----|------|-------------|
| 0 | BOILER_OUTPUT | Boiler output temp updated |
| 1 | BOILER_RETURN | Boiler return temp updated |
| 2 | WHEATER_TANK | Water tank temp updated |
| 3 | WHEATER_OUTPUT | Water output temp updated |
| 4 | WHEATER_RETURN | Water return temp updated |
| 5 | HEATING_RETURN | Heating return temp updated |
| 6 | OUTSIDE | Outside temp updated |
| 7 | INSIDE | Inside/room temp updated |
| 8 | PRESSURE | Pressure sensor updated |
| 9 | ANY_WHEATER | Any water sensor updated |
| 10 | ANY_HEATING | Any heating sensor updated |
| 11 | ANY_TEMP | Any temperature updated |
| 15 | PRESSURE_ERROR | Pressure sensor error |

**Usage Example**:
```cpp
// Wait for boiler temperature update
EventBits_t bits = xEventGroupWaitBits(
    SRP::getSensorEventGroup(),
    SystemEvents::SensorUpdate::BOILER_OUTPUT,
    pdTRUE,  // Clear on exit
    pdFALSE,
    pdMS_TO_TICKS(1000)
);

// Notify that sensor was updated
xEventGroupSetBits(SRP::getSensorEventGroup(),
                   SystemEvents::SensorUpdate::PRESSURE);
```

### 5. ControlRequest Event Group
**Handle**: (various - heating, water control tasks)
**Purpose**: Control request signaling

| Bit | Name | Description |
|-----|------|-------------|
| 0 | ENABLE_HEATING | Enable space heating |
| 1 | DISABLE_HEATING | Disable space heating |
| 2 | ENABLE_WATER | Enable water heating |
| 3 | DISABLE_WATER | Disable water heating |
| 4 | PUMP_START | Start pump |
| 5 | PUMP_STOP | Stop pump |
| 6 | BURNER_START | Start burner |
| 7 | BURNER_STOP | Stop burner |

## Event-Driven Task Pattern

Tasks use event groups to eliminate polling:

```cpp
void TaskFunction(void* parameter) {
    while (true) {
        // Wait for any relevant events
        EventBits_t bits = xEventGroupWaitBits(
            eventGroup,
            EVENT_MASK,     // Which events to wait for
            pdTRUE,         // Clear bits on exit
            pdFALSE,        // Wait for ANY bit (not all)
            pdMS_TO_TICKS(timeout)
        );

        // Process events
        if (bits & EVENT_A) {
            handleEventA();
        }
        if (bits & EVENT_B) {
            handleEventB();
        }

        // Feed watchdog
        Watchdog::feed();
    }
}
```

## Change Event Pattern

For detecting changes (not just state):

```cpp
// Burner request manager sets CHANGED bits
xEventGroupSetBits(group, HEATING_CHANGED | CHANGED);

// Burner control task waits for changes
EventBits_t bits = xEventGroupWaitBits(
    SRP::getBurnerRequestEventGroup(),
    SystemEvents::BurnerRequest::CHANGED,
    pdTRUE,  // Clear on exit - ready for next change
    pdFALSE,
    pdMS_TO_TICKS(5000)
);

// Process the change
if (bits & CHANGED) {
    // Read current request state
    EventBits_t request = xEventGroupGetBits(group);
    processNewRequest(request);
}
```

## Event Group Organization

```
FreeRTOS Event Groups (24 bits each)
├── SystemState (14 resources created by SharedResourceManager)
│   ├── GeneralSystem
│   ├── System
│   ├── SystemState
│   ├── ControlRequests
│   ├── Wheater
│   ├── Heating
│   ├── Burner
│   ├── BurnerRequest
│   ├── Sensor
│   ├── ErrorNotification
│   ├── Timer
│   ├── Relay
│   ├── RelayStatus
│   └── RelayRequest
└── (Additional MQTT event group created dynamically)
```

## Benefits of Event-Driven Architecture

1. **Lower Latency** - React immediately to changes (not poll delay)
2. **CPU Efficiency** - Tasks sleep until events occur
3. **Deterministic** - Guaranteed response within timeout
4. **No Race Conditions** - Atomic bit operations
5. **Type Safety** - Compile-time namespace checking
6. **Self-Documenting** - Clear event names vs magic numbers

## Common Patterns

### Pattern 1: Request-Response
```cpp
// Requester
xEventGroupSetBits(requestGroup, REQUEST_BIT);

// Responder
EventBits_t bits = xEventGroupWaitBits(requestGroup, REQUEST_BIT, ...);
// Handle request
xEventGroupClearBits(requestGroup, REQUEST_BIT);  // Acknowledge
```

### Pattern 2: State Publishing
```cpp
// Publisher
xEventGroupSetBits(statusGroup, PUMP_ON);
// When state changes
xEventGroupClearBits(statusGroup, PUMP_ON);

// Subscriber (multiple tasks can read)
EventBits_t bits = xEventGroupGetBits(statusGroup);
bool pumpRunning = bits & PUMP_ON;
```

### Pattern 3: Change Notification
```cpp
// Notifier
currentValue = newValue;
xEventGroupSetBits(group, VALUE_CHANGED);

// Listener
EventBits_t bits = xEventGroupWaitBits(group, VALUE_CHANGED, pdTRUE, ...);
// Read new value
// CHANGED bit automatically cleared by pdTRUE flag
```

## Event Flow Examples

### Burner Start Sequence

```
1. WheaterControl detects tank temp < setpoint
   → Sets BurnerRequest::WATER | WATER_CHANGED | CHANGED
   → Encodes target temp (65°C) in bits 16-23

2. BurnerControl task wakes on CHANGED bit
   → Reads request bits
   → Decodes target temp
   → Performs safety checks

3. BurnerControl sets Burner::ENABLE
   → State machine starts: IDLE → PRE_PURGE

4. After pre-purge (10s)
   → State machine: PRE_PURGE → IGNITION
   → Sets SystemState::BURNER_ON

5. After ignition confirmed
   → State machine: IGNITION → RUNNING_HIGH
   → Sets Burner::RUNNING | HIGH_POWER

6. WheaterPump detects SystemState::WATER_ON
   → Starts water circulation pump
   → Sets SystemState::WATER_PUMP_ON
```

### Emergency Shutdown

```
1. SafetyInterlocks detects pressure < 0.5 BAR
   → Sets Burner::ERROR_PRESSURE

2. BurnerControl checks safety on every cycle
   → Detects ERROR_PRESSURE bit
   → Immediately sets SystemState::EMERGENCY_STOP
   → Clears all burner request bits
   → State machine: ANY → ERROR

3. CentralizedFailsafe triggered
   → Saves emergency state to FRAM
   → Notifies all tasks via EMERGENCY_STOP bit

4. All control tasks react
   → WheaterControl clears WATER request
   → HeatingControl clears HEATING request
   → Pumps stop
   → System enters safe state
```

## Debugging Events

### Enable Event Logging
```cpp
// In task code
#define LOG_EVENTS  // Enable event bit logging

EventBits_t bits = xEventGroupWaitBits(...);
LOG_DEBUG(TAG, "Events received: 0x%06X", bits);
```

### Common Event Combinations

```cpp
// Burner active for water heating at high power
EventBits_t expectedBits = SystemState::BURNER_ON |
                           SystemState::WATER_ON |
                           Burner::RUNNING |
                           Burner::HIGH_POWER;

// Any heating-related activity
EventBits_t heatingMask = SystemState::HEATING_ON |
                          SystemState::HEATING_PUMP_ON |
                          BurnerRequest::HEATING;
```

## Event Bit Best Practices

1. **Use Namespaces** - Always use `SystemEvents::Category::BIT_NAME`
2. **Clear Change Bits** - Use `pdTRUE` in `xEventGroupWaitBits()` for change events
3. **Atomic Reads** - Event bits are atomic (no mutex needed for reading)
4. **Timeout Always** - Never wait indefinitely (use reasonable timeouts)
5. **Log Events** - Log significant events for debugging
6. **Combine Logically** - Use OR for multiple events, AND for all-required

## Performance Characteristics

- **Set/Clear**: O(1) - Single ARM instruction
- **Wait**: Blocks task until event (zero CPU)
- **Read**: O(1) - Direct memory access
- **Multiple Tasks**: Can wait on same event group
- **Memory**: 4 bytes per event group (24 bits + control)

## Code Generation

Events are generated from a Python script for consistency:

```bash
# Regenerate events (if needed)
python scripts/generate_events_zero_overhead.py

# Output: include/events/SystemEventsGenerated.h
```

## Migration Notes

The project was migrated from polling to event-driven architecture in commits:
- `c92bd69` through `1c94c2c` - Event system implementation
- `6b75161` - Zero-overhead event generation
- Recent commits fixed race conditions in event handling

**Before (Polling)**:
```cpp
void task() {
    while (true) {
        if (millis() - lastCheck > INTERVAL) {
            checkForChanges();
            lastCheck = millis();
        }
        vTaskDelay(pdMS_TO_TICKS(100));  // Poll every 100ms
    }
}
```

**After (Event-Driven)**:
```cpp
void task() {
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(group, MASK, ...);
        if (bits & CHANGE_EVENT) {
            handleChange();  // Immediate response
        }
    }
}
```

## Event Group Reference

| Event Group | Purpose | Primary Users |
|-------------|---------|---------------|
| SystemState | Global system state | All tasks |
| Burner | Burner control/status | BurnerControl, Safety |
| BurnerRequest | Demand coordination | Heating, Water, Burner |
| Sensor | Sensor data ready | Control tasks |
| ControlRequest | Command signaling | MQTT, UI, Scheduler |

Total event bits available: 5 groups × 24 bits = 120 event bits
Currently used: ~85 bits (~71% utilization)
