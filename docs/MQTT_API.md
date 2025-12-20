# MQTT API Reference

## Overview

The boiler controller provides a comprehensive MQTT API for monitoring and control. All topics use the `boiler/` prefix (configurable in MQTTTopics.h).

**Broker**: 192.168.20.27:1883
**Client ID**: `ESPlan-Boiler` (matches DEVICE_HOSTNAME)
**QoS**: 0 (best effort, typical for control systems)
**Keep-Alive**: 60 seconds

---

## Topic Organization

```
boiler/
├── status/           # Published status (read-only)
│   ├── sensors       # Temperature and pressure data
│   ├── burner        # Burner state
│   ├── online        # Connection status (retained)
│   ├── errors        # Error notifications
│   ├── device/       # Device information
│   └── scheduler/    # Schedule status
├── cmd/              # Commands (write)
│   ├── control       # Control commands
│   ├── config        # Configuration changes
│   └── scheduler/    # Schedule management
└── scheduler/        # Scheduler-specific
    ├── response      # Command responses
    └── event         # Schedule events
```

---

## Status Topics (Published by Device)

### 1. Sensor Data
**Topic**: `boiler/status/sensors`
**Frequency**: Every 10 seconds
**Retained**: No

**Format**: Fixed-point JSON (compact, integer values)
```json
{
  "t": {
    "bo": 654,    // Boiler output temp (tenths of °C): 654 = 65.4°C
    "br": 582,    // Boiler return: 58.2°C
    "wt": 551,    // Water tank: 55.1°C
    "hr": 423,    // Heating return: 42.3°C
    "o": 125,     // Outside: 12.5°C
    "i": 213      // Inside/room: 21.3°C (optional, if valid)
  },
  "p": 152,       // Pressure (hundredths of BAR): 152 = 1.52 BAR
  "r": 21,        // Relay states (bitmask)
  "s": 39,        // System state (bitmask)
  "sf": 1         // Sensor fallback mode
}
```

**Relay Bitmask `r`** (byte value):
- Bit 0 (1): Burner enabled
- Bit 1 (2): Heating pump ON
- Bit 2 (4): Water pump ON
- Bit 3 (8): Half power mode
- Bit 4 (16): Water mode active

**Example**: `"r": 21` = binary `10101` = burner ON + water pump ON + water mode ON

**System State Bitmask `s`** (byte value):
- Bit 0 (1): Boiler/system enabled
- Bit 1 (2): Heating enabled
- Bit 2 (4): Heating ON (actively heating)
- Bit 3 (8): Water heating enabled
- Bit 4 (16): Water heating ON (actively heating)
- Bit 5 (32): Water priority active

**Example**: `"s": 39` = binary `100111` = system ON + heating enabled + heating ON + water priority

**Sensor Fallback Mode `sf`**:
- 0 = STARTUP (waiting for sensors to stabilize)
- 1 = NORMAL (all required sensors operational)
- 2 = SHUTDOWN (degraded mode - critical sensors missing)

### 2. Online Status
**Topic**: `boiler/status/online`
**Frequency**: On connect/disconnect
**Retained**: Yes (last will)

```json
{"online": true}
{"online": false}
```

### 3. Device Information
**Topic**: `boiler/status/device/ip`
**Retained**: Yes
```
192.168.16.138
```

**Topic**: `boiler/status/device/hostname`
**Retained**: Yes
```
ESPlan-Boiler
```

### 4. Scheduler Events
**Topic**: `boiler/scheduler/event`
**Frequency**: On schedule start/end
**Retained**: No

```json
{
  "type": "start",
  "schedule": "Morning Shower",
  "schedule_type": "water_heating",
  "timestamp": "2025-08-11T06:30:00"
}
```

```json
{
  "type": "end",
  "schedule": "Morning Shower",
  "schedule_type": "water_heating",
  "timestamp": "2025-08-11T08:00:00"
}
```

### 5. Error Notifications
**Topic**: `boiler/status/errors`
**Frequency**: When errors occur
**Retained**: No

```json
{
  "error": "PRESSURE_LOW",
  "pressure": 0.35,
  "timestamp": 1692345678,
  "severity": "CRITICAL"
}
```

---

## Command Topics (Subscribe)

### Scheduler Commands

#### Add Schedule
**Topic**: `boiler/cmd/scheduler/add`
**Response**: `boiler/scheduler/response`

