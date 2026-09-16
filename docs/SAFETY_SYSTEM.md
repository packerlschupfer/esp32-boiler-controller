# Safety System Architecture

## Overview

The boiler controller implements a **4-layer safety architecture**: three software layers (BurnerSafetyValidator, SafetyInterlocks, CentralizedFailsafe) and the hardware DELAY watchdog of the RYN4 relay module. The burner state machine runs the software checks in its states (see Burner State Machine Integration). Hardware interlocks are not implemented (see below).

**Design Philosophy**: Defense in depth - multiple independent safety checks ensure that failure of one layer does not compromise overall system safety.

---

## Safety Layers

### Layer 1: BurnerSafetyValidator (Pre-operation Validation)

**Purpose**: Validate all safety conditions BEFORE allowing burner operations.

**Location**: `src/modules/control/BurnerSafetyValidator.cpp`

**Validation Steps** (`validateBurnerOperation()`, 6 checks in this order):
1. **Emergency stop** - `EMERGENCY_STOP` bit must be clear
2. **Sensor validity** - At least 2 of boiler output, boiler return and water tank valid and within sensor range; all count as invalid if the MB8ART data is older than `SafetyConfig::sensorStaleMs`
3. **Temperature limits** - Boiler output below `maxBoilerTemp` in heating and water mode: 110.0°C (`MAX_BOILER_TEMP_C`) for both callers - BurnerControlTask passes the limit from `TemperatureSensorFallback::getSafeOperatingParams()` (110.0°C in NORMAL mode), BoilerTempControlTask uses the `SafetyConfig` default. Only in water mode the tank must also be below `wHeaterConfTempSafeLimitHigh` (`maxWaterTemp`)
4. **Pressure bounds** - 1.00-3.50 BAR; without a valid pressure reading the burner is blocked unless built with `ALLOW_NO_PRESSURE_SENSOR`
5. **Hardware interlocks** - Stub, always passes
6. **Thermal shock** - Boiler output more than 35.0°C above return (`ReturnPreheat::MAX_DIFFERENTIAL`) -> `THERMAL_SHOCK_RISK`

There is no runtime limit: the former 1 h continuous / 4 h daily check read counters that were never updated and was removed.

Not checked here: pumps (the pump check was removed; the burner requires an active mode request instead) and anti-flapping (minimum on-time 120 s and off-time 20 s are enforced by the burner state machine, power level changes by `BurnerAntiFlapping` in BoilerTempController).

**Runtime-Configurable Parameters**:
- **Sensor Staleness Timeout**: 30-300s (default: 60s)
  - Prevents operation with stale sensor data
  - Adjustable via MQTT: `boiler/cmd/config/sensor_stale_ms`

**Fixed Safety Limits** (not configurable):
- Boiler output limit: 110.0°C (`MAX_BOILER_TEMP_C`, `SafetyConfig::maxBoilerTemp` default; BurnerControlTask request check and BoilerTempControlTask demand arming)
- Minimum pressure: 1.00 BAR
- Maximum pressure: 3.50 BAR
- Thermal shock differential for this check: 35.0°C (`ReturnPreheat::MAX_DIFFERENTIAL`). The periodic full check in SafetyInterlocks uses the configurable `SafetyConfig::thermalShockDifferentialC` instead (default 45.0°C, see Runtime-Configurable Safety Parameters)

**Result**: Returns a `ValidationResult` (`SAFE_TO_OPERATE` or the failure reason). On a failure BurnerControlTask (`updateBurnerState()`) always withdraws the heat demand and then acts on the result:

| Result | Cause | BurnerControlTask action |
|--------|-------|--------------------------|
| `EMERGENCY_STOP_ACTIVE` | `EMERGENCY_STOP` bit set | `BurnerStateMachine::emergencyStop()` -> ERROR |
| `INSUFFICIENT_SENSORS` | Fewer than 2 valid sensors, or MB8ART data older than `SafetyConfig::sensorStaleMs` | `emergencyStop()` -> ERROR |
| `TEMPERATURE_EXCEEDED` | Boiler output at or above the limit, or (water mode) tank at or above `maxWaterTemp` | `emergencyStop()` -> ERROR |
| `PRESSURE_EXCEEDED` | Pressure below 1.00 or above 3.50 BAR | `emergencyStop()` -> ERROR |
| `HARDWARE_INTERLOCK_OPEN` | Not returned (stub) | `emergencyStop()` -> ERROR |
| `SENSOR_FAILURE` | No valid pressure reading (build without `ALLOW_NO_PRESSURE_SENSOR`) | Demand blocked only |
| `THERMAL_SHOCK_RISK` | Boiler output more than 35.0°C above return | Demand blocked, return preheating started |
| `PUMP_FAILURE` | Not returned (pump check removed) | Demand blocked only |

