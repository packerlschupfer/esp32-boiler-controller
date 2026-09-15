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

Early in `setup()`, before `SystemInitializer::initializeSystem()` runs, `main.cpp` also configures the logger, creates `xGeneralSystemEventGroup` and pre-initializes the `SharedResourceManager` singleton.

---

### Stage 1: Logging System
**Function**: `initializeLogging()`
**Location**: `src/init/LoggingInitializer.cpp`

```cpp
// src/main.cpp setup(), before SystemInitializer:
logger.setMaxLogsPerSecond(0);   // unlimited during boot
// LoggingInitializer::initialize(): per-tag levels via Logger::setTagLevel()
// src/main.cpp after initialization completes:
Logger::getInstance().setMaxLogsPerSecond(200);
```

**Dependencies**: None (MUST be first for all subsequent logging)
**RAM Impact**: ~2KB (log buffers, backend)
**Critical**: All subsequent stages log their progress

---

### Stage 2: Shared Resources
**Function**: `initializeSharedResources()`
**Location**: `src/init/SystemInitializer.cpp`

**Creates in order:**
1. **SharedResourceManager standard resources** (`initializeStandardResources()`)
   - **Event Groups**: `GeneralSystem`, `SystemState`, `ControlRequests`, `Heating`, `Burner`, `BurnerRequest`, `Sensor`, `ErrorNotification`, `Relay`, `RelayStatus`, `RelayRequest`
   - **Mutexes**: `SensorReadings`, `RelayReadings`, `SystemSettings`, `MQTT`
   - **Critical**: Required before any SRP access

2. **Device ready event group** (`deviceReadyEventGroup_`)

3. **Clear stale event bits** in the Sensor, Burner, BurnerRequest, ErrorNotification, SystemState, ControlRequests, Heating, Relay, RelayStatus and RelayRequest groups

4. **StateManager::initialize()**
   - Syncs the enable states from settings to event bits, before any task starts

5. **RelayState mutex** (`initRelayState()`)
   - Creates: `delayMutex` for hardware DELAY tracking
   - **Critical**: Must exist before RelayControlTask

**Dependencies**: Logger
**RAM Impact**: ~2KB
**Failure Mode**: System cannot proceed without event groups

**Watchdog init (between Stage 2 and Stage 3)**: `SRP::getTaskManager().initWatchdog(30, true)` and `Watchdog::quickInit(30, true)` (30 s timeout, panic on timeout). A TaskManager watchdog init failure aborts initialization.

---

### Stage 3: Hardware Buses
**Function**: `initializeHardware()`
**Location**: `src/init/HardwareInitializer.cpp`

**Initializes in order:**
1. **I2C** (`SharedI2CInitializer::ensureI2CInitialized()`)
   - SDA: GPIO 33, SCL: GPIO 32 (`src/shared/SharedI2CInitializer.h`)
   - Speed: 100kHz
   - Devices: DS3231 RTC, FRAM (optional)

2. **SPI** (if needed)
   - Not currently used

3. **UART/Serial**
   - Modbus RTU: RX=36, TX=4, 9600 baud
   - Serial console: 921600 baud

4. **Timezone**: `setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3")`

Ethernet is not started here; see Stage 5.

**Dependencies**: Logger, SharedResources
**RAM Impact**: ~1KB (driver buffers)
**Failure Mode**: Devices won't communicate, but can continue in degraded mode

---

### Stage 4: Modbus Devices
**Function**: `initializeModbusDevices()`
**Location**: `src/init/ModbusDeviceInitializer.cpp`

**Initializes in order:**
1. **MB8ART** (Temperature Sensors)
   - Address: 0x03 (`MB8ART_ADDRESS`)
   - 8 channels (5 active)
   - Read interval: 2500ms (`MB8ART_SENSOR_READ_INTERVAL_MS`)
   - Starts the `MB8ARTProc` and `MB8ART` tasks (core 1)

2. **RYN4** (Relay Controller)
   - Address: 0x02 (`RYN4_ADDRESS`)
   - 8 relays with hardware DELAY support
   - Poll interval: 1000ms (SET), 2000ms (READ)
   - Starts the `RYN4Proc` task (core 1)

3. **ANDRTF3** (Room Temperature)
   - Address: 0x04 (`ANDRTF3_ADDRESS`)
   - Single channel
   - Read interval: 5000ms (`ANDRTF3_SENSOR_READ_INTERVAL_MS`)

A short-lived background Modbus verification task (plain `xTaskCreate`) is also started here.

**Dependencies**: UART initialized, Logger, SharedResources
**RAM Impact**: ~3KB (device buffers, Modbus library)
**Failure Mode**: Critical - cannot operate without sensors/relays