**Water Heating Schedule**:
```json
{
  "type": "water_heating",
  "name": "Morning Shower",
  "start_hour": 6,
  "start_minute": 30,
  "end_hour": 8,
  "end_minute": 0,
  "days": [1,2,3,4,5],    // Monday-Friday (1=Mon, 7=Sun)
  "target_temp": 55,       // Optional, °C (uses system default if omitted)
  "priority": true,        // Optional, default false
  "enabled": true
}
```

**Space Heating Schedule**:
```json
{
  "type": "space_heating",
  "name": "Evening Comfort",
  "start_hour": 18,
  "start_minute": 0,
  "end_hour": 22,
  "end_minute": 0,
  "days": [1,2,3,4,5],
  "mode": 0,               // 0=COMFORT (21°C), 1=ECO (18°C), 2=FROST (10°C)
  "target_temp": 22,       // Optional override (uses mode default if omitted)
  "zones": 1,              // Optional, default 1
  "enabled": true
}
```

**Response**:
```json
{
  "result": "ok",
  "id": 3,
  "message": "Schedule added successfully"
}
```

**Error Response**:
```json
{
  "result": "error",
  "message": "Maximum schedules reached (10 per type)"
}
```

#### Remove Schedule
**Topic**: `boiler/cmd/scheduler/remove`

```json
{
  "id": 3
}
```

**Response**:
```json
{"result": "ok", "message": "Schedule removed"}
```

#### List Schedules
**Topic**: `boiler/cmd/scheduler/list`
**Payload**: Empty `{}` or any value

**Response**: `boiler/scheduler/response`
```json
{
  "count": 2,
  "schedules": [
    {
      "id": 0,
      "type": "water_heating",
      "name": "Morning Shower",
      "start": "06:30",
      "end": "08:00",
      "days": [1,2,3,4,5],
      "target_temp": 55,
      "priority": true,
      "enabled": true,
      "active": false
    },
    {
      "id": 1,
      "type": "space_heating",
      "name": "Evening Comfort",
      "start": "18:00",
      "end": "22:00",
      "days": [1,2,3,4,5],
      "mode": 0,
      "enabled": true,
      "active": true
    }
  ]
}
```

#### Enable/Disable Schedule
**Topic**: `boiler/cmd/scheduler/enable`

```json
{
  "id": 1,
  "enabled": false
}
```

### Control Commands

**Topic Pattern**: `boiler/cmd/{command}`

#### Enable/Disable Boiler
```bash
mosquitto_pub -t "boiler/cmd/boiler" -m "enable"
mosquitto_pub -t "boiler/cmd/boiler" -m "disable"
```

#### Enable/Disable Heating
```bash
mosquitto_pub -t "boiler/cmd/heating" -m "enable"
mosquitto_pub -t "boiler/cmd/heating" -m "disable"
```

#### Enable/Disable Water Heating
```bash
mosquitto_pub -t "boiler/cmd/water" -m "enable"
mosquitto_pub -t "boiler/cmd/water" -m "disable"
```

### Configuration Commands

**Topic Pattern**: `boiler/config/{parameter}`

#### Set Water Temperature Setpoint
```bash
mosquitto_pub -t "boiler/config/water_setpoint" -m '{"value": 60}'
```

#### Set Room Temperature Setpoint
```bash
mosquitto_pub -t "boiler/config/room_setpoint" -m '{"value": 21}'
```

#### Set PID Parameters
```bash
mosquitto_pub -t "boiler/config/pid" -m '{
  "controller": "heating",
  "kp": 2.5,
  "ki": 0.15,
  "kd": 0.8
}'
```

### Safety Configuration Commands

Runtime-configurable safety parameters with validation and NVS persistence.

**Topic**: `boiler/status/safety_config` (Published on startup and after changes)
**Format**:
```json
{
  "pump_prot": 15000,
  "sensor_stale": 60000,
  "post_purge": 90000
}
```

#### Set Pump Protection Delay
Minimum delay before pump can be switched off after turning on (motor protection).

**Topic**: `boiler/cmd/config/pump_protection_ms`
**Range**: 5000-60000ms (5-60 seconds)
**Default**: 15000ms (15 seconds)

```bash
# Set to 30 seconds
mosquitto_pub -h $BROKER -u $USER -P $PASS \
  -t "boiler/cmd/config/pump_protection_ms" -m "30000"
```

#### Set Sensor Staleness Timeout
Maximum time before sensor data is considered stale and triggers safety interlock.