`emergencyStop()` switches the burner relays off and holds the burner in ERROR for `SafetyConfig::errorRecoveryMs` (default 300000 ms = 5 min); after that it returns to IDLE once the safety check passes. It does not set the `EMERGENCY_STOP` latch. BoilerTempControlTask runs the same validation before arming the demand, but on a failure it only blocks the demand (and starts return preheating on `THERMAL_SHOCK_RISK`); it never calls `emergencyStop()`.

---

### Layer 2: SafetyInterlocks (Continuous Monitoring)

**Purpose**: Monitor safety conditions continuously during operation and trigger immediate shutdown if unsafe conditions develop.

**Location**: `src/modules/control/SafetyInterlocks.cpp`

**Monitoring Checks** (`continuousSafetyMonitor()`, skipped while `BOILER_ENABLED` is clear):
1. **Emergency stop** - Every call
2. **Critical temperature** - Boiler output at or above 115.0°C (`CRITICAL_BOILER_TEMP_C`) -> `triggerEmergencyShutdown()`; every call
3. **Sensor staleness** - Boiler output channel stale (`StateManager::isSensorStale()`) -> `triggerEmergencyShutdown()`; every call
4. **Full check** - `performFullSafetyCheck()` every 5 s (`FULL_CHECK_INTERVAL_MS`) while HEATING_ON or WATER_ON is set: emergency stop, critical error bits (SENSOR_FAILURE - set by the sensor fallback only while the boiler output reading is missing -, MODBUS, RELAY), at least 2 valid and fresh sensors, boiler output and return below 110.0°C, thermal shock (`SafetyConfig::thermalShockDifferentialC`), sensor and relay communication, pressure 1.00-3.50 BAR (skipped without a valid pressure reading)

Pump verification was removed (Round 18/19): SafetyInterlocks does not check the pumps.

**Key Difference from Layer 1**:
- Layer 1: Pre-operation validation (gate-keeping)
- Layer 2: Continuous monitoring (real-time protection)

**Action on Failure**: `SafetyInterlocks::continuousSafetyMonitor()` runs on every burner state machine update during IGNITION, RUNNING_LOW and RUNNING_HIGH. A failure calls `BurnerStateMachine::emergencyStop()`: burner relays off, state ERROR.

**Runtime-Configurable Parameters**:
- **Sensor Staleness Timeout**: Same as Layer 1 (shares SafetyConfig)
- **Pump Protection Delay**: 5-60s (default: 15s)
  - Prevents rapid pump cycling (motor protection)
  - Adjustable via MQTT: `boiler/cmd/config/pump_protection_ms`

---

### Layer 3: CentralizedFailsafe (Coordinated Emergency Shutdown)

**Purpose**: Coordinate safe shutdown across all subsystems when safety violation detected.

**Location**: `src/modules/control/CentralizedFailsafe.cpp`

**Shutdown Paths**:

| Function | Burner relays (1-3) | Pumps | Further actions |
|----------|---------------------|-------|-----------------|
| `BurnerSystemController::emergencyShutdown()` | OFF via `RelayControlTask::setRelayStateEmergency()` (rate limiting bypassed) | Not commanded - PumpControlModule keeps running them while HEATING_ON/WATER_ON are set | Relay failure sets `Error::RELAY` and `Error::SAFETY` bits |
| `BurnerStateMachine::emergencyStop()` | OFF via `emergencyShutdown()` | Not commanded | Clears `BURNER_ON`, state ERROR |
| `CentralizedFailsafe::emergencyStop()` | OFF via `emergencyShutdown()` | Both forced ON via `setRelayStateEmergency()` for heat dissipation; PumpControlModule keeps them on until the boiler output is below 60.0°C (again from 65.0°C, always without a valid, fresh reading), also after a release; below that it switches them off (re-sent until the relay follows) | Sets `EMERGENCY_STOP`, clears `BOILER_ENABLED`, logs the error |

