# Event Flow & System Logic

## Overview

This document maps the complete event flow for common scenarios in the boiler controller. Events propagate through multiple tasks and modules using FreeRTOS event groups.

## System Boot Sequence

### Phase 1: Hardware Initialization
```
main.cpp::setup()
├─ SystemInitializer::initialize()
├─ SharedResourceManager::createAll()
│  ├─ Creates 14 event groups
│  ├─ Creates 4 mutexes
│  └─ Sets initial state bits:
│     - SystemState::BOILER_ENABLED
│     - SystemState::HEATING_ENABLED
│     - SystemState::WATER_ENABLED
├─ Initialize I2C bus (GPIO 33/32)
├─ Initialize Modbus RTU (GPIO 36/4, 9600 baud)
└─ Initialize DS3231 RTC
   └─ Set system time from RTC
```

### Phase 2: Device Initialization
```
├─ MB8ART (0x03) - 8-channel temperature sensor
│  ├─ First read attempt
│  ├─ Set device ready bit
│  └─ Start MB8ARTProcessingTask
├─ RYN4 (0x02) - 4-channel relay module
│  ├─ Initialize all relays to OFF
│  ├─ Set device ready bit
│  └─ Start RYN4ProcessingTask
└─ ANDRTF3 (0x04) - Room temperature sensor
   ├─ Test connection
   ├─ First temperature read
   └─ Start ANDRTF3Task
```

### Phase 3: RuntimeStorage & Persistent Data
```
├─ Initialize FRAM (I2C 0x50, 32KB AT24C32)
│  └─ RuntimeStorage initialized
├─ Register with ServiceContainer
├─ Initialize CriticalDataStorage
│  ├─ Check for emergency state (magic + CRC)
│  └─ Log previous state if found
└─ Load schedules from FRAM
   └─ TimerSchedulerTask loads 2 schedules
```

### Phase 4: Network & Communication
```
├─ Start Ethernet (LAN8720 PHY)
│  ├─ PHY starts (1.5s)
│  ├─ Wait for DHCP (background)
│  └─ Sets ETH_CONNECTED event (async)
├─ Start MQTT Task (waits for network)
└─ Start OTA Task (waits for network)
```

### Phase 5: Control Tasks
```
├─ Start BurnerControlTask
│  ├─ Initialize BurnerStateMachine → IDLE
│  ├─ Initialize BurnerAntiFlapping
│  └─ Wait for sensor data ready
├─ Start HeatingControlTask
│  ├─ Initialize PID controller
│  ├─ Load PID state from FRAM (if valid)
│  └─ Register with watchdog (20s timeout)
├─ Start WheaterControlTask (Water)
│  ├─ Initialize water control module
│  └─ Register with watchdog (20s timeout)
├─ Start HeatingPumpControl
├─ Start WheaterPumpControl
└─ Start MonitoringTask
   └─ Begins health monitoring (5s interval)
```

### Phase 6: Network Services Connect
```
Network Connected Event
├─ MQTT connects to broker (192.168.16.16:1883)
│  ├─ Subscribes to command topics
│  ├─ Publishes online status
│  └─ Sets SystemState::MQTT_OPERATIONAL
├─ OTA service enabled
│  └─ Listening on port 3232
└─ NTP sync initiated
   └─ Updates RTC from network time
```

**Total Boot Time**: ~6-7 seconds

---

## Water Heating Request Flow

### Scenario: Morning Shower Schedule Activates

#### Step 1: Schedule Triggers (06:30)
```
TimerSchedulerTask (every 60s check)
├─ Detects schedule active
├─ Calls WaterHeatingScheduleAction::onScheduleStart()
└─ Sets target: 55°C, mode: SCHEDULE
```