---

### Stage 5: Network (Async)
**Function**: `initializeNetworkAsync()`
**Location**: `src/init/NetworkInitializer.cpp`

**Initializes (non-blocking):**
1. **Ethernet** (LAN8720A PHY, `EthernetManager::initializeAsync()`)
   - MDC: GPIO 23, MDIO: GPIO 18, clock: GPIO 17 output
   - Static IP: 192.168.20.40
   - Does not wait for link (`ETH_CONNECTION_TIMEOUT_MS` = 15000 is used only by the blocking `initializeBlocking()` variant)

2. **NetworkMonitor task** (plain `xTaskCreate`, 2048 bytes, priority 1)
   - Logs when the network connects

MQTT, NTP and OTA are not started here. They run as tasks created later (MQTTTask and OTATask in Stage 7, NTPTask in `main.cpp` after initialization). MQTT connects to `MQTT_SERVER`:1883 with client ID `esplan-<DEVICE_HOSTNAME>`.

**Dependencies**: Ethernet PHY, Logger, SharedResources
**RAM Impact**: ~4KB (network stack, MQTT buffers)
**Failure Mode**: Non-critical - system enters degraded mode

---

### Stage 6: Control Modules
**Function**: `initializeControlModules()`
**Location**: `src/init/SystemInitializer.cpp`

**Creates in order:**
1. **CentralizedFailsafe**
   - Emergency shutdown coordinator

2. **TemperatureSensorFallback**
   - Sensor validation and fallback logic

3. **FailOpenMonitor**

4. **BurnerRequestManager**

5. **HeatingControlModule**
   - Space heating control with weather compensation

6. **WheaterControlModule** (Water heating)
   - Tank heating with scheduling

7. **PIDControlModule**
   - PID temperature control

8. **BurnerSystemController**
   - Integrates all control logic
   - 8-state burner FSM
   - 4-layer safety system

**Dependencies**: Sensors initialized, Logger, SharedResources, SRP
**RAM Impact**: ~3KB (control state, PID coefficients)
**Failure Mode**: Critical - cannot control burner

---

### Stage 6b: MQTT
**Function**: `initializeMQTT()` (`InitStage::MQTT`, skipped only if `USE_EVENT_DRIVEN_MQTT` is defined)
**Location**: `src/init/SystemInitializer.cpp`

With `ENABLE_MQTT`, it only obtains the `MQTTManager` singleton. Configuration and the connection are handled by MQTTTask. Failure sets `DEGRADED_MODE` but does not stop initialization.

---

### Stage 7: FreeRTOS Tasks
**Function**: `initializeTasks()`
**Location**: `src/init/TaskInitializer.cpp`

Priorities, stack sizes and cores of all tasks: see the [Task Summary in TASK_ARCHITECTURE.md](TASK_ARCHITECTURE.md#task-summary).

**Creation order** (`TaskInitializer::initializeTasks()`):
1. RelayControl
2. OTATask
3. ANDRTF3 (only if the device is present)
4. ControlTask
5. HeatingControl
6. WheaterControl
7. BurnerControl
8. BoilerTempCtrl
9. MQTTTask (with `ENABLE_MQTT`)
10. PersistentStorage
11. SyslogTask
12. HeatingPump, WaterPump
13. Monitoring (with `ENABLE_MONITORING_TASK`)

MB8ART, MB8ARTProc and RYN4Proc already run from Stage 4. After `initializeSystem()` returns, `src/main.cpp` starts TimerSched and then NTPTask.

**Dependencies**: ALL previous stages complete
**RAM Impact**: 65,536 bytes of configured task stacks (DEBUG_SELECTIVE, 19 tasks)
**Failure Mode**: Task creation failures are logged; the system continues in degraded mode (a missing BurnerControl, RelayControl, MB8ART or MB8ARTProc task is logged as CRITICAL)

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

**Initialization is single-threaded only up to Stage 3**:
- Main thread runs `setup()` → `SystemInitializer::initializeSystem()`
- From Stage 4 on, other tasks already run: MB8ART, MB8ARTProc, RYN4Proc and the background Modbus verification task (Stage 4), and NetworkMonitor (Stage 5)
- Shared data those tasks touch must be accessed through SRP + mutexes from Stage 4 on

---

## 📊 Stack Budget

### DEBUG_SELECTIVE Mode (Default)
```
Total task stacks: 65,536 bytes (19 tasks)
Heap available:    ~280KB
Stack margins:     448-2568 bytes free (runtime measured)
Critical tasks:    +512 bytes safety margin (H3 optimization)
```

### RELEASE Mode (Production)
```
Total task stacks: 36,096 bytes (19 tasks, aggressive optimization)
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