The burner never switches pumps off: the former `setAllRelays(false)` in `emergencyShutdown()` also stopped the pumps, and PumpControlModule only writes on its own state changes, so they stayed off with the heat exchanger still hot.

`CentralizedFailsafe::emergencyStop()` is reached through `SafetyInterlocks::triggerEmergencyShutdown()` (stale sensor data during operation, critical temperature, burner request watchdog) and the EMERGENCY failsafe level. The FRAM emergency record (`CriticalDataStorage`) is written when the failsafe level first reaches CRITICAL or higher: by `triggerFailsafe()` (`saveEmergencyState()`) and, since 2026-09-15, by `emergencyStop()` itself (error code `EMERGENCY_STOP`), so direct stops (critical temperature, stale sensors, request watchdog) leave a record too.

**Emergency Stop Latch**: `EMERGENCY_STOP` stays set until released. BurnerControlTask reads it without clearing and stops the burner once per onset; while it is set the burner stays in ERROR (safety check fails), and `boiler/cmd/system` `on` alone does not restart it. A reboot clears it (event bits are not persisted; the FRAM emergency record is only logged at boot).

**Emergency Stop Release**: `EMERGENCY_STOP` is cleared by `TemperatureSensorFallback` when the sensors return from SHUTDOWN to NORMAL (`CentralizedFailsafe::releaseAfterSensorRecovery()`), but only if stale sensor data caused the stop (`EmergencyStopRelease::Cause::SENSOR_STALE`; a second trigger with another cause while latched makes it non-releasable this way). `BOILER_ENABLED` is not restored on this path. Otherwise it is cleared by the MQTT command `boiler/cmd/emergency_reset` with payload `reset`, which calls `CentralizedFailsafe::clearEmergencyStop()`. The checks, in this order (rules in `include/modules/control/EmergencyStopRelease.h`):
1. `EMERGENCY_STOP` set, else `emergency_not_active`
2. Boiler output valid and below 110.0°C (`MAX_BOILER_TEMP_C`), boiler return below it too if valid, else `emergency_release_refused:temperature_high`. The readings are checked directly, not through `SafetyInterlocks::checkTemperatureLimits()`, which would trigger a new emergency shutdown
3. `TemperatureSensorFallback::canContinueOperation()` and no `SENSOR_FAILURE` error bit, else `emergency_release_refused:sensors_unavailable`
4. `SafetyInterlocks::checkSystemErrors()` (no SENSOR_FAILURE, MODBUS or RELAY error bit), else `emergency_release_refused:system_errors`

On release: `EMERGENCY_STOP` cleared, `BOILER_ENABLED` set only if the saved `boilerEnabled` setting is true, failsafe level WARNING, `recoveryAttempts` reset; result `emergency_released`. The result is published (not retained) on `boiler/status/burner`. The burner state machine still waits out its ERROR recovery delay (`SafetyConfig::errorRecoveryMs`, default 5 min). Pumps that were still dissipating keep running until the boiler output is below 60.0°C (without a usable reading at most `pumpCooldownMs`). `CentralizedFailsafe::attemptRecovery()` exists but has no caller.

**Runtime-Configurable Parameters**:
- **Post-Purge Duration**: 30-180s (default: 90s)
  - Time the burner stays in POST_PURGE with its relays off after a stop
  - Adjustable via MQTT: `boiler/cmd/config/post_purge_ms`

**Post-Purge Timing**:
- **Purpose**: Keep the burner off after a stop; pumps are controlled independently by PumpControlModule. If heat demand returns during post-purge, the burner restarts via PRE_PURGE once the minimum off-time (20 s) has passed
- **Default**: 90 seconds (`SafetyConfig::Defaults::POST_PURGE_MS`)
- **Minimum**: 30 seconds (`Limits::POST_PURGE_MIN_MS`)
- **Maximum**: 180 seconds (`Limits::POST_PURGE_MAX_MS`)
- Values outside 30-180 s are rejected by `SafetyConfig::setPostPurge()`; an out-of-range value loaded from NVS is replaced by the default

---

### Layer 4: DELAY Watchdog (Hardware-Level Relay Safety)

**Purpose**: Hardware-enforced relay auto-OFF protection independent of ESP32 software state.

