# Event System Architecture

## Overview

The boiler controller uses a zero-overhead event system based on FreeRTOS event groups. Event bits are defined as `constexpr` constants in organized namespaces, providing type safety and self-documentation with no runtime cost.

**Key File**: `include/events/SystemEventsGenerated.h` (auto-generated from script)

## Event Groups

SharedResourceManager creates 11 named event groups (see [Event Group Organization](#event-group-organization)). The five most used are described below; all bit definitions are listed in [generated/events.md](generated/events.md).

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

**Bits set by current firmware**: only `ERROR_PRESSURE` and `PRESSURE_OK` (MB8ARTTask, pressure alarm check) and `STATE_TIMEOUT` (BurnerControlTask 1 s timer). The other bits are defined but never set, and nothing reads `ERROR_PRESSURE`. The burner state is published in SystemState (`BURNER_ON`, `BURNER_HEATING_LOW/HIGH`, `BURNER_WATER_LOW/HIGH`, `BURNER_OFF`, `BURNER_ERROR`).

**Usage Example** (BurnerControlTask main loop):
```cpp
// Read-and-clear the timer event without waiting
EventBits_t timeoutBits = xEventGroupWaitBits(
    SRP::getBurnerEventGroup(),
    SystemEvents::Burner::STATE_TIMEOUT,
    pdTRUE,   // Clear on exit
    pdFALSE,  // Wait for any
    0         // No wait
);
if (timeoutBits & SystemEvents::Burner::STATE_TIMEOUT) {
    BurnerStateMachine::update();
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
| 5 | CHANGED | Any request changed |
| 6 | HEATING_CHANGED | Heating request changed |
| 7 | WATER_CHANGED | Water request changed |
| 16-23 | TEMPERATURE | **Encoded temperature** (8 bits, no flags in this range) |

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
    true                // High power
);
// Priority is read from SystemState::WATER_PRIORITY
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
| 2 | WATER_TANK | Water tank temp updated |
| 3 | WATER_OUTPUT | Water output temp updated |
| 4 | WATER_RETURN | Water return temp updated |
| 5 | HEATING_RETURN | Heating return temp updated |
| 6 | OUTSIDE | Outside temp updated |
| 7 | INSIDE | Inside/room temp updated |
| 8 | EXHAUST | Exhaust temp updated |
| 9 | DATA_AVAILABLE | Sensor data available |
| 10-18 | BOILER_OUTPUT_ERROR ... EXHAUST_ERROR | Per-sensor error, same order as bits 0-8 |
| 19 | DATA_ERROR | General sensor data error |
| 20 | PRESSURE | System pressure updated |
| 21 | PRESSURE_ERROR | Pressure sensor error |
| 23 | FIRST_READ_COMPLETE | First sensor read complete |

Combinations: `ALL_TEMPS` (bits 0-7), `CRITICAL_TEMPS` (`BOILER_OUTPUT | EXHAUST`).

**Who sets the per-channel bits**: the MB8ART library, using `SensorHardware::CONFIGS` (`include/config/SensorHardwareConfig.h`, index = channel from `SensorIndices.h`): CH0 `BOILER_OUTPUT`, CH1 `BOILER_RETURN`, CH2 `WATER_TANK`, CH3 `OUTSIDE`, CH6 `WATER_RETURN`, CH7 `HEATING_RETURN` with their `*_ERROR` bits; CH4 (pressure) and CH5 have no library bits, `PRESSURE` / `PRESSURE_ERROR` come from MB8ARTTasks after conversion. `INSIDE` / `INSIDE_ERROR` come from ANDRTF3Task, `DATA_AVAILABLE` / `DATA_ERROR` / `FIRST_READ_COMPLETE` from MB8ARTTasks. Before 2026-09-15 the library ignored the table and used interleaved bits (update 2n, error 2n+1), which overlapped these names (see CHANGELOG).

**Consumers**: BoilerTempControlTask waits on `BOILER_OUTPUT` (clear-on-exit), BurnerControlTask polls `BOILER_RETURN | WATER_TANK` and also updates on its 1 s timer, SafetyInterlocks reads `DATA_AVAILABLE`, MonitoringTask and OtaRollbackGuard read `FIRST_READ_COMPLETE`.

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
**Handle**: `xControlRequestEventGroup` (via `SRP::getControlRequestsEventGroup()`)
**Purpose**: Remote control requests from MQTT/scheduler

| Bit | Name | Description |
|-----|------|-------------|
| 0-1 | BOILER_ENABLE / BOILER_DISABLE | Enable / disable boiler system |
| 2-3 | HEATING_ENABLE / HEATING_DISABLE | Enable / disable heating |
| 4 | HEATING_ON_OVERRIDE | Force heating on |
| 5 | HEATING_OFF_OVERRIDE | Force heating off |
| 6-7 | WATER_ENABLE / WATER_DISABLE | Enable / disable water heating |
| 8-9 | WATER_PRIORITY_ENABLE / WATER_PRIORITY_DISABLE | Enable / disable water priority |
| 10 | WATER_ON_OVERRIDE | Force water heating on |
| 11 | WATER_OFF_OVERRIDE | Force water heating off |
| 12-17 | MQTT_ENABLE ... MQTT_REPORT_DISABLE | MQTT enable / command / report switches |
| 18 | WATER_PRIORITY_RELEASED | Water priority was released (notify heating) |
| 19 | PID_SAVE | Save PID parameters |
| 20 | SAVE_PARAMETERS | Save system parameters |
| 22 | PID_AUTOTUNE | Start PID auto-tuning |
| 23 | PID_AUTOTUNE_STOP | Stop PID auto-tuning |

**Control-task handling**: HeatingControlTask (`HEATING_ON_OVERRIDE`, `HEATING_OFF_OVERRIDE`, `WATER_PRIORITY_RELEASED`) and WheaterControlTask (`WATER_ON_OVERRIDE`, `WATER_OFF_OVERRIDE`) check the group after each timer wait, clear the bits they found and pass the override bits to `processHeatingState(pendingControl)` / `processWaterHeatingState(pendingControl)`, which OR them into the bits they read.

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
// (timeout 100 ms in IGNITION/RUNNING, 3 s when IDLE without demand, else 1 s)
EventBits_t bits = xEventGroupWaitBits(
    SRP::getBurnerRequestEventGroup(),
    SystemEvents::BurnerRequest::CHANGE_EVENT_BITS,
    pdTRUE,  // Clear on exit - a change during processing is kept for the next pass
    pdFALSE,
    pdMS_TO_TICKS(timeoutMs)
);

// Process the change
if (bits & CHANGE_EVENT_BITS) {
    // Read current request state
    EventBits_t request = xEventGroupGetBits(group);
    processNewRequest(request);
}
```

## Event Group Organization

```
FreeRTOS Event Groups (24 usable bits each, bits 24-31 reserved by FreeRTOS)
└── SharedResourceManager::initializeStandardResources() (11 groups)
    ├── GeneralSystem
    ├── SystemState
    ├── ControlRequests
    ├── Heating
    ├── Burner
    ├── BurnerRequest
    ├── Sensor
    ├── ErrorNotification
    ├── Relay
    ├── RelayStatus
    └── RelayRequest
```

Further event groups are created outside SharedResourceManager (device-ready group in SystemInitializer, `xGeneralSystemEventGroup` in `main.cpp`, SchedulerContext, TaskDependencyManager, EventAggregator instances).

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
1. WheaterControlTask starts a charge
   → Sets SystemState::WATER_ON
   → BurnerRequestManager::setWaterRequest(): BurnerRequest::WATER, power bit,
     target encoded in bits 16-23, CHANGED | WATER_CHANGED

2. BurnerControlTask wakes on CHANGE_EVENT_BITS
   → Decodes target, safety checks, publishes BurnerDemandGate permission
   → BurnerStateMachine::setHeatDemand(true) now, or later by BoilerTempControlTask

3. BurnerStateMachine: IDLE → PRE_PURGE (2 s) → IGNITION → RUNNING_LOW/HIGH
   → Sets SystemState::BURNER_ON on entering RUNNING_LOW/HIGH
   → BURNER_WATER_LOW/HIGH are set by BurnerControlTask::updateBurnerState(),
     which only runs on a request change (often still BURNER_OFF at that time)

4. WaterPumpTask (PumpControlModule) follows SystemState::WATER_ON
   → Sets RelayRequest::WATER_PUMP_ON and SystemState::WATER_PUMP_ON
```

Details: [EVENT_FLOW.md, Water Heating Request Flow](EVENT_FLOW.md#water-heating-request-flow).

### Emergency Shutdown

```
1. Failed safety interlock while the burner runs
   → BurnerStateMachine::emergencyStop(): burner relays OFF, state ERROR
     (SystemState::EMERGENCY_STOP is not set on this path)

2. Coordinated stop: CentralizedFailsafe::emergencyStop()
   → Burner relays OFF, both pumps forced ON for heat dissipation
   → Sets SystemState::EMERGENCY_STOP (latched), clears BOILER_ENABLED

3. Reactions
   → BurnerControlTask: BurnerStateMachine::emergencyStop() once per onset
   → PumpControlModule: pumps stay ON until the boiler output has cooled
   → Heating/water tasks end their requests because BOILER_ENABLED is cleared
```

MB8ARTTask sets `Burner::ERROR_PRESSURE` on a pressure alarm, but no task reads it. Details and release: [EVENT_FLOW.md, Emergency Stop Flow](EVENT_FLOW.md#emergency-stop-flow).

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
// Burner active for water heating at high power (SystemState group)
EventBits_t expectedBits = SystemState::BURNER_ON |
                           SystemState::WATER_ON |
                           SystemState::BURNER_WATER_HIGH;

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
# Run from the repository root
python3 tools/generate_events_zero_overhead.py tools/event_config.yaml

# Output: include/events/SystemEventsGenerated.h and docs/generated/events.md
# (paths from the output: section of event_config.yaml)
```

Regenerating reproduces the committed header byte for byte, except the `Generated on:` timestamp line. Edit `tools/event_config.yaml`, never the header. Quote names that YAML would read as booleans (the HeatingEvent names are `"True"` / `"False"`). Optional config keys:
- `comment` on an event: comment line emitted above that constant
- `next_free_bit` on a group: replaces the computed "Next free bit" text
- `migration_macro_order` on a group: order of the migration helper macros (must list every event once)

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

Each event group has 24 usable bits (bits 24-31 are reserved by FreeRTOS). The table lists the five groups described above; SharedResourceManager creates 11 (see Event Group Organization).
