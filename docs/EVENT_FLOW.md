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
├─ MQTT connects to broker (MQTT_SERVER, 192.168.20.27:1883 in the production build)
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
├─ Checks WaterChargePolicy::limitsValid(tempLimitLow, tempLimitHigh)
│  └─ low >= high → no charge, one WARN "Water limits inconsistent"
├─ WaterChargePolicy::nextChargeNeeded(): 25.2°C < tempLimitLow → NEEDS HEATING
├─ Calls BurnerRequestManager::setWaterRequest()
│  ├─ Boiler target = tank + wHeaterConfTempChargeDelta = 25.2°C + 10°C = 35.2°C,
│  │  clamped to water_heating_low_limit..high_limit (default 40-90°C) → 40°C
│  │  (setWaterRequest() also clamps to 20-110°C)
│  ├─ Encode 40°C in bits 16-23
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
├─ Decodes temperature: (bits >> 16) & 0xFF = 40°C
├─ Checks TempSensorFallback::getOperationMode()
│  └─ Changes: NONE → WATER_HEATING
├─ Runs safety checks (sensor staleness, BurnerSafetyValidator)
│  ├─ Pressure OK (1.50 BAR)
│  ├─ Temperature OK (25°C)
│  ├─ All interlocks PASS
│  └─ Validation passed → clears Error::SAFETY
├─ Publishes BurnerDemandGate permission (getBurnerDemandPermission)
└─ BurnerDemandGate::controlTaskMayArm()
   ├─ Boiler 25°C < target 40°C → arm now
   │  └─ BurnerStateMachine::setHeatDemand(true, 400, highPower)
   └─ Boiler already above target → not armed; BoilerTempControlTask
      arms on a later cycle when its PID wants heat

BurnerStateMachine (IDLE)
└─ Heat demand + WATER_ON + WATER request + safety OK
   + minimum off-time over → PRE_PURGE
   (heat demand without an active mode request is ignored)
```

#### Step 4: Water Pump Starts
```
WaterPumpTask (PumpControlModule, every 500ms)
├─ Detects SystemState::WATER_ON bit (with BOILER_ENABLED)
├─ Checks current pump state: OFF
├─ Sets RelayRequest::WATER_PUMP_ON and SystemState::WATER_PUMP_ON
├─ Increments water pump start counter
└─ RelayControlTask switches Relay 6 (WATER_PUMP) ON
   (WheaterControlTask already set RelayRequest::WATER_PUMP_ON at charge start)
```

#### Step 5: Burner Ignition Sequence
```
BurnerStateMachine (PRE_PURGE, PRE_PURGE_TIME_MS = 2s)
├─ WATER_ON/WATER request or heat demand withdrawn → abort to IDLE
└─ PRE_PURGE complete → IGNITION

BurnerStateMachine (IGNITION state)
├─ Enable ignition spark (not implemented)
├─ Open gas valve → Set relays
│  ├─ Relay 1 (BURNER_ENABLE) → OFF
│  ├─ Relay 3 (WATER_MODE) → ON
│  └─ Relay 2 (POWER_BOOST) → ON (full power; OFF = half power)
├─ Wait BURNER_MIN_IGNITION_TIME_MS (3s), then check flame
│  └─ Assumed TRUE (no flame sensor installed)
├─ IGNITION → RUNNING_HIGH (RUNNING_LOW if high power not allowed)
└─ No flame within IGNITION_TIME_MS (5s) → retry via PRE_PURGE
   └─ 3rd failed attempt (MAX_IGNITION_RETRIES) → LOCKOUT
```

#### Step 6: Steady State Operation
```
RUNNING_HIGH state
├─ BurnerControlTask monitors every 100ms
│  ├─ Check flame (assumed OK)
│  ├─ Check safety (pressure, temp)
│  ├─ Check demand still present
│  └─ Feed watchdog
├─ BoilerTempControlTask on every SensorUpdate::BOILER_OUTPUT (~2.5s)
│  ├─ PID power level OFF/HALF/FULL for the request target
│  ├─ Publishes its decision (getBoilerTempDecision)
│  └─ BurnerDemandGate::decide(): ARM / SET_POWER / DISARM every cycle
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
├─ Detects tank temp > tempLimitHigh (charge latch released)
├─ Calls BurnerRequestManager::clearRequest(RequestSource::WATER)
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
├─ Withdraws BurnerDemandGate permission
└─ BurnerStateMachine::setHeatDemand(false)