**Status**: ✅ **Active** (implemented Dec 15, 2025)

**Location**: `src/modules/tasks/RYN4ProcessingTask.cpp`, `SystemConstants::Relay::DELAY_WATCHDOG_SECONDS`

**Mechanism**:

RYN4 relay module supports hardware DELAY commands (0x06XX format) that automatically turn OFF relays after a specified time. This provides hardware-level protection against ESP32 crashes, hangs, or power loss.

**Implementation**:
```cpp
// Every relay ON command includes 10-second auto-OFF timer
Command: 0x060A  // Turn relay ON, auto-OFF after 10 seconds (0x0A = 10)

// Renewal on an elapsed-time schedule, checked on every Modbus SET tick:
// renewed once 5 s (DELAY_WATCHDOG_SECONDS / 2) have passed since the last renewal
// or state change (a state change re-issues the DELAY commands and counts as renewal).
// A failed renewal is retried on the next SET tick.
Safety margin:     at least 5 seconds (50% of 10s)
```

**Protection Scenarios**:

| ESP32 State | DELAY Behavior | Result |
|-------------|----------------|--------|
| Normal operation | Renewed every 5s | Relays stay ON ✅ |
| Software crash | No renewal | Relays auto-OFF after 10s ✅ |
| Power loss | No renewal | Relays auto-OFF after 10s ✅ |
| Task hang | No renewal | Relays auto-OFF after 10s ✅ |
| Modbus failure | No renewal | Relays auto-OFF after 10s ✅ |

**Key Features**:
1. **Hardware-enforced**: RYN4 module handles timing independently
2. **Fail-safe**: Default is OFF if ESP32 fails
3. **Automatic renewal**: Normal operation renews once 5 s have elapsed, checked on every SET tick; a failed renewal is retried on the next SET tick
4. **Compact protocol**: Contiguous relays renewed efficiently (minimizes Modbus traffic)
5. **State tracking**: ESP32 tracks DELAY expiry to skip verification

**Configuration**:
```cpp
// SystemConstants::Relay
constexpr uint8_t DELAY_WATCHDOG_SECONDS = 10;  // Auto-OFF timer
// Renewal interval DELAY_WATCHDOG_SECONDS / 2 = 5 s, elapsed-time based, retried on the next SET tick
```

**Benefits**:
- ✅ Protects against ESP32 software bugs
- ✅ Protects against ESP32 hardware failure
- ✅ Protects against power brown-outs
- ✅ Independent of watchdog timers (additional layer)
- ✅ No additional hardware required (uses existing RYN4)

**Related**: See `docs/ALGORITHMS.md` section 13 for Modbus tick scheduling, and `docs/EQUIPMENT_SPECS.md` for RYN4 DELAY command details.

---

### Not Implemented: Hardware Interlocks

**Purpose**: Physical safety sensors independent of software.

**Status**: Not implemented and not counted as a layer. `BurnerSafetyValidator::checkHardwareInterlocks()` is a stub that always returns true (no interlock inputs are wired).

**Possible additions** (not in the code):
- Physical flame sensor (ionization rod or UV detector)
- Flow sensor (verify water circulation)
- Independent pressure switch
- Redundant temperature sensors

---

## Runtime-Configurable Safety Parameters

The parameters below can be adjusted at runtime via MQTT and persist across reboots. `SafetyConfig` also holds `errorRecoveryMs` (default 300000 ms, range 60000-1800000) and the PID integral limits; these are loaded from NVS but have no MQTT command.

### SafetyConfig Module

**Location**: `src/config/SafetyConfig.cpp`

**Storage**: NVS namespace "safety"

**Parameters**:

#### 1. Pump Protection Delay
```cpp
Range: 5000-60000ms (5-60 seconds)
Default: 15000ms (15 seconds)
MQTT: boiler/cmd/config/pump_protection_ms
```
**Purpose**: Minimum time pump must run before allowing shutdown. Protects pump motor from rapid cycling wear.

**Use Cases**:
- Increase for high-inertia pumps
- Decrease for testing (minimum 5s safety floor)

#### 2. Sensor Staleness Timeout
```cpp
Range: 30000-300000ms (30-300 seconds)
Default: 60000ms (60 seconds)
MQTT: boiler/cmd/config/sensor_stale_ms
```
**Purpose**: Maximum age of sensor data before considering it invalid.