**Topic**: `boiler/cmd/config/sensor_stale_ms`
**Range**: 30000-300000ms (30-300 seconds)
**Default**: 60000ms (60 seconds)

```bash
# Set to 120 seconds
mosquitto_pub -h $BROKER -u $USER -P $PASS \
  -t "boiler/cmd/config/sensor_stale_ms" -m "120000"
```

#### Set Post-Purge Duration
Duration of post-purge cycle after burner shutdown (exhaust gas removal).

**Topic**: `boiler/cmd/config/post_purge_ms`
**Range**: 30000-180000ms (30-180 seconds)
**Default**: 90000ms (90 seconds)

```bash
# Set to 60 seconds
mosquitto_pub -h $BROKER -u $USER -P $PASS \
  -t "boiler/cmd/config/post_purge_ms" -m "60000"
```

**Notes**:
- Changes are saved immediately to NVS (persist across reboots)
- Updated config is published to `boiler/status/safety_config`
- Invalid values are rejected with error message
- All timings critical for safety - only modify if you understand the implications

### Return Preheat Configuration (Thermal Shock Mitigation)

When transitioning from water heating to space heating, the heating return line may be cold while the boiler is hot. Starting with a large temperature differential (>30°C) risks thermal shock damage to the boiler.

The preheat system cycles the heating pump to gradually warm the return line before allowing the burner to start. Progressive pump cycling pattern:
- ON durations increase: 3s → 5s → 8s → 12s → 15s (more circulation as system warms)
- OFF durations decrease: 25s → 20s → 15s → 10s → 5s (less wait as differential reduces)
- Exits when differential < safe threshold or timeout

#### Enable/Disable Preheating
**Topic**: `boiler/cmd/config/preheat_enabled`
**Values**: 0 (disabled), 1 (enabled)
**Default**: 1 (enabled)

```bash
# Disable thermal shock preheating
mosquitto_pub -h $BROKER -u $USER -P $PASS \
  -t "boiler/cmd/config/preheat_enabled" -m "0"

# Enable thermal shock preheating (default)
mosquitto_pub -h $BROKER -u $USER -P $PASS \
  -t "boiler/cmd/config/preheat_enabled" -m "1"
```

#### Set OFF Duration Scaling Factor
Scales the base OFF durations (25s → 20s → 15s → 10s → 5s).
Value of 5 = 1x (default), 10 = 2x longer, 1 = 0.2x shorter.

**Topic**: `boiler/cmd/config/preheat_off_multiplier`
**Range**: 1-10
**Default**: 5 (1x scaling, uses base durations)

```bash
# Double OFF durations (50s → 40s → 30s → 20s → 10s)
mosquitto_pub -h $BROKER -u $USER -P $PASS \
  -t "boiler/cmd/config/preheat_off_multiplier" -m "10"

# Halve OFF durations (12.5s → 10s → 7.5s → 5s → 2.5s)
mosquitto_pub -h $BROKER -u $USER -P $PASS \
  -t "boiler/cmd/config/preheat_off_multiplier" -m "2"
```

#### Set Maximum Preheat Cycles
Maximum number of pump ON/OFF cycles before timing out.

**Topic**: `boiler/cmd/config/preheat_max_cycles`
**Range**: 1-20
**Default**: 8

```bash
# Allow up to 12 preheat cycles
mosquitto_pub -h $BROKER -u $USER -P $PASS \
  -t "boiler/cmd/config/preheat_max_cycles" -m "12"
```

#### Set Preheat Timeout
Maximum total time for preheating before timeout (safety limit).

**Topic**: `boiler/cmd/config/preheat_timeout_ms`
**Range**: 60000-1200000ms (1-20 minutes)
**Default**: 600000ms (10 minutes)

```bash
# Set timeout to 15 minutes
mosquitto_pub -h $BROKER -u $USER -P $PASS \
  -t "boiler/cmd/config/preheat_timeout_ms" -m "900000"
```

#### Set Pump Minimum State Change Time
Minimum time between pump state changes during preheating.
Lower values allow faster cycling but reduce motor protection.

**Topic**: `boiler/cmd/config/preheat_pump_min_ms`
**Range**: 1000-30000ms (1-30 seconds)
**Default**: 3000ms (3 seconds)

```bash
# Set minimum pump state change to 5 seconds
mosquitto_pub -h $BROKER -u $USER -P $PASS \
  -t "boiler/cmd/config/preheat_pump_min_ms" -m "5000"
```

**Note**: This is separate from the normal pump protection delay. During preheating,
shorter cycles are allowed to facilitate faster warming of the return line.