BurnerStateMachine (RUNNING_HIGH)
├─ Demand ended → POST_PURGE once the minimum on-time allows
└─ No active mode request (WATER_ON + WATER request) for 10s
   → POST_PURGE without waiting for the minimum on-time

BurnerStateMachine (POST_PURGE)
├─ BurnerSystemController::deactivate(): burner relays OFF
│  (Relay 1 BURNER_ENABLE, Relay 2 POWER_BOOST, Relay 3 WATER_MODE)
├─ Keep fan running (if equipped)
├─ Heat demand + active mode request return → PRE_PURGE
│  (same conditions as from IDLE, minimum off-time applies)
├─ Wait SafetyConfig::postPurgeMs (default 90s)
└─ POST_PURGE → IDLE
```

#### Step 9: Water Pump Stops
```
WaterPumpTask (PumpControlModule)
├─ Detects SystemState::WATER_ON cleared → pump overrun
│  (SystemSettings::pumpCooldownMs, default 5 min)
├─ After the overrun: sets RelayRequest::WATER_PUMP_OFF,
│  clears SystemState::WATER_PUMP_ON
└─ RelayControlTask switches Relay 6 (WATER_PUMP) OFF
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
├─ Wakes on its 5s process timer
│  (or a HEATING_ON/OFF_OVERRIDE / WATER_PRIORITY_RELEASED control event)
├─ BOILER_ENABLED + HEATING_ENABLED, checkIfSpaceHeatingNeededEvent()
├─ Water priority check, sensors available (TemperatureSensorFallback)
├─ HeatingControlModule::calculateSpaceHeatingTargetTemp() (no PID here)
│  ├─ Heating curve from outside temperature
│  └─ Weather-compensated mode: curve shifted by the room deviation
│     (target 21.0°C - room 19.5°C) × roomTempCurveShiftFactor
└─ Calls BurnerRequestManager::setHeatingRequest(target, false)
   ├─ Clamp to heating_low_limit..heating_high_limit (and 20-110°C)
   ├─ Encode target (e.g. 70°C) in bits 16-23
   ├─ Set BurnerRequest::HEATING
   ├─ Set BurnerRequest::POWER_LOW
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
HeatingPumpTask (PumpControlModule, every 500ms)
├─ Detects SystemState::HEATING_ON (with BOILER_ENABLED)
├─ Does not wait for the burner
└─ Sets RelayRequest::HEATING_PUMP_ON → Relay 5 (HEATING_PUMP) ON
```

#### Step 5: PID Control Loop
```
BoilerTempControlTask (on every SensorUpdate::BOILER_OUTPUT, ~2.5s)
├─ Reads boiler output temp and the target of the active request
├─ BoilerTempController::calculate() (PID, gains pid/spaceHeating/*)
│  ├─ P term: Kp × error
│  ├─ I term: Ki × ∫error dt (anti-windup)
│  ├─ D term: Kd × Δerror/Δt
│  └─ Output = 50 + adjustment/10, clamped 0-100 (50 = at target)
├─ Maps output to power level with hysteresis
│  ├─ OFF → HALF above 55 (FULL above 75)
│  ├─ HALF/FULL → OFF below 35
│  ├─ HALF → FULL above 75
│  └─ FULL → HALF below 65
├─ BurnerAntiFlapping may keep the previous level
└─ BurnerDemandGate::decide(): ARM / SET_POWER / DISARM
   └─ BurnerStateMachine::setHeatDemand()

HeatingControlTask (5s process timer)
└─ Recalculates the heating curve target and updates the heating request
```

---

## Emergency Stop Flow

### Scenario: Pressure Drops Below 0.5 BAR

#### Step 1: Sensor Detects Low Pressure
```
MB8ARTTask (real sensor mode, USE_REAL_PRESSURE_SENSOR in ProjectConfig.h)
├─ Reads the pressure channel (SensorIndex::PRESSURE_CHANNEL): 5.12 mA
├─ Convert 4-20 mA to 0-5 BAR: 0.35 BAR (Pressure_t = 35, plus pressure offset)
│  (below 3.5 mA or above 20.5 mA = sensor fault: pressure invalid,
│   SensorUpdate::PRESSURE_ERROR instead of the steps below)
├─ Sets SensorUpdate::PRESSURE (valid reading)
├─ Compare: 35 < Safety::Pressure::ALARM_MIN (50 = 0.50 BAR)
│  └─ LOG_WARN "Pressure alarm"
└─ Sets Burner::ERROR_PRESSURE event bit
   (no firmware code reads this bit; the burner reacts through Step 2)
```

#### Step 2: Safety Interlock Triggers
```
SafetyInterlocks::continuousSafetyMonitor()
├─ Called by BurnerStateMachine::update() in IGNITION/RUNNING_LOW/RUNNING_HIGH
├─ Every 5 s (FULL_CHECK_INTERVAL_MS), while HEATING_ON or WATER_ON is set:
│  performFullSafetyCheck(isWaterMode)
├─ Checks pressure: 0.35 BAR < MIN_OPERATING (1.00 BAR)
│  └─ status.pressureInRange = FALSE
├─ Result: allInterlocksPassed() = FALSE
└─ continuousSafetyMonitor() returns FALSE
```

#### Step 3: Burner Control Emergency Stop
```
BurnerStateMachine::update() (BurnerControlTask)
├─ continuousSafetyMonitor() returns FALSE in IGNITION/RUNNING_LOW/RUNNING_HIGH
├─ Calls BurnerStateMachine::emergencyStop()
│  ├─ BurnerSystemController::emergencyShutdown(): burner relays OFF
│  │  (BURNER_ENABLE, POWER_BOOST, WATER_MODE, rate limit bypassed)
│  ├─ Clears SystemState::BURNER_ON
│  └─ Current state: RUNNING_HIGH → ERROR (onEnterError() calls
│     emergencyShutdown() again and records the ERROR entry time)
├─ SystemState::EMERGENCY_STOP is NOT set on this path
├─ SystemState::BURNER_ERROR is set by BurnerControlTask's burner state bit
│  update (ERROR and LOCKOUT map to BURNER_ERROR)
└─ CentralizedFailsafe::emergencyStop() is not called on this path
   (see Step 4 for the triggers that reach it)
```

#### Step 4: Coordinated Emergency Stop (other triggers)
```
CentralizedFailsafe::emergencyStop(reason)
(reached via SafetyInterlocks::triggerEmergencyShutdown(): critical temperature
 >= 115°C, stale sensor data during operation, burner request watchdog;
 and via the EMERGENCY failsafe level)
├─ Log "EMERGENCY STOP: <reason>", failsafe level EMERGENCY
├─ BurnerSystemController::emergencyShutdown(): burner relays OFF
├─ Relays 1-3 OFF again via setRelayState() (redundant)
├─ Relay 5 (HEATING_PUMP) and Relay 6 (WATER_PUMP) forced ON via
│  setRelayStateEmergency() for heat dissipation
├─ Set SystemState::EMERGENCY_STOP
├─ Clear SystemState::BOILER_ENABLED, notifyWheaterTaskSwitchedOff()
└─ ErrorHandler::logError(SYSTEM_FAILSAFE_TRIGGERED, reason)
   (the emergency state is written to FRAM by saveEmergencyState() when the
    failsafe level first reaches CRITICAL or higher, see SAFETY_SYSTEM.md)
```

#### Step 5: Subsystem Responses
```
All Control Tasks detect EMERGENCY_STOP
├─ HeatingControlTask
│  └─ Clears HEATING request, stops PID
├─ WheaterControlTask
│  └─ Clears WATER request
│     (CentralizedFailsafe::emergencyStop() clears BOILER_ENABLED and calls
│      notifyWheaterTaskSwitchedOff(): the charge ends on the next run and
│      does not resume after re-enable unless tank < tempLimitLow)
├─ HeatingPumpTask / WaterPumpTask (PumpControlModule)
│  └─ Pumps follow HEATING_ON / WATER_ON (with overrun); the
│     emergency shutdown does not switch the pump relays
└─ MQTTTask
   └─ Publishes emergency alert
      Topic: boiler/status/emergency
      {"reason": "pressure_low", "pressure": 0.35}
```

#### Step 6: Error Indication
```
├─ SystemState::BURNER_ERROR set while the burner is in ERROR
├─ No burner start while in ERROR
│  (checkSafetyConditions() calls BurnerSystemController::performSafetyCheck():
│   EMERGENCY_STOP bit, temperature limits, critical error bits
│   SENSOR_FAILURE/MODBUS/RELAY; it does not check pressure)
└─ Log output only: LOG_WARN from MB8ARTTask ("Pressure alarm", every read)
   and SafetyInterlocks ("System pressure ... out of range"), LOG_ERROR from
   BurnerStateMachine ("Safety interlock failed during operation!").
   This path does not call ErrorHandler::logError(), so its exponential
   backoff (per error code, 1 s doubling to 300 s) does not apply.
```

#### Step 7: Recovery

Recovery: see [STATE_MACHINES.md](STATE_MACHINES.md).

---

## Burner Demand Arming Flow

Two tasks write `BurnerStateMachine::setHeatDemand()`. `BurnerDemandGate` (`include/modules/control/BurnerDemandGate.h`) keeps them consistent, so a request that starts while the boiler is already above target does not ignite.

```
BurnerControlTask::updateBurnerState() (request start/change)
├─ Blocks: return preheating, stale sensors, failed validation,
│  sensor fallback cannot continue, boiler disabled
├─ publishDemandPermission(permitted, maxTargetTemp, highPowerAllowed)
├─ Not permitted → setHeatDemand(false)
└─ Permitted → BurnerDemandGate::controlTaskMayArm()
   ├─ No valid boiler temperature → arm (sensor fallback operation)
   ├─ Fresh decision (≤ DECISION_MAX_AGE_MS 6s) for target ±1°C → follow it
   └─ Otherwise → arm only if boiler output < target

BoilerTempControlTask (each cycle with an active request)
├─ controller.updateMode() → active mode gains from SystemSettings
├─ Target capped by Permission.maxTargetTemp (sensor fallback)
├─ controller.calculate() → OFF/HALF/FULL, publishDecision()
└─ BurnerDemandGate::decide(permitted, pidWantsHeat, changed, armed)
   ├─ ARM       → BurnerSafetyValidator, setHeatDemand(true, target, power)
   ├─ SET_POWER → BurnerSafetyValidator, update power level
   ├─ DISARM    → setHeatDemand(false)
   │              (re-armed while coasting, or permission withdrawn)
   └─ NONE

BurnerControlTask main loop
└─ Request present but not permitted → processBurnerRequest() every 10s
   (change events fire only when the request bits change)
```

---

## Water Heating Switch-Off Flow

```
StateManager::setBoilerEnabled(false)
StateManager::setWaterEnabled(false)
StateManager::setWaterOverrideOff(true)
CentralizedFailsafe::emergencyStop()   (clears BOILER_ENABLED)
└─ notifyWheaterTaskSwitchedOff()
   ├─ Increments the switch-off counter
   └─ xTaskNotifyGive(WheaterControlTask)

WheaterControlTask::processWaterHeatingState() (next run)
├─ Counter changed → clear charge latch (lastHeatingNeeded = false)
├─ Charge running → end it, even if water is already enabled again
│  ├─ Clear WATER_ON (water pump follows it via PumpControlModule overrun)
│  ├─ BurnerRequestManager::clearRequest(RequestSource::WATER)
│  ├─ Set ControlRequest::WATER_PRIORITY_RELEASED
│  └─ Log: "Water heating switched off - ending charge"
└─ While boiler/water disabled or waterOverrideOff set:
   latch cleared on every run

After re-enable
└─ New charge only when tank < tempLimitLow
   (heating preemption and sensor loss keep the latch → charge resumes)
```

---

## MQTT Command Processing Flow

### Scenario: User Changes the Water Tank Stop Limit via MQTT

`boiler/config/+` is subscribed but only logged; settings are changed through the PersistentStorage parameter topics (`boiler/params/...`).

#### Step 1: MQTT Message Arrives
```
MQTT Broker publishes
└─ Topic: boiler/params/set/wheater/tempLimitHigh
   Payload: 600 (tenths of °C = 60.0°C)
```

#### Step 2: Parameter Command Queued
```
PersistentStorage::handleMqttCommand()
(subscription boiler/params/#, set up by PersistentStorageTask)
├─ Strips the prefix "boiler/params/"
├─ "set/" → SET command for parameter wheater/tempLimitHigh
└─ Queues the command (dropped with a warning if the queue is full)
```

#### Step 3: Persistent Storage Task Processes
```
PersistentStorage (queued command processing)
├─ setJson(): range check of the registered parameter (500-850)
├─ Updates SystemSettings::wHeaterConfTempLimitHigh = 600
└─ Publishes the updated parameter value
   (persist with boiler/params/save, see MQTT_API.md)
```

#### Step 4: Water Control Responds
```
WheaterControlTask
├─ No specific event; the next run snapshots
│  wHeaterConfTempLimitLow / wHeaterConfTempLimitHigh
├─ WaterChargePolicy::limitsValid(): low < high required
└─ WaterChargePolicy::nextChargeNeeded() uses the new stop limit
   ├─ Charge running: ends when tank > 60.0°C
   └─ No charge: a new one still starts only below tempLimitLow
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
├─ High Priority Queue (3 slots, sizeof(MQTTPublishRequest))
│  └─ Sensor data, critical alerts
└─ Normal Priority Queue (5 slots)
   └─ Status updates, config responses

Overflow Strategy:
└─ DROP_OLDEST (both queues): the oldest queued message is dropped
```

---

## Timing Diagrams

### Typical Burner Start (from IDLE to RUNNING)
```
Time    Event
0s      Heat demand armed, active mode request present
0s      Safety checks PASS
0s      Anti-flapping check PASS
0s      State: IDLE → PRE_PURGE
0-2s    Pre-purge (PRE_PURGE_TIME_MS), relays OFF
2s      State: PRE_PURGE → IGNITION
2s      Enable ignition, open gas valve
5s      Flame checked after BURNER_MIN_IGNITION_TIME_MS (assumed)
5s      State: IGNITION → RUNNING_HIGH
5-?     Burner running at full power
```

No flame within `IGNITION_TIME_MS` (5s in IGNITION): retry via PRE_PURGE; the 3rd failed attempt (`MAX_IGNITION_RETRIES`) goes to LOCKOUT. The IGNITION StateMachine timeout (`IGNITION_TIME_MS + IGNITION_BACKSTOP_MARGIN_MS` = 7s) is only a backstop.

### Typical Burner Stop (from RUNNING to IDLE)
```
Time    Event
0s      Heat demand removed (target reached)
0s      Check anti-flapping: ON time = 5 min > 2 min MIN ✓
0s      State: RUNNING_HIGH → POST_PURGE
0s      Close gas valve immediately
0-90s   Post-purge (SafetyConfig::postPurgeMs, default 90s)
90s     State: POST_PURGE → IDLE
90s     All burner activity stopped
```

If heat demand and an active mode request return during post-purge, POST_PURGE → PRE_PURGE once the minimum off-time is over. Explicit disable of the running mode or the boiler, and 10s without an active mode request, go to POST_PURGE without waiting for the minimum on-time.

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

### 4. Explicit Disable While Burner Runs
```
MQTT/UI: heating, water or boiler disable
├─ StateManager clears HEATING_ENABLED / WATER_ENABLED / BOILER_ENABLED
└─ BurnerStateMachine (RUNNING_LOW/HIGH, next tick)
   ├─ BurnerTransitionPolicy::stopForExplicitDisable()
   │  └─ Running mode (from BurnerSystemController) disabled, or boiler disabled
   ├─ → POST_PURGE immediately, minimum on-time bypassed
   │  Log: "Space heating disabled - stopping burner now (minimum on-time bypassed)"
   └─ Disabling the mode that is not running does not stop the burner

POST_PURGE never restarts a mode that was just disabled
```

### 5. Water → Heating Handover (MODE_SWITCHING)
```
RUNNING (water mode), WATER_ON cleared, HEATING_ON set
├─ Safety OK and flame → MODE_SWITCHING
└─ Otherwise → POST_PURGE

MODE_SWITCHING
├─ Heating request present → BurnerSystemController::switchMode() → RUNNING_LOW/HIGH
├─ No heating request yet
│  ├─ heatingLikelyWanted() (enabled, not overridden, weather or room rule)
│  │  → wait, at most MODE_SWITCH_MAX_WAIT_MS (15s)
│  └─ Otherwise → POST_PURGE
├─ Demand points back to the running mode
│  ├─ Its ON bit set → RUNNING_LOW
│  └─ ON bit not set → wait (max 15s), then POST_PURGE
└─ StateMachine timeout MODE_SWITCH_HARD_TIMEOUT_MS (30s) → POST_PURGE
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
mosquitto_sub -h 192.168.20.27 -u YOUR_MQTT_USER -P pass -t "boiler/#" -v

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