**Use Cases**:
- Increase for slow Modbus networks
- Decrease for critical safety applications requiring fresh data

#### 3. Post-Purge Duration
```cpp
Range: 30000-180000ms (30-180 seconds)
Default: 90000ms (90 seconds)
MQTT: boiler/cmd/config/post_purge_ms
```
**Purpose**: Duration of the burner POST_PURGE state (burner relays off) after a stop. The circulation pumps are not controlled by the post-purge; PumpControlModule runs them while their mode is active.

**Use Cases**:
- Adjust based on boiler volume and chimney draft
- Regulatory requirements may mandate minimum duration

#### 4. Thermal Shock Differential
```cpp
Range: 100-600 (tenths of °C, 10.0-60.0°C)
Default: 450 (45.0°C)
MQTT: boiler/cmd/config/thermal_shock_c
```
**Purpose**: Maximum boiler output minus return differential in the periodic full check of SafetyInterlocks (`performFullSafetyCheck()`). The burner start check in BurnerSafetyValidator keeps its fixed 35.0°C limit and starts return preheating above it.

---

## Safety Configuration Management

### Loading Configuration

**Startup Sequence**:
1. `PersistentStorageTask` loads SafetyConfig from NVS (line 566)
2. If no stored values, defaults are used
3. Configuration published to MQTT when broker connects

**Code**:
```cpp
// src/modules/tasks/PersistentStorageTask.cpp:566
SafetyConfig::loadFromNVS();
```

### Changing Configuration

**MQTT Command Flow**:
1. User publishes to `boiler/cmd/config/{param}_ms`
2. `MQTTCommandHandlers::routeControlCommand()` routes to handler
3. `handleSafetyConfigCommand()` validates and applies change
4. New value saved immediately to NVS via `SafetyConfig::saveToNVS()`
5. Updated config published to `boiler/status/safety_config`

**Validation**:
- All values checked against min/max ranges
- Invalid values rejected with error message
- Changes take effect immediately (no reboot required)

**Example**:
```bash
# Set sensor staleness to 120 seconds
mosquitto_pub -t "boiler/cmd/config/sensor_stale_ms" -m "120000"

# Device responds on boiler/status/safety_config:
# {"pump_prot":15000,"sensor_stale":120000,"post_purge":90000,"thermal_shock_c":450}
```

---

## Safety Simplification (v0.1.0)

### Removed Safety Checks

The following checks were removed as **redundant or counterproductive**:

#### 1. Rate-of-Change Detection
**Removed**: Temperature change rate limiting
**Reason**:
- Industrial boiler can heat 10-15°C in 15 seconds (normal operation)
- False positives prevented legitimate heating
- Over-temperature protection (110°C limit, 115°C emergency stop) already provides thermal safety

#### 2. Cross-Validation
**Removed**: Inter-sensor agreement checking
**Reason**:
- Boiler output vs return can differ by 30°C (normal during heating)
- False positives due to legitimate thermal gradients
- Individual sensor validity checks remain active

#### 3. Thermal Runaway Detection
**Removed**: Historical temperature trend analysis
**Reason**:
- Overlap with over-temperature protection (110°C / 115°C hard limits)
- Memory overhead (~288 bytes for history buffers)
- Hard limits more reliable than predictive detection

**Memory Freed**: ~288 bytes (removed temperature history vectors)

### Retained Safety Checks

**Core safety checks remain**:
1. Over-temperature protection (110°C limit, 115°C emergency stop)
2. Pressure bounds (1.00-3.50 BAR)
3. Sensor validity and staleness
4. Thermal shock differential (return preheating)
5. Burner anti-flapping (2 min minimum on-time, 20 s minimum off-time)

**Result**: Streamlined safety system with reduced false positives while maintaining all critical protections.

**Rationale**: See [history/DESIGN_SAFETY_SIMPLIFICATION.md](history/DESIGN_SAFETY_SIMPLIFICATION.md) for the original analysis (historical design note).

---

## Burner State Machine Integration

**Location**: `src/modules/control/BurnerStateMachine.cpp`; transition logic in `include/modules/control/BurnerTransitions.h` and `include/modules/control/BurnerTransitionPolicy.h`

See [STATE_MACHINES.md](STATE_MACHINES.md) for the complete transition rules.