#### Set Safe Differential Threshold
Temperature differential (boiler output - return) below which preheating completes.

**Topic**: `boiler/cmd/config/preheat_safe_diff`
**Range**: 100-300 (tenths of °C, i.e., 10.0-30.0°C)
**Default**: 250 (25.0°C)

```bash
# Set safe differential to 20°C
mosquitto_pub -h $BROKER -u $USER -P $PASS \
  -t "boiler/cmd/config/preheat_safe_diff" -m "200"
```

**Preheat Configuration Summary**:

| Parameter | Topic | Range | Default | Description |
|-----------|-------|-------|---------|-------------|
| Enable | `preheat_enabled` | 0-1 | 1 | Enable/disable preheating |
| OFF Scaling | `preheat_off_multiplier` | 1-10 | 5 | OFF duration scale (5=1x) |
| Max Cycles | `preheat_max_cycles` | 1-20 | 8 | Maximum pump cycles |
| Timeout | `preheat_timeout_ms` | 60000-1200000 | 600000 | Total timeout (ms) |
| Pump Min | `preheat_pump_min_ms` | 1000-30000 | 3000 | Min state change (ms) |
| Safe Diff | `preheat_safe_diff` | 100-300 | 250 | Safe differential (tenths °C) |

### Sensor Compensation Offsets

Calibrate temperature and pressure sensors to correct systematic errors.
All values use **fixed-point integers** matching the internal representation:
- **Temperature offsets**: Tenths of °C (e.g., `-14` = -1.4°C)
- **Pressure offset**: Hundredths of BAR (e.g., `-5` = -0.05 BAR)

**Topic Pattern**: `boiler/params/sensor/offset/{sensor}`

| Sensor | Topic | Range | Default | Description |
|--------|-------|-------|---------|-------------|
| boilerOutput | `sensor/offset/boilerOutput` | ±50 (±5.0°C) | 0 | CH0: Boiler output temp |
| boilerReturn | `sensor/offset/boilerReturn` | ±50 (±5.0°C) | 0 | CH1: Boiler return temp |
| waterTank | `sensor/offset/waterTank` | ±50 (±5.0°C) | 0 | CH2: Hot water tank |
| waterOutput | `sensor/offset/waterOutput` | ±50 (±5.0°C) | 0 | CH3: Hot water output |
| waterReturn | `sensor/offset/waterReturn` | ±50 (±5.0°C) | 0 | CH4: Hot water return |
| heatingReturn | `sensor/offset/heatingReturn` | ±50 (±5.0°C) | 0 | CH5: Heating return |
| outside | `sensor/offset/outside` | ±50 (±5.0°C) | 0 | CH6: Outside temp |
| room | `sensor/offset/room` | ±50 (±5.0°C) | -14 | ANDRTF3: Room temp |
| pressure | `sensor/offset/pressure` | ±50 (±0.5 BAR) | 0 | System pressure |

#### Set Sensor Offset
```bash
# Room temperature reads 1.4°C too high → subtract 14 tenths
mosquitto_pub -h $BROKER -u $USER -P $PASS \
  -t "boiler/params/sensor/offset/room" -m "-14"

# Boiler output reads 0.5°C too low → add 5 tenths
mosquitto_pub -h $BROKER -u $USER -P $PASS \
  -t "boiler/params/sensor/offset/boilerOutput" -m "5"

# Pressure sensor reads 0.1 BAR too high → subtract 10 hundredths
mosquitto_pub -h $BROKER -u $USER -P $PASS \
  -t "boiler/params/sensor/offset/pressure" -m "-10"
```

#### Get Current Offsets
```bash
# Get all parameter values
mosquitto_pub -h $BROKER -u $USER -P $PASS \
  -t "boiler/params/get/all" -m ""
```

#### Save Offsets to NVS
```bash
# Persist changes to flash (survives reboot)
mosquitto_pub -h $BROKER -u $USER -P $PASS \
  -t "boiler/params/save" -m ""
```

**Note**: Offsets are applied during sensor data acquisition and immediately affect
all reported temperatures. Changes take effect on the next sensor read cycle.

### Error Log Commands

**Topic Pattern**: `errors/{command}`

#### List Last N Errors
```bash
mosquitto_pub -t "errors/list" -m "20"
```

**Response**: `boiler/status/errors/list`
```json
{
  "count": 20,
  "errors": [
    {
      "timestamp": 1692345678,
      "code": 5,
      "description": "PRESSURE_EXCEEDED",
      "value": 0.35
    },
    ...
  ]
}
```