#### Step 2: Water Control Evaluates
```
WheaterControlTask
├─ Receives schedule notification
├─ Reads current tank temperature via SRP::getSensorReadings()
│  └─ Tank: 25.2°C (Temperature_t = 252)
├─ Compares: 25.2°C < 55.0°C → NEEDS HEATING
├─ Calls BurnerRequestManager::setWaterRequest()
│  ├─ Encode 65°C in bits 16-23 (85°C burner target = 55°C + 10°C)
│  ├─ Set BurnerRequest::WATER
│  ├─ Set BurnerRequest::POWER_HIGH
│  └─ Set BurnerRequest::CHANGED | WATER_CHANGED
└─ Sets SystemState::WATER_ON
```

#### Step 3: Burner Control Responds
```
BurnerControlTask
├─ Wakes on BurnerRequest::CHANGED event
├─ Reads request bits via xEventGroupGetBits()
├─ Decodes temperature: (bits >> 16) & 0xFF = 65°C
├─ Checks TempSensorFallback::getOperationMode()
│  └─ Changes: BOTH → WATER_HEATING
├─ Runs safety checks
│  ├─ Pressure OK (1.50 BAR)
│  ├─ Temperature OK (25°C)
│  ├─ Pump will start
│  └─ All interlocks PASS
└─ BurnerStateMachine::setHeatDemand(true, 650)  // 65.0°C
   └─ Current state: IDLE → transition to PRE_PURGE
```

#### Step 4: Water Pump Starts
```
WheaterPumpControlTask
├─ Detects SystemState::WATER_ON bit
├─ Checks current pump state: OFF
├─ Sets RelayRequest::WATER_PUMP_ON
└─ RelayControlTask processes
   ├─ Calls RYN4::controlRelay(2, ON)
   ├─ Updates SharedRelayReadings::relayWhpump = true
   ├─ Sets SystemState::WATER_PUMP_ON
   └─ Increments water pump start counter
```

#### Step 5: Burner Ignition Sequence
```
BurnerStateMachine (10s PRE_PURGE)
└─ PRE_PURGE complete → IGNITION

BurnerStateMachine (IGNITION state)
├─ Enable ignition spark (not implemented)
├─ Open gas valve → Set relays
│  ├─ Relay 3 (Burner) → ON
│  ├─ Relay 5 (Water Mode) → ON
│  └─ Relay 4 (Power) → OFF (full power)
├─ Wait for flame detection
│  └─ Assumed TRUE (no flame sensor installed)
└─ IGNITION → RUNNING_HIGH (500ms after relays)
```

#### Step 6: Steady State Operation
```
RUNNING_HIGH state
├─ BurnerControlTask monitors every 100ms
│  ├─ Check flame (assumed OK)
│  ├─ Check safety (pressure, temp)
│  ├─ Check demand still present
│  └─ Feed watchdog
├─ MB8ARTTask reads temperatures every 2.5s
│  ├─ Boiler output temp increasing
│  ├─ Water tank temp rising
│  └─ Sets SensorUpdate::BOILER_OUTPUT event
├─ WheaterControlTask monitors progress
│  └─ Tank: 25.2°C → 35.5°C → 45.8°C → ...
└─ MQTTTask publishes sensors every 10s
   └─ Topic: boiler/status/sensors
      {"t":{"bo":654,"wt":458,...},"p":150,"r":21}
```

#### Step 7: Target Reached
```
WheaterControlTask
├─ Detects tank temp ≥ 55.0°C (setpoint)
├─ Calls BurnerRequestManager::clearWaterRequest()
│  ├─ Clear BurnerRequest::WATER
│  ├─ Set BurnerRequest::CHANGED
│  └─ Clear BurnerRequest::WATER_CHANGED
└─ Clears SystemState::WATER_ON
```

#### Step 8: Burner Shutdown
```
BurnerControlTask
├─ Wakes on BurnerRequest::CHANGED
├─ Reads requests: WATER bit cleared
├─ BurnerStateMachine::setHeatDemand(false)
└─ RUNNING_HIGH → POST_PURGE

BurnerStateMachine (POST_PURGE)
├─ Close gas valve (relays 3,4,5 → OFF)
├─ Keep fan running (if equipped)
├─ Wait 60 seconds
└─ POST_PURGE → IDLE
```