**Active mode request**: `(HEATING_ON && BurnerRequest::HEATING) || (WATER_ON && BurnerRequest::WATER)`

**Safety Integration Points**:

### 1. IDLE State
```cpp
Start: Heat demand + active mode request + safety check + minimum off-time (20s)
Safety: A latched heat demand without an active mode request is ignored
        (heat demand survives emergencyStop() and ERROR recovery)
```

### 2. PRE_PURGE State
```cpp
Duration: 2 seconds (PRE_PURGE_TIME_MS, atmospheric burner)
Purpose: Burner relays forced off before ignition
Safety: Safety check failure -> ERROR
        Mode request or heat demand withdrawn -> IDLE (start aborted)
```

### 3. IGNITION State
```cpp
Duration: Flame proxy checked from 3s, attempt fails at 5s (IGNITION_TIME_MS)
Safety: Failed attempt -> PRE_PURGE retry
        3rd failed attempt (MAX_IGNITION_RETRIES) -> LOCKOUT
Backstop: StateMachine timeout 7s (IGNITION_TIME_MS + IGNITION_BACKSTOP_MARGIN_MS) -> LOCKOUT
```

### 4. RUNNING States
```cpp
Continuous: Layer 2 safety monitoring; failure -> emergencyStop() -> ERROR
Stop after minimum on-time (2 min): heat demand ended or safety check failed
Immediate stop to POST_PURGE (minimum on-time bypassed):
  - Running mode or whole boiler explicitly disabled (stopForExplicitDisable)
  - No active mode request for 10s (MODE_DEMAND_LOSS_GRACE_MS)
  - Flame lost (burner relays no longer active)
  - Mode change while not safe or not burning
Power level change refused on entry: DEGRADED failsafe -> POST_PURGE
  3rd fault within 10 min (POWER_FAULT_MAX_COUNT, POWER_FAULT_WINDOW_MS) -> emergencyStop()
```

### 5. MODE_SWITCHING State
```cpp
Entry: Water <-> heating change while safe and burning
Safety: Safety check failure -> ERROR
        Explicit disable of the mode the relays still run in, or of the boiler -> POST_PURGE
Wait for the new mode's request only while heating is likely wanted,
  at most 15s (MODE_SWITCH_MAX_WAIT_MS), otherwise POST_PURGE
Hard limit: 30s (MODE_SWITCH_HARD_TIMEOUT_MS) -> POST_PURGE
```

### 6. POST_PURGE State
```cpp
Duration: Configurable (SafetyConfig::postPurgeMs, default 90s)
Action: Burner relays off; pumps continue under PumpControlModule
Restart: Heat demand returns -> PRE_PURGE (same conditions as a start from IDLE,
         not for a mode that was just disabled)
```

### 7. LOCKOUT State
```cpp
Trigger: 3 failed ignition attempts
Action: Burner relays off, ALARM relay on
Recovery: Automatic after 5 min (LOCKOUT_TIME_MS) or resetLockout() (MQTT burner_reset)
          Retry counter reset by resetLockout(), on leaving LOCKOUT (also after the
          automatic expiry), at every new start and on a successful ignition
```

### 8. ERROR State
```cpp
Trigger: emergencyStop() - interlock failure, emergency stop, failed burner deactivation,
         repeated power level fault
Action: BurnerSystemController::emergencyShutdown() (burner relays 1-3 off)
Recovery: Automatic after SafetyConfig::errorRecoveryMs (default 5 min) once the
          safety check passes
          The safety check fails while EMERGENCY_STOP is set: release it with
          MQTT emergency_reset (see Emergency Stop Release)
```

### Heat Demand Arming

`include/modules/control/BurnerDemandGate.h`: BurnerControlTask publishes a permission (request present, sensor staleness, `BurnerSafetyValidator` and return preheating checks passed, sensor fallback limits). It arms the demand for a new request only when BoilerTempControlTask would: its PID decision is fresh (6 s) and made for about the same target (1.0°C), otherwise the boiler output must be below target. BoilerTempControlTask keeps the demand equal to "permitted AND PID wants heat" on every cycle. The burner therefore does not ignite while the boiler is above target at a request start.

### Relay Command Protection

