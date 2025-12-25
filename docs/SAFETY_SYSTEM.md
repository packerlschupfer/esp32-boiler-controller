# Safety System Architecture

## Overview

The boiler controller implements a **5-layer safety architecture** designed to prevent dangerous conditions through multiple independent safety mechanisms. Each layer operates independently and can trigger emergency shutdown.

**Design Philosophy**: Defense in depth - multiple independent safety checks ensure that failure of one layer does not compromise overall system safety.

---

## Safety Layers

### Layer 1: BurnerSafetyValidator (Pre-operation Validation)

**Purpose**: Validate all safety conditions BEFORE allowing burner operations.

**Location**: `src/modules/control/BurnerSafetyValidator.cpp`

**Validation Steps** (7 critical checks):
1. **System enabled check** - Verify boiler control is active
2. **Pressure bounds** - 1.00-3.50 BAR operating range
3. **Temperature limits** - Maximum 90�C over-temperature protection
4. **Sensor validity** - All critical sensors must have valid readings
5. **Sensor staleness** - Data must be recent (configurable timeout)
6. **Pump interlock** - Heating or water pump must be running
7. **Burner anti-flapping** - Minimum 2 minutes between burner cycles

**Runtime-Configurable Parameters**:
- **Sensor Staleness Timeout**: 30-300s (default: 60s)
  - Prevents operation with stale sensor data
  - Adjustable via MQTT: `boiler/cmd/config/sensor_stale_ms`

**Fixed Safety Limits** (not configurable):
- Maximum temperature: 90�C
- Minimum pressure: 1.00 BAR
- Maximum pressure: 3.50 BAR
- Minimum burner cycle time: 120 seconds

**Result**: Returns `SafetyCheckResult` enum indicating pass/fail and reason.

---

### Layer 2: SafetyInterlocks (Continuous Monitoring)

**Purpose**: Monitor safety conditions continuously during operation and trigger immediate shutdown if unsafe conditions develop.

**Location**: `src/modules/control/SafetyInterlocks.cpp`

**Monitoring Checks**:
1. **Sensor staleness detection** - Configurable timeout (default 60s)
2. **Temperature bounds monitoring** - Continuous over-temp protection
3. **Pressure monitoring** - Continuous pressure range verification
4. **Pump state verification** - Ensure pump runs when burner active

**Key Difference from Layer 1**:
- Layer 1: Pre-operation validation (gate-keeping)
- Layer 2: Continuous monitoring (real-time protection)

**Runtime-Configurable Parameters**:
- **Sensor Staleness Timeout**: Same as Layer 1 (shares SafetyConfig)
- **Pump Protection Delay**: 5-60s (default: 15s)
  - Prevents rapid pump cycling (motor protection)
  - Adjustable via MQTT: `boiler/cmd/config/pump_protection_ms`

---

### Layer 3: CentralizedFailsafe (Coordinated Emergency Shutdown)

**Purpose**: Coordinate safe shutdown across all subsystems when safety violation detected.

**Location**: `src/modules/control/CentralizedFailsafe.cpp`

**Shutdown Sequence**:
1. **Immediate burner cutoff** - Stop combustion
2. **Maintain pump operation** - Continue circulation for heat dissipation
3. **Post-purge execution** - Configurable duration for exhaust gas removal
4. **State persistence** - Log failure reason to FRAM
5. **MQTT notification** - Publish safety event

**Runtime-Configurable Parameters**:
- **Post-Purge Duration**: 30-180s (default: 90s)
  - Critical for removing combustion gases
  - Adjustable via MQTT: `boiler/cmd/config/post_purge_ms`

**Post-Purge Timing**:
- **Purpose**: Exhaust residual combustion gases after burner shutdown
- **Default**: 90 seconds (tested and proven safe)
- **Minimum**: 30 seconds (regulatory requirement)
- **Maximum**: 180 seconds (prevents excessive cycling)

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

// Renewed every 5 seconds (50% safety margin)
SET tick occurs at: 0.5s, 3.0s, 5.5s, 8.0s, 10.5s...
DELAY expires at:  10.5s if not renewed at 5.5s
Safety margin:     5 seconds (50% of 10s)
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
3. **Automatic renewal**: Normal operation renews every 5s (ticks 1, 6)
4. **Compact protocol**: Contiguous relays renewed efficiently (minimizes Modbus traffic)
5. **State tracking**: ESP32 tracks DELAY expiry to skip verification

**Configuration**:
```cpp
// SystemConstants::Relay
constexpr uint8_t DELAY_WATCHDOG_SECONDS = 10;  // Auto-OFF timer
// Renewed at 5s intervals (Modbus SET ticks: 1, 6 in 10-tick cycle)
```

**Benefits**:
- ✅ Protects against ESP32 software bugs
- ✅ Protects against ESP32 hardware failure
- ✅ Protects against power brown-outs
- ✅ Independent of watchdog timers (additional layer)
- ✅ No additional hardware required (uses existing RYN4)

**Related**: See `docs/ALGORITHMS.md` section 13 for Modbus tick scheduling, and `docs/EQUIPMENT_SPECS.md` for RYN4 DELAY command details.

---

### Layer 5: Hardware Interlocks (Future)

**Purpose**: Physical safety sensors independent of software.

**Status**: Planned for future implementation

