# System Initialization Order

**Critical Document**: This initialization sequence MUST be followed exactly. Violating the order will cause system failures, nullptr dereferences, or undefined behavior.

---

## 🎯 Overview

The ESP32 Boiler Controller uses a carefully orchestrated initialization sequence to ensure all dependencies are satisfied before resources are used. The initialization is managed by `SystemInitializer` class in `src/init/SystemInitializer.cpp`.

---

## 📋 Initialization Stages

### Stage 0: Pre-Main Setup
**Location**: `src/main.cpp` (before `setup()`)

```cpp
// Global instances that MUST exist before setup()
TaskManager taskManager(&Watchdog::getInstance());  // Singleton watchdog
```

**Dependencies**: None
**RAM Impact**: ~80 bytes (TaskManager state)

---

### Stage 1: Logging System
**Function**: `initializeLogging()`
**Location**: `src/init/LoggingInitializer.cpp`

```cpp
Logger& logger = Logger::getInstance();  // Singleton
logger.setMaxLogsPerSecond(200);
logger.setLogLevel(LogLevel::INFO);
```

**Dependencies**: None (MUST be first for all subsequent logging)
**RAM Impact**: ~2KB (log buffers, backend)
**Critical**: All subsequent stages log their progress

---

### Stage 2: Shared Resources
**Function**: `initializeSharedResources()`
**Location**: `src/init/SystemInitializer.cpp`

**Creates in order:**
1. **RelayState mutex** (`initRelayState()`)
   - Creates: `delayMutex` for hardware DELAY tracking
   - RAM: 80 bytes
   - **Critical**: Must exist before RelayControlTask

2. **SharedResourceManager singleton**
   - Creates: All FreeRTOS primitives (mutexes, event groups, queues)
   - RAM: ~2KB
   - **Critical**: Required before any SRP access

3. **Event Groups** (via SRP)
   - `SystemStateEventGroup`
   - `GeneralSystemEventGroup`
   - `SensorEventGroup`
   - `RelayEventGroup`
   - `ControlRequestsEventGroup`

4. **Mutexes** (via SRP)
   - `sensorReadingsMutex`
   - `relayReadingsMutex`
   - `systemSettingsMutex`
   - `modbusCoordinatorMutex`

**Dependencies**: Logger
**RAM Impact**: ~2KB
**Failure Mode**: System cannot proceed without event groups

---

### Stage 3: Hardware Buses
**Function**: `initializeHardware()`
**Location**: `src/init/HardwareInitializer.cpp`

**Initializes in order:**
1. **I2C** (`Wire.begin()`)
   - SDA: GPIO 21, SCL: GPIO 22
   - Speed: 100kHz
   - Devices: DS3231 RTC, FRAM (optional)

2. **SPI** (if needed)
   - Not currently used

3. **UART/Serial**
   - Modbus RTU: RX=36, TX=4, 9600 baud
   - Serial console: 921600 baud

4. **Ethernet** (LAN8720A PHY)
   - MDC: GPIO 23, MDIO: GPIO 18, CLK: GPIO 17
   - Static IP configuration
   - Non-blocking initialization

**Dependencies**: Logger, SharedResources
**RAM Impact**: ~1KB (driver buffers)
**Failure Mode**: Devices won't communicate, but can continue in degraded mode

---

### Stage 4: Modbus Devices
**Function**: `initializeModbusDevices()`
**Location**: `src/init/ModbusDeviceInitializer.cpp`

**Initializes in order:**
1. **MB8ART** (Temperature Sensors)
   - Address: 0x01
   - 8 channels (5 active)
   - Poll interval: 2000ms

2. **RYN4** (Relay Controller)
   - Address: 0x02
   - 8 relays with hardware DELAY support
   - Poll interval: 1000ms (SET), 2000ms (READ)

3. **ANDRTF3** (Room Temperature)
   - Address: 0x03
   - Single channel
   - Poll interval: 5000ms

**Dependencies**: UART initialized, Logger, SharedResources
**RAM Impact**: ~3KB (device buffers, Modbus library)
**Failure Mode**: Critical - cannot operate without sensors/relays

---

### Stage 5: Network (Async)
**Function**: `initializeNetworkAsync()`
**Location**: `src/init/NetworkInitializer.cpp`

**Initializes (non-blocking):**
1. **Ethernet** (LAN8720A)
   - Static IP: 192.168.20.40
   - Waits up to 10 seconds for link

2. **MQTT** (via MQTTManager)
   - Broker: 192.168.20.27:1883
   - Client ID: "esplan-{hostname}"
   - Auto-reconnect enabled

3. **NTP** (via NTPClient)
   - Pool: pool.ntp.org
   - Timezone: Europe/Vienna
   - Syncs RTC if DS3231 available

4. **OTA** (ArduinoOTA)
   - Port: 3232
   - Password protected

**Dependencies**: Ethernet PHY, Logger, SharedResources
**RAM Impact**: ~4KB (network stack, MQTT buffers)
**Failure Mode**: Non-critical - system enters degraded mode

---

### Stage 6: Control Modules
**Function**: `initializeControlModules()`
**Location**: `src/init/SystemInitializer.cpp`

**Creates in order:**
1. **TemperatureSensorFallback**
   - Sensor validation and fallback logic

2. **HeatingControlModule**
   - Space heating control with weather compensation

3. **WheaterControlModule** (Water heating)
   - Tank heating with scheduling

4. **PIDControlModule**
   - PID temperature control