#### Clear Error Log
```bash
mosquitto_pub -t "errors/clear" -m ""
```

**Response**: `boiler/status/errors/cleared` with payload `"ok"`

#### Get Error Statistics
```bash
mosquitto_pub -t "errors/stats" -m ""
```

**Response**: `boiler/status/errors/stats`
```json
{
  "total": 142,
  "by_type": {
    "SENSOR_READ_FAILED": 85,
    "PRESSURE_EXCEEDED": 12,
    "COMMUNICATION_TIMEOUT": 45
  }
}
```

---

## Example Workflows

### Complete Setup Example

```bash
# Set broker credentials
BROKER="192.168.16.16"
USER="YOUR_MQTT_USER"
PASS="YOUR_MQTT_PASSWORD"

# Monitor all boiler topics
mosquitto_sub -h $BROKER -u $USER -P $PASS -t "boiler/#" -v &

# Add morning shower schedule
mosquitto_pub -h $BROKER -u $USER -P $PASS \
  -t "boiler/cmd/scheduler/add" \
  -m '{
    "type": "water_heating",
    "name": "Morning Shower",
    "start_hour": 6,
    "start_minute": 30,
    "end_hour": 8,
    "end_minute": 0,
    "days": [1,2,3,4,5],
    "target_temp": 55,
    "priority": true,
    "enabled": true
  }'

# Add evening heating schedule
mosquitto_pub -h $BROKER -u $USER -P $PASS \
  -t "boiler/cmd/scheduler/add" \
  -m '{
    "type": "space_heating",
    "name": "Evening Comfort",
    "start_hour": 18,
    "start_minute": 0,
    "end_hour": 22,
    "end_minute": 0,
    "days": [1,2,3,4,5],
    "mode": 0,
    "enabled": true
  }'

# List all schedules
mosquitto_pub -h $BROKER -u $USER -P $PASS \
  -t "boiler/cmd/scheduler/list" \
  -m '{}'

# Get sensor data once
mosquitto_sub -h $BROKER -u $USER -P $PASS \
  -t "boiler/status/sensors" \
  -C 1
```

### Monitoring Script

```bash
#!/bin/bash
# Monitor boiler with formatted output

BROKER="192.168.16.16"
USER="YOUR_MQTT_USER"
PASS="YOUR_MQTT_PASSWORD"

mosquitto_sub -h $BROKER -u $USER -P $PASS \
  -t "boiler/status/sensors" \
  -v | while read topic payload; do

  # Parse JSON and format output
  echo "$(date '+%H:%M:%S') - Sensor Update:"
  echo "$payload" | jq -r '
    "  Boiler Output: \(.t.bo/10)°C",
    "  Water Tank: \(.t.wt/10)°C",
    "  Pressure: \(.p/100) BAR",
    "  Relays: \(.r)"
  '
  echo ""
done
```

### Testing Commands

```bash
# Test echo (verify MQTT working)
mosquitto_pub -t "test/echo" -m "hello"
# Listen for: test/response with "hello"

# Get current sensor reading
mosquitto_sub -t "boiler/status/sensors" -C 1 -W 5

# Check if device is online
mosquitto_sub -t "boiler/status/online" -C 1 -W 2

# View all retained messages
mosquitto_sub -t "#" --retained-only -W 2

# Clear a retained message
mosquitto_pub -t "system/status" -r -n
```

---

## MQTT Topics Reference

### Published Topics (Device → Broker)

| Topic | Frequency | Retained | Priority | Description |
|-------|-----------|----------|----------|-------------|
| `boiler/status/sensors` | 10s | No | High | Temperature/pressure data |
| `boiler/status/online` | On change | Yes | High | Connection status |
| `boiler/status/safety_config` | On boot/change | No | Medium | Safety configuration |
| `boiler/status/device/ip` | On boot | Yes | Low | IP address |
| `boiler/status/device/hostname` | On boot | Yes | Low | Device hostname |
| `boiler/scheduler/event` | On event | No | Medium | Schedule start/end |
| `boiler/scheduler/response` | On command | No | Medium | Command responses |
| `boiler/status/errors` | On error | No | High | Error notifications |
| `boiler/status/errors/list` | On request | No | Low | Error log dump |
| `boiler/config/response` | On command | No | Medium | Config change ack |
| `test/response` | On echo | No | Low | Echo test response |