**Planned Features**:
- Physical flame sensor (ionization rod or UV detector)
- Flow sensor (verify water circulation)
- Independent pressure switch
- Redundant temperature sensors

---

## Runtime-Configurable Safety Parameters

All safety parameters can be adjusted at runtime via MQTT and persist across reboots.

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
**Purpose**: Duration to run circulation pump after burner shutdown to remove exhaust gases.

**Use Cases**:
- Adjust based on boiler volume and chimney draft
- Regulatory requirements may mandate minimum duration

---

## Safety Configuration Management

### Loading Configuration

**Startup Sequence**:
1. `PersistentStorageTask` loads SafetyConfig from NVS (line 306)
2. If no stored values, defaults are used
3. Configuration published to MQTT when broker connects

**Code**:
```cpp
// src/modules/tasks/PersistentStorageTask.cpp:306
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
# {"pump_prot":15000,"sensor_stale":120000,"post_purge":90000}
```

---

## Safety Simplification (v0.1.0)

### Removed Safety Checks

The following checks were removed as **redundant or counterproductive**:

#### 1. Rate-of-Change Detection
**Removed**: Temperature change rate limiting
**Reason**:
- Industrial boiler can heat 10-15�C in 15 seconds (normal operation)
- False positives prevented legitimate heating
- Over-temperature protection (90�C) already provides thermal safety

#### 2. Cross-Validation
**Removed**: Inter-sensor agreement checking
**Reason**:
- Boiler output vs return can differ by 30�C (normal during heating)
- False positives due to legitimate thermal gradients
- Individual sensor validity checks remain active

#### 3. Thermal Runaway Detection
**Removed**: Historical temperature trend analysis
**Reason**:
- Overlap with over-temperature protection (90�C hard limit)
- Memory overhead (~288 bytes for history buffers)
- Hard limits more reliable than predictive detection

**Memory Freed**: ~288 bytes (removed temperature history vectors)

### Retained Safety Checks

**Core safety checks remain**:
1. Over-temperature protection (90�C)
2. Pressure bounds (1.00-3.50 BAR)
3. Sensor validity and staleness
4. Pump interlock
5. Burner anti-flapping (2 min minimum cycle)

**Result**: Streamlined safety system with reduced false positives while maintaining all critical protections.

**Rationale**: See `DESIGN_SAFETY_SIMPLIFICATION.md` for complete analysis.

---

## Burner State Machine Integration

**Location**: `src/modules/control/BurnerStateMachine.cpp`

**Safety Integration Points**:

### 1. PRE_PURGE State
```cpp
Duration: 30 seconds (fixed)
Purpose: Exhaust any residual gases before ignition
```

### 2. IGNITION State
```cpp
Duration: 10 seconds
Safety: If flame not detected, enter LOCKOUT
```

### 3. RUNNING States
```cpp
Continuous: Layer 2 safety monitoring active
Action: Any violation triggers POST_PURGE shutdown
```

### 4. POST_PURGE State
```cpp
Duration: Configurable (SafetyConfig::postPurgeMs)
Purpose: Exhaust combustion gases after shutdown
Safety: Pumps continue, burner disabled
```

### 5. LOCKOUT State
```cpp
Trigger: Ignition failure or safety violation
Recovery: Manual intervention required
```

### 6. ERROR State
```cpp
Trigger: System-level errors
Recovery: Clear error condition, then manual reset
```

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
Format: {"pump_prot":15000,"sensor_stale":60000,"post_purge":90000}
```

**System Health**:
```
Topic: boiler/status/health
Frequency: Every 60 seconds
Includes: Last safety check result, failsafe state
```

**Error Notifications**:
```
Topic: boiler/status/errors
Trigger: Safety violations
Format: {"error":"SAFETY_CHECK_FAILED","reason":"OVER_TEMP","timestamp":...}
```

### Command Topics

**Safety Parameter Configuration**:
```
boiler/cmd/config/pump_protection_ms    - Pump motor protection
boiler/cmd/config/sensor_stale_ms       - Sensor staleness timeout
boiler/cmd/config/post_purge_ms         - Post-purge duration
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
1. **Over-temperature**: Simulate sensor reading >90�C � expect immediate shutdown
2. **Sensor staleness**: Disconnect Modbus � expect shutdown after timeout
3. **Pressure loss**: Simulate low pressure � expect burner inhibit
4. **Pump interlock**: Stop pump manually � expect burner cutoff
5. **Post-purge**: Shutdown during heating � verify 90s pump continuation

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
2. Preserve hard safety limits (90�C, pressure bounds)
3. Follow existing validation patterns
4. Include safety impact analysis
5. Test against known failure scenarios

**Safety-Critical Files**:
- `src/modules/control/BurnerSafetyValidator.cpp`
- `src/modules/control/SafetyInterlocks.cpp`
- `src/modules/control/CentralizedFailsafe.cpp`
- `src/config/SafetyConfig.cpp`
- `src/modules/control/BurnerStateMachine.cpp`

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
Max temperature:     90�C
Pressure range:      1.00-3.50 BAR
Burner min cycle:    120 seconds
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

# Check error log
mosquitto_pub -t "errors/list" -m "20"
```

---

**� Critical Safety Warning**: Only modify safety parameters if you fully understand the implications. Incorrect values can compromise safety. When in doubt, use defaults - they are proven safe in field testing.