5. **BurnerSystemController**
   - Integrates all control logic
   - 8-state burner FSM
   - 4-layer safety system

6. **CentralizedFailsafe**
   - Emergency shutdown coordinator

**Dependencies**: Sensors initialized, Logger, SharedResources, SRP
**RAM Impact**: ~3KB (control state, PID coefficients)
**Failure Mode**: Critical - cannot control burner

---

### Stage 7: FreeRTOS Tasks
**Function**: `initializeTasks()`
**Location**: `src/init/TaskInitializer.cpp`

**Tasks created in priority order** (high to low):

| Priority | Task | Stack (DEBUG_SELECTIVE) | Purpose |
|----------|------|-------------------------|---------|
| 4 | **BurnerControlTask** | 4096 bytes | Safety-critical burner FSM |
| 4 | **RelayControlTask** | 4096 bytes | Physical relay control |
| 3 | **HeatingControlTask** | 3584 bytes | Space heating control |
| 3 | **WheaterControlTask** | 3584 bytes | Water heating control |
| 3 | **ControlTask** | 3584 bytes | General control coordination |
| 3 | **SensorTask** (ANDRTF3) | 3584 bytes | Room temperature |
| 3 | **MB8ARTTask** | 3072 bytes | Temperature sensor array |
| 3 | **RYN4Task** | 2560 bytes | Relay status polling |
| 2 | **MQTTTask** | 3584 bytes | MQTT communication |
| 2 | **MonitoringTask** | 3584 bytes | Health monitoring |
| 2 | **TimerSchedulerTask** | 3072 bytes | Schedule management |
| 2 | **HeatingPumpTask** | 3072 bytes | Heating circulation pump |
| 2 | **WaterPumpTask** | 3072 bytes | Hot water loading pump |
| 1 | **OTATask** | 3072 bytes | Firmware updates |
| 1 | **NTPTask** | 2048 bytes | Time synchronization |

**Dependencies**: ALL previous stages complete
**RAM Impact**: ~54KB (total task stacks)
**Failure Mode**: Task creation failure is fatal

---

## ⚠️ Critical Dependencies Summary

### Must Initialize Before Tasks Start:
1. ✅ **Logger** - Required for all logging
2. ✅ **RelayState::initRelayState()** - Creates delay mutex
3. ✅ **SharedResourceManager** - Creates all FreeRTOS primitives
4. ✅ **Modbus devices** - Required for sensor/relay data
5. ✅ **Control modules** - Required for burner FSM

### Can Initialize Asynchronously:
- Network (Ethernet, MQTT, NTP, OTA)
- DS3231 RTC (graceful degradation if missing)
- FRAM logging (optional)

---

## 🔒 Thread-Safety During Initialization

**Initialization is SINGLE-THREADED** until tasks are created:
- Main thread runs `setup()` → `SystemInitializer::initializeSystem()`
- No mutexes needed until Stage 7
- After task creation, all access via SRP + mutexes

---

## 📊 Stack Budget

### DEBUG_SELECTIVE Mode (Default)
```
Total task stacks: ~54KB
Heap available:    ~280KB
Stack margins:     448-2568 bytes free (runtime measured)
Critical tasks:    +512 bytes safety margin (H3 optimization)
```

### RELEASE Mode (Production)
```
Total task stacks: ~30KB (aggressive optimization)
Heap available:    ~297KB
Stack margins:     Minimal but verified safe
```

---

## 🚨 Common Initialization Errors

### Error: Nullptr dereference in task
**Cause**: Task started before resource initialized
**Fix**: Verify initialization order, check Stage completion

### Error: Mutex timeout during init
**Cause**: Attempting mutex lock before mutex created
**Fix**: Check SharedResourceManager initialized first

### Error: Modbus communication failure
**Cause**: UART not initialized before Modbus devices
**Fix**: Ensure Stage 3 (Hardware) before Stage 4 (Modbus)

### Error: Task creation failed
**Cause**: Insufficient heap memory
**Fix**: Check stack sizes in `ProjectConfig.h`, reduce if needed

---

## 🔧 Modifying Initialization Order

**⚠️ WARNING**: Changing initialization order is DANGEROUS!

**If you must change:**
1. Update this document FIRST with rationale
2. Test ALL build modes (debug_full, debug_selective, release)
3. Verify with runtime stack monitoring
4. Check for race conditions in first 30 seconds
5. Update `SystemInitializer::InitStage` enum

---

## 📚 Related Documentation

- **src/init/SystemInitializer.h** - InitStage enum definitions
- **docs/TASK_ARCHITECTURE.md** - Complete task descriptions
- **docs/MUTEX_HIERARCHY.md** - Deadlock prevention
- **docs/MEMORY_OPTIMIZATION.md** - Stack vs heap trade-offs
- **src/config/ProjectConfig.h** - Stack size definitions

---

## 🎓 Learning Resources

### Why This Order Matters:
1. **Logger first**: All stages need logging for diagnostics
2. **Mutexes before SRP**: SRP assumes mutexes exist
3. **Hardware before devices**: Devices need communication buses
4. **Devices before control**: Control needs sensor data
5. **Everything before tasks**: Tasks assume resources ready

### Initialization Patterns:
- **Result&lt;T&gt;**: All init functions return error status
- **Staged cleanup**: Each stage stores `currentStage_` for rollback
- **Graceful degradation**: Network failure doesn't stop system
- **Defensive checks**: nullptr checks before dereference

---

**Document Version**: 0.1.0
**Last Updated**: 2025-12-16
**Status**: ✅ Production-verified initialization sequence