### Subscribed Topics (Broker → Device)

| Topic | Purpose | Payload Format |
|-------|---------|----------------|
| `test/echo` | Echo test | Any string |
| `boiler/cmd/+` | Control commands | String or JSON |
| `boiler/cmd/config/pump_protection_ms` | Pump protection delay | Integer (5000-60000) |
| `boiler/cmd/config/sensor_stale_ms` | Sensor staleness timeout | Integer (30000-300000) |
| `boiler/cmd/config/post_purge_ms` | Post-purge duration | Integer (30000-180000) |
| `boiler/cmd/config/preheat_enabled` | Enable/disable preheating | Integer (0-1) |
| `boiler/cmd/config/preheat_off_multiplier` | OFF duration multiplier | Integer (1-10) |
| `boiler/cmd/config/preheat_max_cycles` | Max preheat cycles | Integer (1-20) |
| `boiler/cmd/config/preheat_timeout_ms` | Preheat timeout | Integer (60000-1200000) |
| `boiler/cmd/config/preheat_pump_min_ms` | Min pump state change | Integer (1000-30000) |
| `boiler/cmd/config/preheat_safe_diff` | Safe differential | Integer (100-300) tenths °C |
| `boiler/config/+` | Configuration | JSON |
| `boiler/cmd/scheduler/+` | Schedule commands | JSON |
| `errors/+` | Error log commands | String or JSON |

---

## Message Priorities

The system uses 2 priority queues for MQTT publishing:

### High Priority Queue (3 slots, 392 bytes each)
- Sensor data (real-time monitoring)
- Critical alerts
- Connection status
- Error notifications

### Normal Priority Queue (5 slots, 392 bytes each)
- Status updates
- Configuration responses
- Schedule events
- Debug information

**Overflow Strategy**: DROP_LOWEST_PRIORITY
- Scans queue to find lowest priority message
- Drops it to make room for new message
- Preserves sensor data over status updates

---

## Data Format Details

### Temperature Values
All temperatures use fixed-point representation (tenths of °C):
- **Storage**: `int16_t` (Temperature_t)
- **MQTT**: Integer value (divide by 10 for °C)
- **Range**: -3276.8°C to +3276.7°C
- **Precision**: 0.1°C

**Examples**:
- 273 = 27.3°C
- -50 = -5.0°C
- 654 = 65.4°C

### Pressure Values
Pressure uses fixed-point representation (hundredths of BAR):
- **Storage**: `int16_t` (Pressure_t)
- **MQTT**: Integer value (divide by 100 for BAR)
- **Range**: -327.68 to +327.67 BAR
- **Precision**: 0.01 BAR

**Examples**:
- 150 = 1.50 BAR
- 235 = 2.35 BAR
- 152 = 1.52 BAR

### Schedule Days Array
Days of week as integers (1=Monday, 7=Sunday):
- `[1,2,3,4,5]` = Weekdays
- `[6,7]` = Weekend
- `[1,2,3,4,5,6,7]` = Every day

**Note**: Different from old HotWaterScheduler which used bitmask!

---

## Error Codes

Common error codes in notifications:

| Code | Name | Description |
|------|------|-------------|
| 1 | MEMORY_ALLOCATION_FAILED | Heap exhausted |
| 2 | RELAY_OPERATION_FAILED | Relay control error |
| 3 | SENSOR_READ_FAILED | Sensor communication timeout |
| 4 | COMMUNICATION_TIMEOUT | Modbus timeout |
| 5 | SAFETY_CHECK_FAILED | Safety interlock failed |
| 6 | INITIALIZATION_FAILED | Startup error |
| 7 | PRESSURE_EXCEEDED | Pressure out of range |
| 8 | TEMPERATURE_EXCEEDED | Temperature too high |
| 9 | BURNER_IGNITION_FAILED | Failed to ignite |
| 10 | SYSTEM_FAILSAFE_TRIGGERED | Emergency stop |

---

## Rate Limiting & Throttling

### MQTT Command Rate Limit
**Current**: No rate limiting (future enhancement recommended)

**Recommended**: 100ms minimum between commands

### Sensor Publishing
**Rate**: Fixed at 10 seconds
**Override**: Not currently supported via MQTT

### Error Logging with Exponential Backoff
Repeated errors are rate-limited:
- First: Immediate
- Second: 1 second
- Third: 2 seconds
- Fourth: 4 seconds
- ...exponential...
- Max: 5 minutes

---

## MQTT Configuration