#### Step 9: Water Pump Stops
```
WheaterPumpControlTask
├─ Detects SystemState::WATER_ON cleared
├─ Sets RelayRequest::WATER_PUMP_OFF
└─ RelayControlTask
   ├─ RYN4::controlRelay(2, OFF)
   ├─ Updates SharedRelayReadings
   └─ Clears SystemState::WATER_PUMP_ON
```

**Total Cycle Time**: ~15-20 minutes (depends on water volume)

---

## Space Heating Request Flow

### Scenario: Room Temperature Drops Below Setpoint

#### Step 1: Temperature Monitoring
```
ANDRTF3Task (every 5s)
├─ Reads room sensor via Modbus
├─ Temperature: 19.5°C (Temperature_t = 195)
├─ Updates SharedSensorReadings::insideTemp
└─ Sets SensorUpdate::INSIDE event bit
```

#### Step 2: Heating Control Evaluates
```
HeatingControlTask
├─ Wakes on SensorUpdate::INSIDE event
├─ Reads current temp: 19.5°C
├─ Reads setpoint from SystemSettings: 21.0°C
├─ Calculates error: 21.0 - 19.5 = 1.5°C
├─ PID Controller runs
│  ├─ Error = 1.5°C → positive (need heat)
│  ├─ Output = 45.0 (scale 0-100)
│  └─ Decision: Request burner at MODERATE power
└─ Calls BurnerRequestManager::setHeatingRequest()
   ├─ Encode 70°C target (boiler temp)
   ├─ Set BurnerRequest::HEATING
   ├─ Set BurnerRequest::POWER_HIGH
   └─ Set BurnerRequest::HEATING_CHANGED | CHANGED
```

#### Step 3: Priority Arbitration
```
BurnerControlTask receives CHANGED event
├─ Check if water request also present
│  └─ Water has priority if SystemState::WATER_PRIORITY bit set
├─ If both requests:
│  └─ Process water first (DHW priority)
└─ If only heating:
   └─ Process heating request immediately
```

#### Step 4: Heating Pump Starts
```
HeatingPumpControlTask
├─ Detects SystemState::HEATING_ON
├─ Waits for burner to start
└─ After burner confirmed:
   └─ Starts heating pump (Relay 1 → ON)
```

#### Step 5: PID Control Loop
```
HeatingControlTask (every 1000ms)
├─ Read current boiler temp
├─ Calculate PID output
│  ├─ P term: Kp × error
│  ├─ I term: Ki × ∫error dt
│  ├─ D term: Kd × Δerror/Δt
│  └─ Output = P + I + D
├─ Translate output to power request
│  ├─ Output > 50 → HIGH_POWER
│  ├─ Output 20-50 → LOW_POWER
│  └─ Output < 20 → STOP
└─ Update burner request if changed
```

---

## Emergency Stop Flow

### Scenario: Pressure Drops Below 0.5 BAR

#### Step 1: Sensor Detects Low Pressure
```
MB8ARTTask (real sensor mode)
├─ Reads channel 7: 3.8mA current
├─ Convert to pressure: 0.35 BAR (Pressure_t = 35)
├─ Compare: 35 < ALARM_MIN (50)
│  └─ CRITICAL: Below minimum!
├─ Sets Burner::ERROR_PRESSURE event bit
└─ Sets SensorUpdate::PRESSURE_ERROR
```

#### Step 2: Safety Interlock Triggers
```
SafetyInterlocks::performFullSafetyCheck()
├─ Called by BurnerControl on every cycle
├─ Checks pressure: 0.35 BAR < 1.0 BAR minimum
│  └─ status.pressureInRange = FALSE
├─ Result: allInterlocksPassed() = FALSE
└─ Returns INTERLOCK_FAILED
```

#### Step 3: Burner Control Emergency Stop
```
BurnerControlTask
├─ Safety check fails
├─ Calls BurnerStateMachine::emergencyStop()
│  ├─ Current state: RUNNING_HIGH → ERROR
│  ├─ Immediately close gas valve (all relays OFF)
│  └─ Set SystemState::EMERGENCY_STOP | BURNER_ERROR
└─ Call CentralizedFailsafe::triggerEmergency()
```

