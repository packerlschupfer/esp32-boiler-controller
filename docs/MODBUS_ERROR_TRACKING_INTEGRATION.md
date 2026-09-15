# Modbus Error Tracking Integration

## Overview

Modbus error tracking is implemented in the **ESP32-ModbusDevice library** (`ModbusErrorTracker`)
and is integrated in all three device libraries and in the main project.

**Architecture:**
- **ESP32-ModbusDevice**: Core tracking logic (`modbus::ModbusErrorTracker`, error categorization, statistics)
- **Device Libraries** (MB8ART, RYN4, ANDRTF3): Record errors/successes after Modbus operations
- **Main Project**: Queries stats and publishes them to MQTT (`src/modules/tasks/MonitoringTask.cpp`)

---

## Current Integration

**ESP32-ModbusDevice library:**
- `modbus::ModbusErrorTracker` static class, counters are `std::atomic`
- Error categories (`ModbusErrorTracker::ErrorCategory`): `CRC_ERROR`, `TIMEOUT`, `INVALID_DATA`, `DEVICE_ERROR`, `OTHER`
- Per-device counters, up to `MODBUS_ERROR_TRACKER_MAX_DEVICES` (default 8)
- No `init()` needed (static initialization)

**Device libraries** (call sites of `recordError()` / `recordSuccess()`):

| Library | Where tracking is called |
|---------|--------------------------|
| ESP32-MB8ART | `MB8ART.cpp`, `MB8ARTModbus.cpp`, `MB8ARTConfig.cpp` |
| ESP32-RYN4 | `RYN4_TRACK_*` macros (`RYN4Logging.h`) used in `RYN4Control.cpp`, `RYN4Modbus.cpp`, `RYN4State.cpp`, `RYN4Config.cpp`, `RYN4AdvancedConfig.cpp` |
| ESP32-ANDRTF3 | `ANDRTF3.cpp` |

**Main project:**
- `publishModbusErrorStats()` in `src/modules/tasks/MonitoringTask.cpp`
- Called from the detailed monitoring report, timer `DETAILED_MONITOR_INTERVAL_MS` = 1800000 ms
  (**every 30 minutes**, `src/config/SystemConstants.h`)
- One message per device to `boiler/diagnostics/modbus/{address}`
  (`MQTT_DIAGNOSTICS_MODBUS_PREFIX` in `include/MQTTTopics.h`)
- Address is formatted as two hex digits (`%02X`), QoS 0, not retained, low priority

---

## Device Addresses

From `src/config/ProjectConfig.h`:

| Device | Address | Topic | Purpose |
|--------|---------|-------|---------|
| RYN4 | 0x02 | `boiler/diagnostics/modbus/02` | 8-channel relay module |
| MB8ART | 0x03 | `boiler/diagnostics/modbus/03` | 8-channel temperature sensors |
| ANDRTF3 | 0x04 | `boiler/diagnostics/modbus/04` | Room temperature sensor |

Stats are published in the order MB8ART, RYN4, ANDRTF3.

---

## Monitor MQTT Topics

```bash
# Subscribe to diagnostics
mosquitto_sub -h BROKER_IP -u USER -P PASS -t "boiler/diagnostics/modbus/#" -v
```

---

## MQTT Output (Every 30 Minutes)

Example values:

```json
boiler/diagnostics/modbus/03 {
  "address": 3,
  "crc_errors": 12,
  "timeouts": 0,
  "invalid_data": 0,
  "device_errors": 0,
  "other_errors": 0,
  "success_count": 1523,
  "total_errors": 12,
  "error_rate_pct": 0.78,
  "last_error_ms_ago": 125340
}

boiler/diagnostics/modbus/02 {
  "address": 2,
  "crc_errors": 3,
  "timeouts": 1,
  "invalid_data": 0,
  "device_errors": 0,
  "other_errors": 0,
  "success_count": 982,
  "total_errors": 4,
  "error_rate_pct": 0.41,
  "last_error_ms_ago": 42100
}

boiler/diagnostics/modbus/04 {
  "address": 4,
  "crc_errors": 0,
  "timeouts": 0,
  "invalid_data": 0,
  "device_errors": 0,
  "other_errors": 0,
  "success_count": 245,
  "total_errors": 0,
  "error_rate_pct": 0.0
}
```

