# System Initialization Order Documentation

## Overview

The ESPlan Boiler Controller follows a strict initialization sequence to ensure all dependencies are satisfied and resources are properly allocated. This document details the initialization order, dependencies, and cleanup procedures.

## Initialization Stages

### Stage 1: Logging System
**Purpose**: Establish logging infrastructure before any other operations

**Components Initialized**:
- Serial port (115200 baud)
- ConsoleBackend
- Logger instance with backend
- Log levels based on build mode

**Dependencies**: None

**Critical Requirements**:
- Must complete before any LOG_* macros are used
- Serial must be ready within SERIAL_TIMEOUT_MS (2000ms)

### Stage 2: Shared Resources
**Purpose**: Create all mutexes, semaphores, and event groups used for inter-task communication

**Components Initialized**:
1. **Mutexes** (in order):
   - `xSharedSensorReadingsMutex` - Protects temperature sensor data
   - `xSharedRelayReadingsMutex` - Protects relay state data
   - `xSystemSettingsMutex` - Protects system configuration
   - `xSensorMiThMutex` - Protects BLE sensor data
   - `mqttMutex` - Protects MQTT operations

2. **Event Groups** (in order):
   - `xSensorEventGroup` - Sensor update notifications
   - `xRelayEventGroup` - Relay control requests
   - `xSystemEventGroup` - System-wide events
   - `xSystemStateEventGroup` - System state changes
   - `xBurnerEventGroup` - Burner-specific events
   - `xHeatingEventGroup` - Heating system events
   - `xWheaterEventGroup` - Water heater events
   - `xControlAndRequestsEventGroup` - Control requests
   - `xErrorNotificationGroup` - Error notifications
   - `xTimerEventGroup` - Timer events
   - `xRelayStatusGroup` - Relay status updates
   - `xSensorMiThEventGroup` - BLE sensor events

**Dependencies**: Logging system must be initialized

**Critical Requirements**:
- All handles must be created before any task starts
- Creation failure is fatal - system cannot continue

### Stage 3: Hardware Interfaces
**Purpose**: Initialize low-level hardware communication

**Components Initialized**:
1. RS485 Serial (Serial1)
   - Baud: RS485_BAUD_RATE (9600)
   - RX Pin: RS485_RX_PIN (36)
   - TX Pin: RS485_TX_PIN (4)
   - Format: 8N1

2. Modbus RTU Master
   - Uses Serial1 for communication
   - Registers mainHandleData callback

**Dependencies**: Shared resources must be initialized

**Critical Requirements**:
- Serial1 must be available (not used by other peripherals)
- GPIO pins must be free and correctly configured

### Stage 4: Network (Ethernet)
**Purpose**: Establish network connectivity for MQTT and OTA

**Components Initialized**:
1. Ethernet PHY (LAN8720)
   - MDC Pin: ETH_PHY_MDC_PIN (23)
   - MDIO Pin: ETH_PHY_MDIO_PIN (18)
   - PHY Address: ETH_PHY_ADDR (0)
   - Clock Mode: ETH_CLOCK_GPIO17_OUT

2. TCP/IP Stack
   - DHCP client (or static IP if configured)
   - DNS resolver
   - Network event handlers

**Dependencies**: 
- Hardware interfaces must be initialized
- Shared resources for network events

**Critical Requirements**:
- Ethernet cable must be connected
- PHY must respond within timeout
- IP address must be obtained within ETH_CONNECTION_TIMEOUT_MS (15s)

### Stage 5: Modbus Devices
**Purpose**: Initialize and verify communication with Modbus slave devices

**Components Initialized**:
1. **MB8ART** (Temperature Sensor Module)
   - Address: MB8ART_ADDRESS (0x01)
   - 8 temperature channels
   - Polling interval: 5 seconds

2. **RYN4** (Relay Module)
   - Address: MODBUS_RYN4_ADDRESS (0x02)
   - 8 relay channels
   - Custom relay mappings applied

**Dependencies**:
- Hardware interfaces (Serial1, Modbus master)
- Shared resources (for device registry)

**Critical Requirements**:
- Devices must respond to initialization within 10 seconds
- Device registry must be updated before any communication
- Failure is non-fatal but limits functionality

### Stage 6: Control Modules
**Purpose**: Initialize business logic modules