#### Step 4: Emergency State Persistence
```
CentralizedFailsafe::triggerEmergency()
├─ Log critical error
├─ Save to FRAM via CriticalDataStorage
│  ├─ EmergencyState.reason = PRESSURE_LOW
│  ├─ EmergencyState.lastPressure = 35 (0.35 BAR)
│  ├─ EmergencyState.wasHeating = true
│  ├─ EmergencyState.timestamp = current millis
│  └─ Calculate CRC32 and write to FRAM
├─ Clear all burner requests
│  └─ BurnerRequest group cleared
└─ Notify all subsystems
   └─ Set Error::CRITICAL_FAILURE
```

#### Step 5: Subsystem Responses
```
All Control Tasks detect EMERGENCY_STOP
├─ HeatingControlTask
│  └─ Clears HEATING request, stops PID
├─ WheaterControlTask
│  └─ Clears WATER request
├─ HeatingPumpControl
│  └─ Stops heating pump (with delay for cooldown)
├─ WheaterPumpControl
│  └─ Stops water pump (immediate)
└─ MQTTTask
   └─ Publishes emergency alert
      Topic: boiler/status/emergency
      {"reason": "pressure_low", "pressure": 0.35}
```

#### Step 6: Error Indication
```
├─ SystemState::BURNER_ERROR bit remains set
├─ All attempts to start burner blocked
│  └─ checkSafetyConditions() returns FALSE
├─ Error logged with exponential backoff
│  └─ 1s → 2s → 4s → ... → 300s (5 min max)
└─ Waits for manual intervention
```

#### Step 7: Recovery (After Pressure Restored)
```
Operator fixes pressure leak
├─ Pressure rises to 1.5 BAR
├─ MB8ARTTask detects: 1.5 BAR > ALARM_MIN
│  └─ Clears Burner::ERROR_PRESSURE
├─ SafetyInterlocks::performFullSafetyCheck()
│  └─ All checks now PASS
└─ BurnerStateMachine::handleErrorState()
   ├─ Detects safety restored
   ├─ Clears SystemState::BURNER_ERROR
   └─ ERROR → IDLE (ready for operation)
```

---

## MQTT Command Processing Flow

### Scenario: User Changes Water Setpoint via MQTT

#### Step 1: MQTT Message Arrives
```
MQTT Broker publishes
└─ Topic: boiler/config/water_setpoint
   Payload: {"value": 60}
```

#### Step 2: MQTT Task Receives
```
MQTTTask::onMessage()
├─ Topic parsed: last segment = "water_setpoint"
├─ JSON parsed with ArduinoJson
│  └─ value = 60°C extracted
├─ Validates range (40-65°C for water)
└─ Queues parameter update request
```

#### Step 3: Persistent Storage Task Processes
```
PersistentStorageTask
├─ Receives update request from queue
├─ Takes SystemSettings mutex
├─ Updates settings.waterSetpoint = 600 (Temperature_t)
├─ Releases mutex
├─ Saves to NVS flash
│  └─ namespace: "boiler", key: "water_sp"
└─ Publishes confirmation
   Topic: boiler/config/response
   {"result": "ok", "param": "water_setpoint"}
```

#### Step 4: Water Control Responds
```
WheaterControlTask
├─ Detects SystemSettings changed (no specific event)
├─ Next cycle re-evaluates
├─ New setpoint: 60°C
├─ Current tank: 45°C
├─ Needs heating: 45°C < 60°C
└─ Updates burner request with new target
   └─ BurnerRequestManager updates target temp
```

---

## Sensor Data Flow

### Continuous Sensor Reading Cycle

