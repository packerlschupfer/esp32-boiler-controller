# FreeRTOS Task Architecture

**Project**: ESP32 Boiler Controller
**Version**: 0.1.0
**Last Updated**: December 16, 2025

---

## Overview

The ESP32 Boiler Controller uses **18 active FreeRTOS tasks** managing boiler control, sensors, communication, and monitoring. The architecture is fully **event-driven** with zero polling loops for optimal power efficiency and responsive safety systems.

**Key Characteristics:**
- Event-driven architecture (no polling)
- Priority-based scheduling (1-4)
- Core pinning for time-critical tasks
- Comprehensive watchdog coverage
- Thread-safe shared resource access

---

## Task Summary

| Task | Priority | Stack (SEL) | Core | Purpose |
|------|----------|-------------|------|---------|
| BurnerControlTask | 4 | 3584 | Any | Burner state machine, safety-critical |
| RelayControlTask | 4 | 3584 | 1 | Physical relay control, motor protection |
| HeatingControlTask | 3 | 3584 | Any | Space heating PID control |
| WheaterControlTask | 3 | 3584 | Any | Water heating PID control |
| MB8ARTProcessingTask | 3 | 3072 | Any | Temperature sensor data processing |
| ANDRTF3Task | 3 | 3584 | Any | Room temperature sensor |
| SensorTask | 3 | 3584 | Any | Legacy sensor coordination |
| PIDControlTask | 3 | 4096 | Any | Future PID expansion (skeleton) |
| RYN4ProcessingTask | 3 | 2560 | Any | Relay module data processing |
| ControlTask | 3 | 3072 | Any | Remote control command handler |
| MQTTTask | 2 | 3584 | 1 | MQTT communication & queuing |
| MonitoringTask | 2 | 3584 | 0 | System health & diagnostics |
| HeatingPumpTask | 2 | 3072 | Any | Heating circulation pump control |
| WaterPumpTask | 2 | 3072 | Any | Hot water loading pump control |
| OTATask | 1 | 3072 | 1 | Over-the-air firmware updates |
| TimerSchedulerTask | - | 3072 | Any | Schedule management |
| PersistentStorageTask | - | 5120 | Any | NVS parameter persistence |
| NTPTask | - | default | Any | Time synchronization |

*Stack sizes shown for LOG_MODE_DEBUG_SELECTIVE (active development mode)*

---

## Detailed Task Documentation

### 1. BurnerControlTask ⚠️ SAFETY-CRITICAL

**File**: `src/modules/tasks/BurnerControlTask.cpp`
**Priority**: 4 (Highest)
**Stack**: 2560 (DEBUG_FULL) | 3584 (DEBUG_SELECTIVE) | 1536 (RELEASE)
**Core**: Not pinned (tskNO_AFFINITY)
**Watchdog**: 15000ms (WDT_BURNER_CONTROL_MS)

**Purpose**: Manages the burner state machine, enforces safety interlocks, coordinates heating/water demand, and handles emergency shutdown.

**Event Groups**:
- **Sensor Event Group** (waits): `BOILER_OUTPUT`, `BOILER_RETURN`, `WATER_TANK`, `FIRST_READ_COMPLETE`, `DATA_AVAILABLE`
- **Burner Event Group** (sets/waits): `STATE_TIMEOUT`, `FLAME_STATE_CHANGED`, `PRESSURE_CHANGED`, `FLOW_CHANGED`, `SAFETY_EVENT`
- **Burner Request Event Group** (waits): `HEATING`, `WATER`, `CHANGE_EVENT_BITS`, `TEMPERATURE_MASK`
- **System State Event Group** (sets): `BURNER_OFF`, `BURNER_HEATING_LOW/HIGH`, `BURNER_WATER_LOW/HIGH`, `BURNER_ERROR`
- **Control Requests Event Group** (waits): Various control commands

**Mutexes**:
- `SensorReadingsMutex` (via MutexRetryHelper)
- `BurnerSystemController` internal mutex

**State Machine**: See [STATE_MACHINES.md](STATE_MACHINES.md) for details

**Safety Features**:
- Continuous safety monitoring in IGNITION/RUNNING states
- Emergency stop with immediate relay shutdown
- Burner request expiration watchdog (10 minutes)
- Anti-flapping protection (minimum on-time 2 min, off-time 20s)

---

### 2. RelayControlTask ⚠️ SAFETY-CRITICAL

**File**: `src/modules/tasks/RelayControlTask.cpp`
**Priority**: 4 (Highest)
**Stack**: 2560 (DEBUG_FULL) | 3584 (DEBUG_SELECTIVE) | 1536 (RELEASE)
**Core**: 1 (pinned)
**Watchdog**: 15000ms (WDT_RELAY_CONTROL_MS)