`include/shared/RelayCommandPolicy.h`: relay rate limiting (`MIN_RELAY_SWITCH_INTERVAL_MS`, `MAX_RELAY_TOGGLE_RATE_PER_MIN`) and pump motor protection count only real state changes. A mode switch sends all three burner relays; unchanged relays in that batch no longer consume the rate limit, so the following power level change is not rejected. `setRelayStateEmergency()` bypasses both protections.

---

## Safety Event Logging

**Location**: FRAM (MB85RC256V) via `CriticalDataStorage`

**Logged Events**:
- Safety check failures (with reason code)
- Emergency shutdowns
- Configuration changes
- Burner lockouts

**Persistence**: CRC32-validated structures survive power loss

**Access**: Via MQTT error log commands

---

## MQTT Safety Monitoring

### Status Topics

**Safety Configuration**:
```
Topic: boiler/status/safety_config
Frequency: On boot and after changes
Format: {"pump_prot":15000,"sensor_stale":60000,"post_purge":90000,"thermal_shock_c":450}
```

**System Health**:
```
Topic: boiler/status/health
Frequency: Every 60 seconds
Includes: Last safety check result, failsafe state
```

**Error Notifications**:
```
Topic: boiler/status/error
Trigger: Rejected MQTT commands (not safety violations)
Format: plain string: invalid_config_value, invalid_numeric_value, unknown_command
```

**Error Context Snapshot**:
```
Topic: boiler/error/context (retained)
Trigger: ErrorHandler::logError() with a critical error code: SYSTEM_FAILSAFE_TRIGGERED,
         IGNITION_FAILURE, RELAY_SAFETY_INTERLOCK, TEMPERATURE_CRITICAL, SYSTEM_OVERHEATED,
         EMERGENCY_STOP
Rate: At most one snapshot per 30 s; captured in the erroring task, published by the MQTT task
Content: Error code, component, description, task, heap, system state and burner request bits,
         boiler/return/tank temperatures, pressure, relay desired/actual state
```
Field names and formats: see [MQTT_API.md](MQTT_API.md) (Published Topics, `boiler/error/context`).

Safety validation failures have no dedicated MQTT message. `BurnerSafetyValidator::logSafetyEvent()` logs them, records `RELAY_SAFETY_INTERLOCK` via `ErrorHandler::logError()` (which captures an error context snapshot) and sets the `Error::SAFETY` bit. `CentralizedFailsafe::emergencyStop()` logs `SYSTEM_FAILSAFE_TRIGGERED` and therefore also captures one.

### Command Topics

**Safety Parameter Configuration**:
```
boiler/cmd/config/pump_protection_ms    - Pump motor protection
boiler/cmd/config/sensor_stale_ms       - Sensor staleness timeout
boiler/cmd/config/post_purge_ms         - Post-purge duration
boiler/cmd/config/thermal_shock_c       - Thermal shock differential (tenths of °C)
```

See [MQTT_API.md](MQTT_API.md) for complete command reference.

---

## Sensor Precision

### Temperature Sensors (MB8ART)

**Resolution Modes:**
- **LOW_RES**: 0.1°C precision (range: -200°C to 850°C)
- **HIGH_RES**: 0.01°C precision (range: -200°C to 200°C)

**Data Storage:**
- All temperatures stored as `Temperature_t` (int16_t, tenths of degree)
- HIGH_RES values automatically rounded to tenths by MB8ART library
- No floating-point arithmetic - integer-only for safety-critical paths

**Conversion:**
- HIGH_RES: 735 hundredths (73.5°C) → rounded to 74 tenths (7.4°C)
- LOW_RES: 74 tenths (7.4°C) → used directly as 74 tenths (7.4°C)

**Rationale:**
- 0.1°C precision far exceeds boiler control requirements
- Safety margins: 5-10°C typical
- Control hysteresis: 2-5°C typical
- Industrial sensor accuracy: ±0.5°C typical

**Hardware Configuration:**
- Sensor can be in either LOW_RES or HIGH_RES mode
- Library automatically adapts to configured mode
- No manual configuration required by controller

---

## Testing and Validation

### Field Testing Results

**Status**: Production-tested with weeks of continuous operation
- Zero watchdog resets
- Zero false safety shutdowns
- Verified post-purge effectiveness
- Pump protection prevents motor damage

### Safety Test Scenarios