#### MB8ART Temperature Sensors
```
MB8ARTTask (every 2.5 seconds)
├─ Coordinated via ModbusCoordinator
├─ Reads all 8 channels via Modbus
│  ├─ Ch1: Boiler output → 55.4°C (Temperature_t = 554)
│  ├─ Ch2: Boiler return → 48.2°C (Temperature_t = 482)
│  ├─ Ch3: Water tank → 42.1°C (Temperature_t = 421)
│  ├─ Ch4: Heating return → 35.6°C (Temperature_t = 356)
│  ├─ Ch5: Outside → 12.3°C (Temperature_t = 123)
│  ├─ Ch6: (unused)
│  ├─ Ch7: Pressure sensor → 1.52 BAR (Pressure_t = 152)
│  └─ Ch8: (unused)
├─ Takes SensorReadings mutex
├─ Updates SharedSensorReadings
│  ├─ .boilerTempOutput = 554
│  ├─ .wHeaterTempTank = 421
│  ├─ .systemPressure = 152
│  └─ .lastUpdateTimestamp = millis()
├─ Releases mutex
└─ Sets event bits for each sensor
   ├─ SensorUpdate::BOILER_OUTPUT
   ├─ SensorUpdate::WHEATER_TANK
   └─ SensorUpdate::PRESSURE
```

#### Event Propagation
```
Sensor Events Trigger Multiple Tasks:

SensorUpdate::BOILER_OUTPUT wakes:
├─ BurnerControlTask
│  └─ Checks if temp approaching target
├─ HeatingControlTask
│  └─ Runs PID loop with new measurement
└─ SafetyInterlocks
   └─ Verifies temp < MAX_LIMIT

SensorUpdate::PRESSURE wakes:
├─ BurnerControlTask
│  └─ Safety check includes pressure
├─ SafetyInterlocks
│  └─ Verifies 1.0 < pressure < 3.5 BAR
└─ (Future) PumpSpeedControl
   └─ Adjust pump based on pressure
```

---

## Scheduler Event Flow

### Timer Scheduler State Machine

```
TimerSchedulerTask (checks every 60 seconds)
├─ Get current time from DS3231
├─ For each enabled schedule:
│  ├─ Check if currently in time window
│  ├─ Check day of week matches
│  └─ Determine state change
│     ├─ NOT_ACTIVE → ACTIVE: onScheduleStart()
│     ├─ ACTIVE → NOT_ACTIVE: onScheduleEnd()
│     ├─ NOT_ACTIVE → PREHEATING: onPreheatingStart()
│     └─ NO_CHANGE: continue
└─ Process state changes
```

### Schedule Action Handlers

Each schedule type has custom logic:

#### WaterHeatingScheduleAction
```
onScheduleStart()
├─ Extract target temp from schedule
├─ Call WheaterControlModule::setTargetTemp()
├─ Set priority flag
└─ Publish MQTT event
   Topic: boiler/scheduler/event
   {"type": "start", "schedule": "Morning Shower"}

onScheduleEnd()
├─ Clear priority flag
├─ Revert to default water setpoint
└─ Publish MQTT event
```

#### SpaceHeatingScheduleAction
```
onScheduleStart()
├─ Determine mode (COMFORT/ECO/FROST)
├─ Apply temperature for mode:
│  ├─ COMFORT: 21.0°C (Temperature_t = 210)
│  ├─ ECO: 18.0°C (Temperature_t = 180)
│  └─ FROST: 10.0°C (Temperature_t = 100)
├─ Update SystemSettings::targetTemperatureInside
└─ Heating task picks up change automatically

onPreheatingStart() [called 3 hours before start]
├─ Calculate temperature rise needed
│  └─ Example: 15°C current → 21°C target = 6°C rise
├─ Estimate time: 6°C / 2°C per hour = 3 hours
├─ Start heating early if needed
└─ Set preheating flag
```

---

## Error Handling Flow

### Error Detection → Logging → Recovery