### Compile-Time Settings
**File**: `src/config/ProjectConfig.h`

```cpp
#define MQTT_SERVER "192.168.16.16"
#define MQTT_PORT 1883
#define MQTT_CLIENT_ID DEVICE_HOSTNAME  // "ESPlan-Boiler"
#define MQTT_RECONNECT_INTERVAL_MS 5000
#define MQTT_KEEP_ALIVE_SECONDS 60
```

### Queue Configuration
**File**: `src/modules/tasks/MQTTTask.cpp`

```cpp
// High priority queue
QueueConfig highPriorityConfig = {
    .length = 3,
    .itemSize = sizeof(MQTTPublishRequest),  // 392 bytes
    .overflowStrategy = OverflowStrategy::DROP_LOWEST_PRIORITY
};

// Normal priority queue
QueueConfig normalPriorityConfig = {
    .length = 5,
    .itemSize = sizeof(MQTTPublishRequest),
    .overflowStrategy = OverflowStrategy::DROP_LOWEST_PRIORITY
};
```

---

## Diagnostic Topics (Implemented)

### Task Information
**Topic**: `boiler/status/diagnostics/tasks`
**Trigger**: MQTT command or periodic
**Payload**: FreeRTOS task status

```json
{
  "tasks": [
    {
      "name": "BurnerControl",
      "state": "Running",
      "priority": 5,
      "stack_free": 1024,
      "cpu_percent": 2.1
    },
    ...
  ]
}
```

### Memory Statistics
**Topic**: `boiler/status/diagnostics/memory`

```json
{
  "heap": {
    "free": 112472,
    "min_free": 108956,
    "largest_block": 57332
  },
  "tasks": {
    "total_allocated": 25600
  }
}
```

---

## Testing Utilities

### Check Device Connection
```bash
mosquitto_sub -h 192.168.16.16 -u YOUR_MQTT_USER -P pass \
  -t "boiler/status/online" -C 1 -W 2

# Expected: {"online": true}
```

### Monitor Sensor Data with jq
```bash
mosquitto_sub -h 192.168.16.16 -u YOUR_MQTT_USER -P pass \
  -t "boiler/status/sensors" | jq '{
    boiler_out: (.t.bo / 10),
    water_tank: (.t.wt / 10),
    pressure: (.p / 100),
    room_temp: (.t.i / 10)
  }'
```

### Wait for Schedule Event
```bash
# Wait for next schedule start/end
mosquitto_sub -h 192.168.16.16 -u YOUR_MQTT_USER -P pass \
  -t "boiler/scheduler/event" -C 1

# Timeout after 5 minutes
timeout 300 mosquitto_sub -t "boiler/scheduler/event" -C 1
```

### Subscribe with Pattern Matching
```bash
# All sensor and status topics
mosquitto_sub -t "boiler/status/+" -v

# All commands (for debugging)
mosquitto_sub -t "boiler/cmd/#" -v

# Everything (including responses)
mosquitto_sub -t "boiler/#" -v
```

---

## MQTT Client Libraries

### Python (paho-mqtt)
```python
import paho.mqtt.client as mqtt
import json

def on_message(client, userdata, msg):
    if msg.topic == "boiler/status/sensors":
        data = json.loads(msg.payload)
        boiler_temp = data['t']['bo'] / 10.0
        pressure = data['p'] / 100.0
        print(f"Boiler: {boiler_temp}°C, Pressure: {pressure} BAR")

client = mqtt.Client()
client.username_pw_set("YOUR_MQTT_USER", "YOUR_MQTT_PASSWORD")
client.on_message = on_message
client.connect("192.168.16.16", 1883, 60)
client.subscribe("boiler/status/sensors")
client.loop_forever()
```

### Node.js (mqtt.js)
```javascript
const mqtt = require('mqtt');
const client = mqtt.connect('mqtt://192.168.16.16', {
  username: 'YOUR_MQTT_USER',
  password: 'YOUR_MQTT_PASSWORD'
});

client.on('message', (topic, message) => {
  if (topic === 'boiler/status/sensors') {
    const data = JSON.parse(message);
    const boilerTemp = data.t.bo / 10;
    const pressure = data.p / 100;
    console.log(`Boiler: ${boilerTemp}°C, Pressure: ${pressure} BAR`);
  }
});

client.subscribe('boiler/status/sensors');
```