**Components Initialized** (in order):
1. `BurnerControlModule` - Gas burner control logic
2. `HeatingControlModule` - Space heating control
3. `WheaterControlModule` - Water heater control
4. `PIDControlModule` - PID controller (thread-safe instance)
5. `PumpControlModule` - Pump control logic
6. `HeatingPumpControlModule` - Heating pump specific logic
7. `WheaterPumpControlModule` - Water pump specific logic
8. `SystemControlModule` - Overall system coordination

**Dependencies**:
- Shared resources (event groups, mutexes)
- Modbus devices (for relay/sensor access)

**Critical Requirements**:
- PIDControlModule must create its mutex successfully
- HeatingControlModule requires valid event group handles

### Stage 7: MQTT
**Purpose**: Establish cloud connectivity for remote monitoring/control

**Components Initialized**:
1. MQTTManager instance
2. Network connectivity check
3. Broker connection
4. Initial status publications
5. Topic subscriptions

**Dependencies**:
- Network must be connected
- mqttMutex must be created
- Credentials must be available

**Critical Requirements**:
- Non-critical - system continues without MQTT
- Connection timeout: 10 seconds
- Credentials loaded from secure storage

### Stage 8: Tasks
**Purpose**: Create and start all FreeRTOS tasks

**Task Creation Order**:
1. **OTATask** - Over-the-air updates
2. **MonitoringTask** - System health monitoring
3. **MiThermometerSensorTask** - BLE sensor scanning
4. **MB8ARTStatusTask** - Temperature polling
5. **MB8ARTControlTask** - Temperature data processing
6. **RelayControlTask** - Relay command processing
7. **RelayStatusTask** - Relay status monitoring
8. **SensorTask** - Sensor data aggregation
9. **ControlTask** - Main control logic
10. **WheaterControlTask** - Water heater control
11. **PIDControlTask** - PID calculations
12. **BurnerControlTask** - Burner state machine

**Dependencies**:
- All previous stages must be complete
- Control modules must be initialized
- Devices must be ready

**Critical Requirements**:
- Each task must be created with sufficient stack
- Task handles must be registered with TaskManager
- Watchdog configuration for each task

## Cleanup Order (Reverse)

If initialization fails at any stage, cleanup proceeds in reverse order:

1. **Tasks**: Delete all created tasks
2. **MQTT**: Disconnect and cleanup manager
3. **Control Modules**: Delete all module instances
4. **Modbus Devices**: Unregister and delete devices
5. **Network**: Close connections
6. **Hardware**: Stop Modbus master, close Serial1
7. **Shared Resources**: Delete all event groups and mutexes
8. **Logging**: Final cleanup

## Error Handling

### Fatal Errors (System Halts)
- Mutex creation failure
- Event group creation failure
- Critical task creation failure
- System resource exhaustion

### Non-Fatal Errors (Degraded Operation)
- MQTT connection failure
- Individual Modbus device failure
- Non-critical task creation failure

### Recovery Mechanisms
- Automatic retry for network connections
- Device re-initialization attempts
- Graceful degradation for missing components

## Best Practices

1. **Always check return values** from initialization functions
2. **Log all failures** with specific error codes
3. **Clean up resources** if initialization fails
4. **Use the SystemInitializer class** for automatic cleanup
5. **Test initialization** with missing/failed components
6. **Monitor free heap** during initialization
7. **Verify stack sizes** for all tasks

## Timing Constraints

- Serial timeout: 2 seconds
- Ethernet connection: 15 seconds
- Modbus device init: 10 seconds each
- MQTT connection: 10 seconds
- Total boot time target: < 45 seconds

## Memory Requirements

Minimum free heap required at each stage:
- After shared resources: 100KB
- After hardware init: 95KB
- After network init: 85KB
- After Modbus devices: 75KB
- After control modules: 65KB
- After MQTT: 55KB
- After all tasks: 40KB (minimum operational)

## Dependencies Diagram

```
Logging
   │
   └─> Shared Resources
           │
           └─> Hardware Interfaces
                   │
                   └─> Network
                           │
                           └─> Modbus Devices
                                   │
                                   └─> Control Modules
                                           │
                                           ├─> MQTT (optional)
                                           │
                                           └─> Tasks
```

## Testing Initialization

Test scenarios to verify proper initialization:
1. Normal boot - all components present
2. No Ethernet cable connected
3. Modbus device disconnected
4. MQTT broker unreachable
5. Low memory conditions
6. Rapid reset cycles

---
*Last Updated: $(date)*
*Version: 1.0.0*