#### Typical Error Flow
```
1. Error Detected
   └─ Any task detects abnormal condition

2. Error Logged
   ├─ ErrorHandler::logError(errorCode)
   ├─ Checks rate limit (exponential backoff)
   ├─ Logs to serial
   └─ Appends to ErrorLogFRAM (circular buffer)

3. Error Propagated
   ├─ Set appropriate error event bit
   │  └─ Burner::ERROR_PRESSURE
   │  └─ SystemState::BURNER_ERROR
   └─ MQTT notification (if connected)

4. System Response
   ├─ BurnerControl checks error bits
   ├─ Decides action based on severity
   │  ├─ CRITICAL → Emergency stop
   │  ├─ WARNING → Degraded mode
   │  └─ INFO → Log only
   └─ Modifies operation accordingly

5. Recovery Attempt
   ├─ ErrorRecoveryManager evaluates
   ├─ Checks if error cleared
   ├─ Attempts automatic recovery
   │  └─ Max 3 attempts per error
   └─ If successful:
      ├─ Clear error bits
      └─ Resume normal operation
```

### Error Rate Limiting
```
ErrorHandler tracks errors per type:
├─ First occurrence: Log immediately
├─ Second: Log after 1 second
├─ Third: Log after 2 seconds
├─ Fourth: Log after 4 seconds
├─ ...exponential backoff...
└─ Max interval: 5 minutes

When error clears:
└─ Reset interval to 1 second
```

---

## Watchdog Monitoring Flow

### Task Health Monitoring

```
Each Task:
└─ Calls Watchdog::feed() on every iteration
   └─ Updates last-seen timestamp

MonitoringTask (every 5 seconds):
├─ Check all registered tasks
├─ For each task:
│  ├─ Calculate time since last feed
│  ├─ Compare to task timeout
│  └─ If exceeded:
│     ├─ Log warning
│     ├─ Set WARNING event bit
│     └─ (Future) Attempt task recovery
└─ Publish health metrics via MQTT
```

**Watchdog Timeouts**:
- BurnerControl: 15 seconds
- HeatingControl: 20 seconds
- WheaterControl: 20 seconds
- MQTT: 30 seconds
- Monitoring: 10 seconds

---

## Data Flow Summary

### Read-Only Data (No Mutex Needed)
```
Event Bits - Atomic read via xEventGroupGetBits()
SystemConstants - Compile-time constants
Configuration - Read-only after init
```

### Shared Data (Mutex Protected)
```
SharedSensorReadings
├─ Mutex: SRP::getSensorReadingsMutex()
├─ Writers: Sensor tasks (MB8ART, ANDRTF3)
└─ Readers: All control tasks

SharedRelayReadings
├─ Mutex: SRP::getRelayReadingsMutex()
├─ Writers: RelayControlTask
└─ Readers: Control tasks, MQTT

SystemSettings
├─ Mutex: SRP::getSystemSettingsMutex()
├─ Writers: PersistentStorageTask, MQTT
└─ Readers: Control tasks
```

### Queue-Based Communication
```
MQTT Publish Requests
├─ High Priority Queue (3 slots, 392 bytes each)
│  └─ Sensor data, critical alerts
└─ Normal Priority Queue (5 slots, 392 bytes each)
   └─ Status updates, config responses

Overflow Strategy:
└─ DROP_LOWEST_PRIORITY: Scan queue, drop lowest
```

---

## Timing Diagrams

### Typical Burner Start (from IDLE to RUNNING)
```
Time    Event
0s      Heat demand detected
0s      Safety checks PASS
0s      Anti-flapping check PASS
0s      State: IDLE → PRE_PURGE
0-10s   Pre-purge fan running, relays OFF
10s     State: PRE_PURGE → IGNITION
10s     Enable ignition, open gas valve
10.5s   Flame detected (or assumed)
10.5s   State: IGNITION → RUNNING_HIGH
10.5-?  Burner running at full power
```

### Typical Burner Stop (from RUNNING to IDLE)
```
Time    Event
0s      Heat demand removed (target reached)
0s      Check anti-flapping: ON time = 5 min > 2 min MIN ✓
0s      State: RUNNING_HIGH → POST_PURGE
0s      Close gas valve immediately
0-60s   Fan continues running (purge cycle)
60s     State: POST_PURGE → IDLE
60s     All burner activity stopped
```