**Field notes:**
- `address` is decimal in the payload; the topic suffix is hex
- `error_rate_pct` = total errors / (total errors + successes) x 100
- `last_error_ms_ago` is omitted when the device has had no error since boot (or since reset)
- Counters are in RAM and start from zero after each reboot

---

## Interpretation of Results

### Healthy Device Example
```json
{
  "address": 3,
  "crc_errors": 2,      // Few CRC errors (EMI is minimal)
  "timeouts": 0,        // No timeouts (device responsive)
  "error_rate_pct": 0.13
}
```
**Diagnosis:** Device is healthy, minimal bus noise

### Bus Noise Problem
```json
{
  "address": 3,
  "crc_errors": 127,    // Many CRC errors (EMI/noise)
  "timeouts": 3,        // Few timeouts
  "error_rate_pct": 5.2
}
```
**Diagnosis:** Bus noise issue - check wiring, shielding, grounding

### Failing Device
```json
{
  "address": 2,
  "crc_errors": 2,
  "timeouts": 48,       // Many timeouts (device unresponsive)
  "error_rate_pct": 15.7
}
```
**Diagnosis:** Device is failing or losing power - check connections

---

## Important Notes

### Diagnostic Only

The error tracker only counts results - it does not change communication behavior:

- Tracks errors by category
- Stats are published to MQTT by the main project
- Does NOT add retries
- Does NOT change the ModbusCoordinator schedule

It helps answer:
- "Is this CRC error (bus noise) or timeout (device failure)?"
- "Which device has the most problems?"
- "Error rate trending over time"

---

## API Reference (ESP32-ModbusDevice)

### Recording Errors/Successes (device libraries)

```cpp
#include <ModbusErrorTracker.h>

// After Modbus operation
if (result.isError()) {
    auto category = modbus::ModbusErrorTracker::categorizeError(result.error());
    modbus::ModbusErrorTracker::recordError(deviceAddress, category);
} else {
    modbus::ModbusErrorTracker::recordSuccess(deviceAddress);
}
```

### Querying Statistics (main project)

```cpp
uint32_t crc = modbus::ModbusErrorTracker::getCrcErrors(address);
uint32_t timeouts = modbus::ModbusErrorTracker::getTimeouts(address);
uint32_t invalidData = modbus::ModbusErrorTracker::getInvalidDataErrors(address);
uint32_t deviceErrors = modbus::ModbusErrorTracker::getDeviceErrors(address);
uint32_t otherErrors = modbus::ModbusErrorTracker::getOtherErrors(address);
uint32_t successCount = modbus::ModbusErrorTracker::getSuccessCount(address);
uint32_t totalErrors = modbus::ModbusErrorTracker::getTotalErrors(address);
float errorRate = modbus::ModbusErrorTracker::getErrorRate(address);  // Percentage
uint32_t lastErrorTime = modbus::ModbusErrorTracker::getLastErrorTime(address);  // millis(), 0 = none
uint8_t tracked = modbus::ModbusErrorTracker::getTrackedDeviceCount();
bool known = modbus::ModbusErrorTracker::isDeviceTracked(address);
const char* name = modbus::ModbusErrorTracker::categoryToString(category);
```

### Resetting Statistics

```cpp
modbus::ModbusErrorTracker::resetDevice(address);
modbus::ModbusErrorTracker::resetAll();
```

No MQTT command resets the counters; they reset on reboot.

---

## Troubleshooting

### MQTT Messages Not Published

**Check 1:** Is MonitoringTask running?
```bash
mosquitto_sub -h BROKER_IP -u USER -P PASS -t "boiler/status/health" -v
```

**Check 2:** Wait up to 30 minutes after boot for the first detailed report

**Check 3:** In debug builds, check serial logs for "Modbus stats 0x.." debug messages

### A Device Shows Only Zeros

- The device has not been polled yet, or it is not on the bus at the configured address
- Verify the address table above against the device configuration

---

**Document Version**: 3.0.0
**Last Updated**: 2026-09-15
**Status**: Implemented in ESP32-ModbusDevice, all device libraries and main project