### Home Assistant Integration
```yaml
# configuration.yaml
mqtt:
  sensor:
    - name: "Boiler Output Temperature"
      state_topic: "boiler/status/sensors"
      value_template: "{{ value_json.t.bo | float / 10 }}"
      unit_of_measurement: "°C"
      device_class: temperature

    - name: "System Pressure"
      state_topic: "boiler/status/sensors"
      value_template: "{{ value_json.p | float / 100 }}"
      unit_of_measurement: "BAR"
      device_class: pressure

    - name: "Water Tank Temperature"
      state_topic: "boiler/status/sensors"
      value_template: "{{ value_json.t.wt | float / 10 }}"
      unit_of_measurement: "°C"
      device_class: temperature

  binary_sensor:
    - name: "Boiler Online"
      state_topic: "boiler/status/online"
      value_template: "{{ value_json.online }}"
      payload_on: true
      payload_off: false

  switch:
    - name: "Water Heating"
      command_topic: "boiler/cmd/water"
      payload_on: "enable"
      payload_off: "disable"
```

---

## Security Considerations

### Current Implementation
- Username/password authentication
- No TLS/SSL (LAN only)
- No message signing
- No command authorization

### Recommended for Production
1. **Enable TLS**: Encrypt MQTT traffic
2. **ACL**: Restrict topics per user
3. **Command Validation**: Reject invalid ranges
4. **Audit Log**: Track all command executions

### Network Security
- Firewall: Block external access to MQTT port (1883/8883)
- VLAN: Isolate IoT devices
- VPN: Remote access only via VPN

---

## Troubleshooting

### Device Not Publishing
**Check**:
1. Network connected: `ping 192.168.16.138`
2. MQTT online status: `mosquitto_sub -t "boiler/status/online" -C 1 -W 5`
3. Broker logs: Check mosquitto.log for connection attempts

### Commands Not Working
**Check**:
1. Topic spelling: Must match exactly
2. JSON format: Use `jq` to validate
3. Credentials: Verify username/password
4. Device logs: Check serial output for command reception

### Sensor Data Not Updating
**Check**:
1. Subscribe to correct topic: `boiler/status/sensors`
2. Wait 10 seconds (publication interval)
3. Check for Modbus errors in device logs

### Schedules Not Activating
**Check**:
1. RTC time correct: Check device logs for "DS3231 initialized"
2. Schedule enabled: List schedules and verify
3. Days array correct: 1=Monday (not 0!)
4. Time zone: System uses CET/CEST

---

## MQTT Topic Migration

### Old (HotWaterScheduler) vs New (TimerScheduler)

| Old Topic | New Topic | Notes |
|-----------|-----------|-------|
| `esp32/boiler/control/hotwater/schedule/add` | `boiler/cmd/scheduler/add` | Different JSON format |
| `esp32/boiler/status/hotwater/schedule` | `boiler/scheduler/response` | Moved to scheduler namespace |
| N/A | `boiler/scheduler/event` | New: schedule start/end events |

**Breaking Changes**:
- Days format: Bitmask → Array of integers
- Topic prefix: `esp32/boiler/` → `boiler/`
- Schedule type: Must specify `"type": "water_heating"`

---

## Future MQTT API Extensions

**Planned** (from IMPROVEMENT_OPPORTUNITIES.md):
- Stack usage monitoring (`boiler/status/diagnostics/stacks`)
- Pressure trend analysis (`boiler/status/trends/pressure`)
- Efficiency metrics (`boiler/status/efficiency`)
- PID tuning history (`boiler/status/pid/history`)
- Remote PID auto-tune trigger (`boiler/cmd/pid/autotune`)

---

## Quick Reference Card

```
# Read sensor data
mosquitto_sub -t "boiler/status/sensors" -C 1

# Add water heating schedule (JSON)
mosquitto_pub -t "boiler/cmd/scheduler/add" -m '{...}'

# Remove schedule
mosquitto_pub -t "boiler/cmd/scheduler/remove" -m '{"id": 3}'

# List all schedules
mosquitto_pub -t "boiler/cmd/scheduler/list" -m '{}'

# Enable/disable heating
mosquitto_pub -t "boiler/cmd/heating" -m "enable"
mosquitto_pub -t "boiler/cmd/heating" -m "disable"

# Get error log (last 20)
mosquitto_pub -t "errors/list" -m "20"

# Clear errors
mosquitto_pub -t "errors/clear" -m ""

# Test echo
mosquitto_pub -t "test/echo" -m "hello"
# Response on: test/response

# Monitor everything
mosquitto_sub -t "boiler/#" -v
```