### MQTT Sensor Publishing Cycle
```
Time    Task              Event
0s      MB8ARTTask        Read sensors → Update SharedSensorReadings
0s      MB8ARTTask        Set SensorUpdate::BOILER_OUTPUT
0-2.5s  (Other tasks process sensor data)
2.5s    MB8ARTTask        Next sensor read cycle
5s      ANDRTF3Task       Read room temp → Update SharedSensorReadings
5s      ANDRTF3Task       Set SensorUpdate::INSIDE
10s     MQTTTask timer    Publish sensor data to broker
10s     MQTTTask          Queue high-priority publish request
10-10.1s MQTT library     Transmit to broker
10.1s   Broker            Publish to subscribers
```

---

## Inter-Task Communication Patterns

### Pattern 1: Request-Acknowledge
```
Task A (Requester)
├─ Sets request bits: BurnerRequest::WATER
├─ Sets change bit: BurnerRequest::CHANGED
└─ Continues operation

Task B (Responder)
├─ Wakes on CHANGED event (pdTRUE clears bit)
├─ Reads current request state
├─ Processes request
└─ May set response/acknowledge bits
```

### Pattern 2: Publish-Subscribe
```
Publisher (Sensor Task)
├─ Updates shared data (mutex protected)
├─ Sets event bit: SensorUpdate::TEMPERATURE
└─ Releases mutex

Subscriber 1 (Control Task)
├─ Waits on event bit
├─ Wakes when bit set
├─ Reads shared data (mutex protected)
└─ Processes data

Subscriber 2 (Another Control Task)
└─ Also wakes on same event
   └─ Multiple tasks can wait on same event
```

### Pattern 3: State Notification
```
State Owner (Control Module)
├─ Changes internal state
├─ Updates SystemState bits
│  └─ Set: BURNER_ON
└─ Multiple observers react automatically

Observers
├─ Pump Control: Start pump when burner ON
├─ MQTT: Publish status update
└─ Monitoring: Track burner runtime
```

---

## Critical Event Sequences

### 1. Power Failure Recovery
```
Power Lost
└─ (No clean shutdown)

Power Restored
├─ ESP32 boots
├─ SystemInitializer runs
├─ CriticalDataStorage::init()
│  ├─ Reads emergency state from FRAM
│  ├─ Checks magic (0xDEADBEEF) + CRC
│  └─ If valid:
│     ├─ Log: "Previous emergency: PRESSURE_LOW at 17:32"
│     ├─ Log: "Boiler was at 65.2°C, Pressure 0.35 BAR"
│     └─ Decision: Do not auto-restart (pressure was critical)
└─ Waits in IDLE for manual start confirmation
```

### 2. Network Reconnection
```
Network Lost
├─ MQTTTask detects disconnect
├─ Sets SystemState::MQTT_OPERATIONAL = 0
└─ Begins reconnection attempts
   └─ Exponential backoff: 5s → 10s → 20s → ... → 60s max

Network Restored
├─ MQTT reconnects successfully
├─ Resubscribes to all topics
│  └─ boiler/cmd/+, boiler/config/+, boiler/cmd/scheduler/+
├─ Sets SystemState::MQTT_OPERATIONAL = 1
└─ Publishes retained online status
   Topic: boiler/status/online
   Payload: {"online": true}
   Retain: TRUE
```

### 3. Sensor Failure Handling
```
MB8ART Communication Timeout
├─ No response after 3 attempts (500ms each)
├─ Sets SensorUpdate::ERROR
├─ SharedSensorReadings marked invalid
│  └─ isBoilerTempOutputValid = false
└─ BurnerControl responds
   ├─ If burner RUNNING:
   │  └─ Continue with last known temp (short term)
   │  └─ If >5s no update: Emergency stop
   └─ If burner IDLE:
      └─ Block start until sensor restored
```

---

## Event Timing Characteristics

### Event Latency (from trigger to response)