**Recommended Tests**:
1. **Over-temperature**: Simulate boiler output >=110°C at a request -> expect burner start refused and ERROR for `errorRecoveryMs` (`TEMPERATURE_EXCEEDED` calls `BurnerStateMachine::emergencyStop()`); >=115°C during operation -> expect emergency stop (`CentralizedFailsafe::emergencyStop()`)
2. **Sensor staleness**: Disconnect Modbus -> expect shutdown after timeout
3. **Pressure loss**: Simulate pressure below 1.00 BAR at a request -> expect ERROR for `errorRecoveryMs` (default 5 min, `PRESSURE_EXCEEDED` calls `emergencyStop()`); a missing pressure reading (`SENSOR_FAILURE`) only blocks the demand
4. **Thermal shock**: Boiler output more than 35°C above return -> expect burner blocked and return preheating (heating pump cycling). There is no pump interlock: a stopped pump does not cut off the burner directly
5. **Post-purge**: Shutdown during heating -> verify burner relays stay off for `SafetyConfig::postPurgeMs` (default 90s) while pumps follow their mode

---

## Safety Compliance

**Standards Considered**:
- EN 60730-1: Automatic electrical controls
- EN 12828: Heating systems design
- VDE 0116: Combustion control systems

**Professional Installation Required**: This is industrial control software managing combustion equipment. Installation must be performed by qualified technicians with understanding of:
- Local building codes
- Gas safety regulations
- Electrical safety standards
- Boiler/burner control requirements

---

## Safety-Critical Code Review

**All changes affecting safety must**:
1. Maintain 4-layer architecture independence
2. Preserve hard safety limits (110°C / 115°C temperature, pressure bounds)
3. Follow existing validation patterns
4. Include safety impact analysis
5. Test against known failure scenarios

**Safety-Critical Files**:
- `src/modules/control/BurnerSafetyValidator.cpp`
- `src/modules/control/SafetyInterlocks.cpp`
- `src/modules/control/CentralizedFailsafe.cpp`
- `src/config/SafetyConfig.cpp`
- `src/modules/control/BurnerStateMachine.cpp`
- `src/modules/control/BurnerSystemController.cpp`
- `include/modules/control/BurnerTransitions.h`
- `include/modules/control/BurnerTransitionPolicy.h`
- `include/modules/control/BurnerDemandGate.h`
- `include/shared/RelayCommandPolicy.h`

---

## Future Enhancements

**Planned** (from IMPROVEMENT_OPPORTUNITIES.md):
1. **Hardware flame sensor** - Replace relay-based flame detection
2. **Flow sensor integration** - Verify water circulation
3. **Independent pressure switch** - Hardware pressure monitoring
4. **Safety audit log** - Detailed event timeline for analysis
5. **Watchdog improvements** - Enhanced task monitoring with safety fallback

---

## Quick Reference

**Default Safety Values**:
```
Sensor staleness:    60 seconds
Pump protection:     15 seconds
Post-purge:          90 seconds
DELAY watchdog:      10 seconds (hardware auto-OFF, renewed every 5s)
Max boiler temp:     110.0°C (burner blocked), 115.0°C (emergency stop)
Pressure range:      1.00-3.50 BAR
Burner min on-time:  120 seconds (bypassed on explicit disable, lost mode request, flame loss)
Burner min off-time: 20 seconds
Error recovery:      5 minutes (automatic, after safety check passes)
```

**Safety Configuration via MQTT**:
```bash
# View current config
mosquitto_sub -t "boiler/status/safety_config" -C 1

# Change sensor staleness to 90 seconds
mosquitto_pub -t "boiler/cmd/config/sensor_stale_ms" -m "90000"

# Change post-purge to 60 seconds
mosquitto_pub -t "boiler/cmd/config/post_purge_ms" -m "60000"
```

**Emergency Actions**:
```bash
# Disable entire system
mosquitto_pub -t "boiler/cmd/system" -m "off"

# Release a latched emergency stop (refused while its causes persist)
mosquitto_pub -t "boiler/cmd/emergency_reset" -m "reset"

# Leave burner LOCKOUT
mosquitto_pub -t "boiler/cmd/burner_reset" -m "lockout"

# Check error log
mosquitto_pub -t "errors/list" -m "20"
```

---

**Critical Safety Warning**: Only modify safety parameters if you fully understand the implications. Incorrect values can compromise safety. When in doubt, use defaults - they are proven safe in field testing.