**Purpose**: Controls 8 physical relays via RYN4 Modbus module, enforces pump motor protection, provides rate limiting, and monitors relay health.

**Event Groups**:
- **Relay Request Event Group** (waits): All relay request bits (pumps, burner, power, valve, alarm)
- **System State Event Group** (monitors): `HEATING_ON`, `WATER_ON`, `BURNER_ON`
- **Relay Status Event Group** (sets): `SYNCHRONIZED`, `COMM_OK`
- **Error Notification Event Group** (sets): `RELAY` on failures

**Mutexes**:
- `taskMutex` (task coordination)
- `relayStateMutex` (Round 20 Issue #6 - relay state protection)
- `RYN4` device mutex (via MutexRetryHelper)
- `RelayReadingsMutex` (shared state)

**Equipment Protection**:
- **Pump motor protection**: 30s minimum between state changes (prevents motor damage)
- **Relay rate limiting**: Prevents rapid toggling
- **Health monitoring**: Tracks relay failures and communication errors

**Relay Functions**:
1. Heating Pump (Relay 0)
2. Water Pump (Relay 1)
3. Burner Enable (Relay 2)
4. Water Mode (Relay 3)
5. Power Boost (Relay 2) - ON=full power, OFF=half power
6. Valve Control (Relay 5)
7. Alarm (Relay 6)
8. Reserved (Relay 7)

---

### 3. HeatingControlTask

**File**: `src/modules/tasks/HeatingControlTask.cpp`
**Priority**: 3
**Stack**: 3072 (DEBUG_FULL) | 3584 (DEBUG_SELECTIVE) | 2048 (RELEASE)
**Core**: Not pinned
**Watchdog**: Dynamic - max(sensorInterval * 4, WDT_HEATING_CONTROL_MS)

**Purpose**: Space heating control with PID, room temperature monitoring, burner request management, water priority coordination.

**Event Groups**:
- **System State Event Group** (checks/sets): `BOILER_ENABLED`, `HEATING_ENABLED`, `HEATING_ON`
- **Heating Event Group** (sets): `HeatingEvent::True/False`
- **Control Requests Event Group** (waits): `HEATING_ON_OVERRIDE`, `HEATING_OFF_OVERRIDE`, `WATER_PRIORITY_RELEASED`
- **Burner Request Event Group** (waits/checks): `WATER`
- **System State Event Group** (checks): `WATER_PRIORITY`

**Mutexes**:
- `SensorReadingsMutex` (via MutexRetryHelper)

**Control Logic**:
- PID control for room temperature
- Integrates with TimerSchedulerTask for scheduled heating
- Yields to water heating when priority flag set
- Anti-windup and derivative-on-PV improvements (Round 2)

---

### 4. WheaterControlTask (Water Heater)

**File**: `src/modules/tasks/WheaterControlTask.cpp`
**Priority**: 3
**Stack**: 3072 (DEBUG_FULL) | 3584 (DEBUG_SELECTIVE) | 2048 (RELEASE)
**Core**: Not pinned
**Watchdog**: 15000ms (WDT_WHEATER_CONTROL_MS)

**Purpose**: Water heating control, water pump management, PID control for tank temperature, heat recovery mode.

**Event Groups**:
- **System State Event Group** (checks/sets): `BOILER_ENABLED`, `WATER_ENABLED`, `WATER_ON`
- **Relay Event Group** (sets): `WATER_PUMP_ON`, `WATER_PUMP_OFF`
- **Control Requests Event Group** (waits/sets): `WATER_ON_OVERRIDE`, `WATER_OFF_OVERRIDE`, `WATER_PRIORITY_RELEASED`

**Mutexes**:
- `SensorReadingsMutex`

**Control Parameters** (configurable via MQTT):
- `tempLimitLow`: Start threshold (default 45.0°C)
- `tempLimitHigh`: Stop threshold (default 65.0°C)
- PID gains for fine control

**Modes**:
- **On-demand heating**: Activated when tank temp < tempLimitLow
- **Scheduled heating**: Via TimerSchedulerTask integration
- **Heat recovery**: Uses residual boiler heat after space heating

---

### 5. MQTTTask

**File**: `src/modules/tasks/MQTTTask.cpp`
**Priority**: 2
**Stack**: 3072 (DEBUG_FULL) | 3584 (DEBUG_SELECTIVE) | 1536 (RELEASE)
**Core**: 1 (pinned)
**Watchdog**: 30000ms (WDT_MQTT_TASK_MS)

**Purpose**: MQTT communication, sensor data publishing, health monitoring, command handling, priority-based message queuing with backpressure.

**Event Groups**:
- **MQTT Task Event Group** (internal): `CONNECTED`, `DISCONNECTED`, `MESSAGE`, `PUBLISH_SENSORS`, `PUBLISH_HEALTH`, `PROCESS_QUEUE`, `RETRY_SUBSCRIPTIONS`
- **System State Event Group** (sets): `MQTT_OPERATIONAL`
- **Error Notification Event Group** (sets): `NETWORK` on connection loss
- **General System Event Group** (sets): `MQTT_QUEUE_PRESSURE` (throttling signal)

**Queues**:
- **High priority queue**: 20 messages (sensor data, critical alerts)
- **Normal priority queue**: 40 messages (status updates)
- **Overflow strategy**: DROP_OLDEST with logging
- **Backpressure**: Sets `MQTT_QUEUE_PRESSURE` bit at 80% utilization

**Mutexes**:
- `mqttMutex_` (client operations)
- `SensorReadingsMutex` (read sensor data)
- `RelayReadingsMutex` (read relay states)

**Publishing Intervals**:
- Sensor data: 10s (balanced update rate)
- Health data: 60s (system monitoring)
- On-demand: Event-driven status updates

**Message Priorities**:
- `CRITICAL`: Bypasses queue under pressure
- `HIGH`: Never throttled
- `MEDIUM`: Throttled at 80% utilization
- `LOW`: Throttled at 50% utilization

---

### 6. MonitoringTask

**File**: `src/modules/tasks/MonitoringTask.cpp`
**Priority**: 2
**Stack**: 4096 (DEBUG_FULL) | 3584 (DEBUG_SELECTIVE) | 3584 (RELEASE)
**Core**: 0 (pinned)
**Watchdog**: Dynamic based on LOG_MODE

**Purpose**: System health monitoring, task stack analysis, error log dumping, periodic diagnostics.

**Event Groups**:
- **Monitoring Event Group** (internal): `HEALTH_CHECK`, `DETAILED`, `ON_DEMAND`, `CRITICAL`
- **Sensor Event Group** (checks): `FIRST_READ_COMPLETE`

**Mutexes**:
- `SensorReadingsMutex` (read sensor status)
- `RelayReadingsMutex` (read relay status)

**Monitoring Intervals**:
- **Health check**: 5s (basic heap, tasks, connectivity)
- **Detailed diagnostics**: 10 minutes (stack HWM, error logs)
- **MQTT health publish**: 60s (via MQTTTask)

**Diagnostics**:
- FreeRTOS task status (all 16 tasks)
- Stack high water mark analysis
- Heap fragmentation tracking
- Error log dumping (FRAM circular buffer)
- Sensor staleness detection

---

### 7. MB8ARTProcessingTask

**File**: `src/modules/tasks/MB8ARTProcessingTask.cpp`
**Priority**: 3
**Stack**: 3072 (DEBUG_FULL) | 3072 (DEBUG_SELECTIVE) | 1536 (RELEASE)
**Core**: Not pinned
**Watchdog**: 15000ms (WDT_SENSOR_PROCESSING_MS)

**Purpose**: Process Modbus packets from MB8ART 8-channel temperature sensor, update SharedSensorReadings.

**Sensor Channels**:
1. Boiler Output Temperature (critical)
2. Boiler Return Temperature
3. Water Tank Temperature
4. Water Output Temperature
5. Water Return Temperature
6. Heating Return Temperature
7. Pressure Sensor (4-20mA, 0.01 BAR precision)
8. Exhaust Temperature

**Event Groups**: None (MB8ART v1.2+ handles internally)

**Queues**: MB8ART internal queue (library manages)

**Mutexes**: MB8ART internal mutex

**Reading Interval**: 2.5s (critical boiler temperatures)

**Safety**:
- Staleness detection (15s threshold)
- Modbus error detection (0xFFFF, 0x0000, 0x7530)
- Invalid temperature range filtering

---

### 8. ANDRTF3Task

**File**: `src/modules/tasks/ANDRTF3Task.cpp`
**Priority**: 3
**Stack**: 2048 (DEBUG_FULL) | 3584 (DEBUG_SELECTIVE) | 1024 (RELEASE)
**Core**: Not pinned
**Watchdog**: 20000ms (ANDRTF3_SENSOR_READ_INTERVAL_MS * 4)

**Purpose**: Read room temperature from ANDRTF3 Modbus sensor via ModbusCoordinator.

**Event Groups**:
- **Sensor Event Group** (sets): `INSIDE`, `INSIDE_ERROR`

**Queues**: Uses FreeRTOS task notifications from ModbusCoordinator

**Mutexes**:
- `SensorReadingsMutex`

**Reading Interval**: 5s (room temperature changes slowly)

**Features**:
- Temperature compensation offset (configurable via MQTT)
- Direct read fallback on coordinator timeout (30s)
- Error recovery with exponential backoff

---

### 9. TimerSchedulerTask

**File**: `src/modules/tasks/TimerSchedulerTask.cpp`
**Priority**: Not explicitly defined (likely default)
**Stack**: 3072 bytes
**Core**: Not pinned
**Watchdog**: Disabled

**Purpose**: Generic timer scheduler for water/space heating schedules, FRAM persistence, MQTT command interface.

**Event Groups**:
- **Scheduler Event Group** (internal): `CHECK_SCHEDULE`, `PUBLISH_STATUS`, `SCHEDULE_CHANGED`, `SAVE_SCHEDULES`

**Mutexes**:
- `schedulesMutex` (Round 14 Issue #5 - schedule array protection)

**Persistence**: RuntimeStorage (FRAM) with CRC32 validation

**MQTT API**:
- `boiler/cmd/scheduler/add` - Add schedule
- `boiler/cmd/scheduler/remove` - Remove schedule (by ID)
- `boiler/cmd/scheduler/list` - List all schedules
- `boiler/status/scheduler/info` - Active schedule status

**Schedule Types**:
- **Water heating**: Timed tank heating sessions
- **Space heating**: Room temperature schedule (future)

**Limits**: MAX_SCHEDULES = 16 (enforced in Round 2)

---

### 10. PersistentStorageTask

**File**: `src/modules/tasks/PersistentStorageTask.cpp`
**Priority**: Not explicitly defined
**Stack**: 5120 bytes (large for JSON operations)
**Core**: Not pinned
**Watchdog**: Not registered (event-driven)

**Purpose**: NVS parameter persistence, MQTT parameter API, temperature offset management, error logging to FRAM.

**Event Groups**:
- **Storage Event Group** (internal): `SAVE_REQUEST`, `LOAD_REQUEST`, `MQTT_RECONNECT`
- **System State Event Group** (checks): `MQTT_OPERATIONAL`
- **General System Event Group** (sets): `STORAGE_READY`
- **Control Requests Event Group** (sets): `SAVE_PARAMETERS`

**Queues**: PersistentStorage internal command queue

**MQTT Parameter API**:
```
boiler/params/wheater/tempLimitLow       - Water start threshold (45.0°C)
boiler/params/wheater/tempLimitHigh      - Water stop threshold (65.0°C)
boiler/params/heating/hysteresis         - Space heating hysteresis (0.5°C)
boiler/params/heating/setpoint           - Comfort temperature (21.0°C)
boiler/params/pid/spaceHeating/kp        - PID proportional gain
boiler/params/pid/spaceHeating/ki        - PID integral gain
boiler/params/pid/spaceHeating/kd        - PID derivative gain
boiler/params/get/all                    - Request all parameters
boiler/params/save                       - Save all to NVS
boiler/params/save/changed               - Save only changed parameters
```

**NVS Recovery**: Automatic namespace erase on corruption (Round 7)

---

### 11. NTPTask

**File**: `src/modules/tasks/NTPTask.cpp`
**Priority**: Not explicitly defined
**Stack**: Default
**Core**: Not pinned
**Watchdog**: 60000ms

**Purpose**: NTP time synchronization, DS3231 RTC fallback, timezone handling, drift tracking.

**Event Groups**:
- **NTP Event Group** (internal): `SYNC_NOW`, `NETWORK_READY`, `LOG_STATUS`, `UPDATE_CONFIG`, `SYNC_FAILED`
- **General System Event Group** (sets): `TIME_SYNCED`

**Mutexes**:
- `ntpMutex`

**NTP Servers** (priority order):
1. 192.168.20.1 (local gateway)
2. pool.ntp.org
3. time.google.com
4. time.cloudflare.com

**Sync Intervals**:
- Initial sync: On network ready
- Auto sync: 6 hours (21600000ms)
- Retry: 30s after failure

**RTC Fallback**: Switches to DS3231 after 5 consecutive NTP failures

**Timezone**: Configurable via TZ environment variable (default: CET-1CEST,M3.5.0,M10.5.0/3)

---

### 12. OTATask

**File**: `src/modules/tasks/OTATask.cpp`
**Priority**: 1 (Lowest)
**Stack**: 2048 (DEBUG_FULL) | 3072 (DEBUG_SELECTIVE) | 1024 (RELEASE)
**Core**: 1 (pinned)
**Watchdog**: Disabled (not registered)

**Purpose**: Over-the-air firmware updates via ArduinoOTA, network state monitoring.

**Event Groups**:
- **OTA Event Group** (internal): `NETWORK_CONNECTED`, `NETWORK_DISCONNECTED`, `CHECK_UPDATE`, `UPDATE_STARTED`, `UPDATE_COMPLETED`, `UPDATE_ERROR`
- **System State Event Group** (checks): `NETWORK_READY`

**Mutexes**:
- `otaStatusMutex`

**Features**:
- Automatic update checks (30s interval)
- Password-protected updates
- Progress tracking via MQTT
- Safe rollback on failure

**Configuration**:
- Port: 3232
- Password: Defined in ProjectConfig.h

---

### 13. HeatingPumpTask

**File**: `src/modules/control/PumpControlModule.cpp`
**Priority**: 2 (Medium)
**Stack**: 2560 (DEBUG_FULL) | 3072 (DEBUG_SELECTIVE) | 1024 (RELEASE)
**Core**: Any (not pinned)
**Watchdog**: Disabled

**Purpose**: Controls heating circulation pump independently of burner state machine. Provides clean separation between burner operation and pump control, with motor protection enforced at the relay layer.

**Event Groups**:
- **System State Event Group** (waits): `HEATING_ON` - Heating mode active
- **System State Event Group** (sets): `HEATING_PUMP_ON` - Pump state indicator
- **Relay Request Event Group** (requests): `HEATING_PUMP_ON`, `HEATING_PUMP_OFF`

**Architecture**:
Uses unified `PumpControlModule` with parameterized configuration for code reuse.

**Operation Sequence**:
1. Wait for `HEATING_ON` event bit (heating mode activated)
2. Issue relay request via `RelayRequest::HEATING_PUMP_ON`
3. Set state indicator `HEATING_PUMP_ON`
4. Increment FRAM counter for pump starts
5. Monitor for `HEATING_ON` clear (heating mode deactivated)
6. Issue relay request via `RelayRequest::HEATING_PUMP_OFF`
7. Clear state indicator

**Motor Protection**:
- Enforced by RelayControlTask (not PumpControlTask)
- Configurable via MQTT: `boiler/cmd/config/pump_protection_ms` (5000-60000ms, default 15000ms)
- Prevents mechanical damage from rapid cycling

**Decoupling Benefits**:
- Burner state machine doesn't manage pump timing
- Simplified burner logic (no pump state tracking)
- Pump control testable independently
- Motor protection centralized in one location

**FRAM Integration**:
- Tracks pump start count for maintenance scheduling
- Counter type: `rtstorage::CounterType::HEATING_PUMP_STARTS`

---

### 14. WaterPumpTask

**File**: `src/modules/control/PumpControlModule.cpp`
**Priority**: 2 (Medium)
**Stack**: 2560 (DEBUG_FULL) | 3072 (DEBUG_SELECTIVE) | 1024 (RELEASE)
**Core**: Any (not pinned)
**Watchdog**: Disabled

**Purpose**: Controls hot water tank loading pump during water heating cycles. Operates independently from burner, allowing pump to run before/after burner as needed for heat transfer.

**Event Groups**:
- **System State Event Group** (waits): `WATER_ON` - Water heating mode active
- **System State Event Group** (sets): `WATER_PUMP_ON` - Pump state indicator
- **Relay Request Event Group** (requests): `WATER_PUMP_ON`, `WATER_PUMP_OFF`

**Architecture**:
Uses same unified `PumpControlModule` with water-specific configuration.

**Operation**: Same event-driven pattern as HeatingPumpTask but for water heating mode

**Tank Loading Strategy**:
- Pump activates with `WATER_ON` event
- Burner modulates power (HALF/FULL) while pump circulates
- Pump may continue briefly after burner stops (heat transfer)
- Motor protection enforced by RelayControlTask

**FRAM Integration**:
- Tracks pump start count for wear monitoring
- Counter type: `rtstorage::CounterType::WATER_PUMP_STARTS`

**Related**: See `src/modules/control/PumpControlModule.h` for unified implementation

---

### 15. SensorTask (Legacy)

**Purpose**: Minimal task - just waits for sensor data events (actual processing in MB8ARTProcessingTask)

---

### 16. PIDControlTask (Skeleton)

**Purpose**: Future PID control expansion - currently just logs and delays

---

### 17. RYN4ProcessingTask

**Purpose**: Process Modbus packets from RYN4 relay controller via ModbusCoordinator scheduling

**File**: `src/modules/tasks/RYN4ProcessingTask.cpp`
**Priority**: 3
**Stack**: 2560 bytes
**Core**: Not pinned
**Watchdog**: WDT_SENSOR_PROCESSING_MS (15s)

**Architecture**: Coordinated Modbus operations with relay verification

**Tick Schedule** (via ModbusCoordinator):
- **SET tick** (ticks 1, 6): Write pending relay changes via `setMultipleRelayStates()`
- **READ tick** (ticks 3, 8): Verify relay states via `readBitmapStatus()`
- **Timing**: SET → READ = 1000ms (2 coordinator ticks × 500ms)

**Relay State Tracking**:
- Uses shared `RelayState` structure (`src/shared/RelayState.h`)
- `desired`: Target relay states (bitmask)
- `actual`: Last verified physical states
- `pendingWrite`: Flag for write scheduling
- `consecutiveMismatches`: Counter for retry logic
- `delayMask`: Tracks relays with active hardware DELAY timers
- `delayExpiry[]`: Timestamp for DELAY expiration

**Verification Logic**:
1. **DELAY-Aware Verification**: Skip relays with active hardware DELAY countdown
   ```cpp
   uint8_t mismatchMask = actual ^ desired;
   uint8_t realMismatch = mismatchMask & ~delayMask;
   if (realMismatch == 0) {
       // All mismatches from DELAY - expected!
       return;
   }
   ```

2. **Mismatch Counting**: Distinguishes timing issues from real failures
   - First mismatch: DEBUG log (silent retry)
   - Second+ mismatch: ERROR log (persistent problem)
   - Reset counter on successful verification

3. **Auto-Retry**: Persistent mismatches re-queue write on next SET tick

**Hardware DELAY Support**:
- RYN4 module supports hardware timer (0x06XX commands)
- Physical relay state differs from desired during countdown
- Verification must skip DELAY relays to avoid false positives
- See `docs/EQUIPMENT_SPECS.md` for DELAY command details

**Event Groups**:
- **Relay Status Event Group** (sets): `SYNCHRONIZED`, `COMM_OK`, `COMM_ERROR`
- **Relay Control Event Group** (sets): `DATA_AVAILABLE`

**Mutexes**:
- `RelayReadingsMutex` (updates SharedRelayReadings for MQTT)
- `RelayState.delayMutex` (protects delayExpiry timestamps)

#### ControlTask
**Purpose**: Remote control command processing, system enable state management via StateManager

---

## Priority Hierarchy

### Priority 4 (Safety-Critical)
Highest priority tasks that must never be blocked:
- **BurnerControlTask**: State machine, safety interlocks
- **RelayControlTask**: Physical hardware control

### Priority 3 (Control Logic)
Normal control tasks with sensor coordination:
- HeatingControlTask, WheaterControlTask
- MB8ARTProcessingTask, ANDRTF3Task
- SensorTask, PIDControlTask, RYN4ProcessingTask, ControlTask

### Priority 2 (Non-Critical Communication & Pump Control)
- **MQTTTask**: Network communication (can tolerate delays)
- **MonitoringTask**: Diagnostics and logging
- **HeatingPumpTask**: Heating circulation pump control
- **WaterPumpTask**: Hot water loading pump control

### Priority 1 (Lowest)
- **OTATask**: Firmware updates (background operation)

---

## Core Affinity

### Core 0 (Default)
Most tasks run here unless explicitly pinned:
- BurnerControlTask, HeatingControlTask, WheaterControlTask
- MB8ARTProcessingTask, ANDRTF3Task, SensorTask
- PIDControlTask, RYN4ProcessingTask, ControlTask
- HeatingPumpTask, WaterPumpTask
- TimerSchedulerTask, PersistentStorageTask, NTPTask
- MonitoringTask (explicitly pinned to Core 0)

### Core 1 (Explicitly Pinned)
Time-critical communication tasks:
- **RelayControlTask**: Deterministic relay control timing
- **MQTTTask**: Isolate network I/O
- **OTATask**: Isolate firmware update operations

**Rationale**: Pinning communication tasks to Core 1 prevents interference with critical control loops on Core 0.

---

## Inter-Task Communication

### Event-Driven Architecture
All tasks use FreeRTOS event groups for synchronization - **NO polling loops**.

Benefits:
- Zero CPU usage when idle (power efficiency)
- Instant response to events (safety)
- Simplified code (no polling logic)

### Key Event Groups

| Event Group | Tasks Using | Purpose |
|-------------|-------------|---------|
| Sensor Event Group | Burner, Heating, Wheater, ANDRTF3, Sensor | Temperature updates |
| Burner Event Group | BurnerControl | State machine events |
| Burner Request Event Group | BurnerControl, Heating, Wheater | Demand coordination |
| System State Event Group | All control tasks, Pump tasks | System-wide enable states |
| Control Requests Event Group | ControlTask, Heating, Wheater | MQTT commands, overrides |
| Relay Event Group | RelayControl, Wheater | Relay requests |
| Task-Specific Groups | MQTT, NTP, OTA, Monitoring, Storage, Scheduler | Internal events |

See [EVENT_GROUPS.md](EVENT_GROUPS.md) for complete event bit definitions.

### Queue Usage

**MQTT Queues** (priority-based):
- High priority: 20 messages (sensor data, critical alerts)
- Normal priority: 40 messages (status updates)
- Overflow: DROP_OLDEST with throttling

**Modbus Queues** (internal):
- MB8ART library queue
- RYN4 library queue

**Storage Queue**:
- PersistentStorage command queue

---

## Safety Features

### Watchdog Protection

**Critical Tasks** (system reset on timeout):
- BurnerControlTask (15s)
- RelayControlTask (15s)

**Non-Critical Tasks** (logged only):
- All others with appropriate timeouts

**Disabled Watchdog**:
- OTATask (firmware update in progress)
- PIDControlTask (skeleton)
- TimerSchedulerTask (event-driven)
- PersistentStorageTask (NVS operations)

### Equipment Protection

**Pump Motor Protection**:
- 30s minimum between state changes (RelayControlTask)
- Prevents mechanical damage from rapid cycling

**Burner Anti-Flapping**:
- Minimum on-time: 2 minutes
- Minimum off-time: 20 seconds
- Prevents fuel waste and equipment wear

**Relay Rate Limiting**:
- Prevents rapid toggling
- Tracks toggle rate per relay

**Burner Request Expiration**:
- 10-minute watchdog
- Prevents stuck-on conditions

### Sensor Safety

**Staleness Detection**:
- Sensor data older than 15s blocks burner operation
- Enforced in BurnerControlTask safety checks

**Hysteresis** (DISABLED):
- Sensors are stable and accurate in this installation
- Previously: 3 valid start / 2 invalid stop (archived Round 4)
- Code structure preserved for future use

**Fallback Modes**:
- ANDRTF3: Direct read fallback on coordinator timeout
- NTP: DS3231 RTC fallback after 5 failures
- Graceful degradation when sensors fail

---

## Memory Optimization

### Stack Size Tuning (December 2025)

Based on runtime profiling showing only 60-100 bytes free:

**Increased** (safety margin):
- PIDControlTask: 3072 → 4096 bytes (was showing 60 bytes free)
- MonitoringTask: 3072 → 3584 bytes (task status allocation)

**Optimized** (based on HWM profiling):
- MONITORING_TASK: 4096 → 3584 (had 2232 free)
- RELAY_CONTROL_TASK: 4096 → 3584 (had 2032 free)
- SENSOR_TASK: 4096 → 3584 (had 2272 free)
- WHEATER_CONTROL_TASK: 4096 → 3584 (had 2344 free)
- MB8ART_PROCESSING_TASK: 4096 → 3072 (had 2568 free)

**Conservative** (debug modes):
- DEBUG_FULL: Larger stacks for extensive logging
- DEBUG_SELECTIVE: Optimized based on profiling
- RELEASE: Aggressive optimization

### Total Stack Memory by Mode

| Mode | Total Stack | Notes |
|------|-------------|-------|
| DEBUG_FULL | ~53 KB | Conservative for development |
| DEBUG_SELECTIVE | ~57 KB | Optimized for active development |
| RELEASE | ~28 KB | Aggressive optimization for production |

### Memory Savings (Rounds 1-7)

| Optimization | Savings |
|--------------|---------|
| Stack tuning (Round 2) | 4.5 KB |
| Float→Fixed-point | 1 KB+ heap |
| String→char[] buffers | 200+ bytes heap |
| Buffer size reductions | 1 KB |
| **Total** | **~6.7 KB+ recovered** |

---

## Task Lifecycle

### Initialization Order

1. **System Init**: Event group creation, shared resource allocation
2. **Core Services**: SharedResourceManager, ServiceContainer
3. **Hardware Tasks**: MB8ARTProcessingTask, RYN4ProcessingTask
4. **Control Tasks**: BurnerControlTask, HeatingControlTask, WheaterControlTask
5. **Communication Tasks**: MQTTTask, NTPTask, OTATask
6. **Monitoring Tasks**: MonitoringTask
7. **Auxiliary Tasks**: TimerSchedulerTask, PersistentStorageTask, ControlTask

### Startup Dependencies

**BurnerControlTask** waits for:
- Sensor Event Group: `FIRST_READ_COMPLETE`
- Valid sensor data in SharedSensorReadings

**MQTTTask** waits for:
- Network connection (Ethernet or WiFi)
- Broker connection (192.168.20.27:1883)

**TimerSchedulerTask** waits for:
- RuntimeStorage initialization
- RTC/NTP time sync

**PersistentStorageTask** waits for:
- NVS partition available
- MQTT connection for parameter API

### Shutdown/Cleanup

All tasks register cleanup handlers via `TaskCleanupHandler`:
- Timer deletion
- Mutex release
- Queue cleanup
- Event group clear

**Emergency Shutdown**:
1. BurnerSystemController emergencyShutdown()
2. RelayControlTask kills all relays
3. CriticalDataStorage saves emergency state to FRAM
4. System reset or safe idle state

---

## Performance Characteristics

### CPU Utilization (typical)

| Task | CPU % | Notes |
|------|-------|-------|
| IDLE | ~85% | Event-driven architecture = low CPU usage |
| BurnerControlTask | <1% | Only active during state transitions |
| HeatingControlTask | <1% | PID calculations every 10s |
| MQTTTask | 2-5% | Network I/O and JSON serialization |
| MB8ARTProcessingTask | 1-2% | Modbus processing every 2.5s |
| MonitoringTask | <1% | Periodic diagnostics |
| Other tasks | <1% each | Event-driven, minimal CPU |

### Memory Footprint (DEBUG_SELECTIVE)

| Resource | Usage | Total Available |
|----------|-------|-----------------|
| Heap | ~66-70 KB free | ~120 KB |
| Stack | ~57 KB allocated | ~80 KB available |
| FRAM | ~25 KB used | 32 KB (MB85RC256V) |
| NVS | ~4 KB | 20 KB partition |

**Fragmentation**: 23-28% (acceptable for FreeRTOS heap_4)

---

## Troubleshooting

### Common Issues

**Stack Overflow**:
- Symptom: Task resets, corruption, crashes
- Detection: Stack HWM < 100 bytes (logged by MonitoringTask)
- Solution: Increase stack size in ProjectConfig.h

**Mutex Deadlock**:
- Symptom: Tasks block indefinitely
- Detection: Watchdog timeout
- Prevention: Follow mutex hierarchy (see MUTEX_HIERARCHY.md)
- Recovery: System reset via watchdog

**Queue Overflow**:
- Symptom: Message loss, `MQTT_QUEUE_PRESSURE` bit set
- Detection: QueueManager logging
- Solution: Throttle non-critical messages

**Event Group Saturation**:
- Symptom: Missed events, spurious triggers
- Detection: Event bit conflicts logged
- Solution: Clear event groups on startup (Round 3)

### Debug Tools

**Stack Analysis**:
```cpp
MonitoringTask logs task status every 10 minutes
Check log for "Stack HWM" values
```

**Heap Monitoring**:
```cpp
LOG_INFO shows free heap in health checks
Watch for fragmentation >30%
```

**MQTT Diagnostics**:
```bash
mosquitto_sub -h 192.168.20.27 -u YOUR_MQTT_USER -P <pass> -t "boiler/status/health" -v
```

**Runtime Profiling**:
```cpp
Enable DEBUG_TASK_STATISTICS in ProjectConfig.h
Check CPU utilization per task
```

---

## Future Improvements

### Planned Enhancements

1. **Dynamic Priority Adjustment**
   - Boost HeatingControlTask priority when PID active
   - Reduce MQTTTask priority during critical operations

2. **Advanced Scheduling**
   - Space heating schedules (currently only water)
   - Multi-zone control
   - Weather compensation

3. **Predictive Control**
   - Machine learning for occupancy prediction
   - Adaptive PID tuning based on thermal mass
   - Load balancing between heating and water

4. **Enhanced Diagnostics**
   - Task execution timing histograms
   - Event group utilization tracking
   - Mutex contention analysis

5. **Remote Configuration**
   - OTA configuration updates
   - A/B testing for control parameters
   - Live PID tuning via MQTT

---

## References

- [STATE_MACHINES.md](STATE_MACHINES.md) - Burner and heating state machines
- [EVENT_GROUPS.md](EVENT_GROUPS.md) - Complete event bit reference
- [MUTEX_HIERARCHY.md](MUTEX_HIERARCHY.md) - Deadlock prevention
- [SAFETY_SYSTEM.md](SAFETY_SYSTEM.md) - Safety layer documentation
- [MQTT_API.md](MQTT_API.md) - Complete MQTT topic reference
- [DEEP_CODE_ANALYSIS_HISTORY.md](DEEP_CODE_ANALYSIS_HISTORY.md) - Analysis rounds 1-21

---

**Document Status**: ✅ Complete
**Validation**: All information extracted from source code (December 4, 2025)
**Next Review**: After significant architectural changes