| Scenario | Typical Latency | Max Latency |
|----------|-----------------|-------------|
| Burner request change | 50-100ms | 5000ms |
| Sensor update processed | 10-50ms | 1000ms |
| Emergency stop triggered | <10ms | 100ms |
| MQTT command processed | 100-500ms | 2000ms |
| Schedule activation | 0-60s | 60s |

### Task Wake-Up Times

```
Event Set → Task Wake → Handler Start
  ↓         ↓            ↓
  0ms       <1ms         <5ms (typical)
```

FreeRTOS guarantees task wake within one tick (1ms @ 1000Hz tick rate).

---

## Debugging Event Flow

### Enable Event Tracing
```cpp
// In any task
#define TRACE_EVENTS

EventBits_t bits = xEventGroupWaitBits(...);
LOG_DEBUG(TAG, "Events: 0x%06X Timeout: %s",
         bits, (bits == 0) ? "YES" : "NO");
```

### Monitor Event Group State
```cpp
// Get current bits without waiting
EventBits_t current = xEventGroupGetBits(eventGroup);

// Log all set bits
LOG_DEBUG(TAG, "SystemState: 0x%06X", current);
// Example output: 0x0000B3
// Binary: 10110011
// Bits set: 0,1,4,5,7 = BOILER_ENABLED, BOILER_ON, WATER_ENABLED, WATER_ON, BURNER_ON
```

### MQTT Event Monitoring
```bash
# Watch all boiler events in real-time
mosquitto_sub -h 192.168.16.16 -u YOUR_MQTT_USER -P pass -t "boiler/#" -v

# Common event topics:
boiler/status/burner          # Burner state changes
boiler/status/sensors         # Sensor updates
boiler/scheduler/event        # Schedule start/end
boiler/status/errors          # Error notifications
```

---

## Performance Metrics

### Event Processing Overhead

| Operation | CPU Time | Notes |
|-----------|----------|-------|
| xEventGroupSetBits() | <1µs | Single ARM instruction |
| xEventGroupGetBits() | <1µs | Direct memory read |
| xEventGroupWaitBits() | 0 (blocked) | Task sleeps, no CPU |
| Context switch | ~10µs | FreeRTOS overhead |

### Memory Usage

```
Event Group: 4 bytes (24 bits + control byte)
Total: 14 event groups × 4 bytes = 56 bytes

Mutexes: 96 bytes each (FreeRTOS structure)
Total: 4 mutexes × 96 bytes = 384 bytes

Queue: Variable (depends on item size × depth)
MQTT High: 392 bytes × 3 = 1176 bytes
MQTT Normal: 392 bytes × 5 = 1960 bytes
```

---

## Event Flow Best Practices

1. **Set Events After Data Updated**
   ```cpp
   // GOOD:
   updateData();
   setEventBit();

   // BAD:
   setEventBit();  // Readers wake before data ready!
   updateData();
   ```

2. **Use Appropriate Timeouts**
   ```cpp
   // Critical safety: Short timeout
   xEventGroupWaitBits(..., pdMS_TO_TICKS(100));

   // Normal operation: Moderate timeout
   xEventGroupWaitBits(..., pdMS_TO_TICKS(5000));
   ```

3. **Clear Change Bits**
   ```cpp
   // For change events, clear on read
   xEventGroupWaitBits(group, CHANGED, pdTRUE, ...);
                                      // ↑ Clear bit
   ```

4. **Check Multiple Events**
   ```cpp
   // Wait for ANY of several events
   EventBits_t mask = EVENT_A | EVENT_B | EVENT_C;
   EventBits_t bits = xEventGroupWaitBits(group, mask, ...);

   // Process each individually
   if (bits & EVENT_A) handleA();
   if (bits & EVENT_B) handleB();
   if (bits & EVENT_C) handleC();
   ```

5. **Atomic State Changes**
   ```cpp
   // Update multiple bits atomically
   xEventGroupClearBits(group, OLD_STATE_BITS);
   xEventGroupSetBits(group, NEW_STATE_BITS);
   ```
