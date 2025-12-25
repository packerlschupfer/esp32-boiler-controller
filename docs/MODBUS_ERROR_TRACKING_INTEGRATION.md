# Modbus Error Tracking Integration Guide

## Overview

Modbus error tracking is now implemented in the **ESP32-ModbusDevice library** (commit a3eb56e). This provides reusable error diagnostics for all Modbus-based device libraries.

**Architecture:**
- **ESP32-ModbusDevice**: Core tracking logic (error categorization, statistics)
- **Device Libraries** (MB8ART, RYN4, ANDRTF3): Record errors/successes after operations
- **Main Project**: Queries stats and publishes to MQTT

---

## What's Already Done

✅ **In ESP32-ModbusDevice library:**
- `ModbusErrorTracker` class with thread-safe statistics
- Error categorization: CRC_ERROR, TIMEOUT, INVALID_DATA, DEVICE_ERROR, OTHER
- Per-device counters (up to 8 devices)
- Query API for error rates, counts, timestamps

✅ **In main project (esp32-boiler-controller):**
- MQTT publishing function in `MonitoringTask.cpp`
- Publishes stats every 5 minutes to `boiler/diagnostics/modbus/{address}`
- JSON format with error breakdown and rates

❌ **What's NOT done:**
- Device libraries don't call tracking functions yet
- Stats will show zeros until libraries are updated

---

## Integration Steps for Device Libraries

### Step 1: Update ESP32-MB8ART

**File:** `ESP32-MB8ART/src/MB8ART.cpp`

**Add tracking after Modbus operations:**

```cpp
#include <ModbusErrorTracker.h>

// Example: After reading temperature
Temperature_t MB8ART::readTemperature(uint8_t channel) {
    auto result = modbusDevice_->readInputRegisters(baseAddress_ + channel, 1);

    if (result.isError()) {
        // Record error with categorization
        auto category = ModbusErrorTracker::categorizeError(result.error());
        ModbusErrorTracker::recordError(address_, category);

        // Existing error handling...
        return INVALID_TEMPERATURE;
    }

    // Record success
    ModbusErrorTracker::recordSuccess(address_);

    // Existing success handling...
    return convertToTemperature(result.value());
}
```

**Integration points:**
- `readTemperature()` - After each channel read
- `readAllChannels()` - After batch operation
- Any other Modbus read operations

---

### Step 2: Update ESP32-RYN4

**File:** `ESP32-RYN4/src/RYN4.cpp`

```cpp
#include <ModbusErrorTracker.h>

// Example: After setting relay state
bool RYN4::setRelay(uint8_t relay, bool state) {
    auto result = modbusDevice_->writeSingleCoil(relay, state);

    if (result.isError()) {
        auto category = ModbusErrorTracker::categorizeError(result.error());
        ModbusErrorTracker::recordError(address_, category);
        return false;
    }

    ModbusErrorTracker::recordSuccess(address_);
    return true;
}

// Example: After reading relay states
uint8_t RYN4::readRelayStates() {
    auto result = modbusDevice_->readCoils(0, 8);

    if (result.isError()) {
        auto category = ModbusErrorTracker::categorizeError(result.error());
        ModbusErrorTracker::recordError(address_, category);
        return 0;
    }

    ModbusErrorTracker::recordSuccess(address_);
    return result.value();
}
```

**Integration points:**
- `setRelay()` / `setMultipleRelays()` - After write operations
- `readRelayStates()` - After read operations
- DELAY command operations

---

### Step 3: Update ESP32-ANDRTF3

**File:** `ESP32-ANDRTF3/src/ANDRTF3.cpp`

```cpp
#include <ModbusErrorTracker.h>

// Example: After reading temperature
Temperature_t ANDRTF3::readTemperature() {
    auto result = modbusDevice_->readInputRegisters(TEMP_REGISTER, 1);

    if (result.isError()) {
        auto category = ModbusErrorTracker::categorizeError(result.error());
        ModbusErrorTracker::recordError(address_, category);
        return INVALID_TEMPERATURE;
    }

    ModbusErrorTracker::recordSuccess(address_);
    return convertToTemperature(result.value());
}
```

**Integration points:**
- `readTemperature()` - After temperature read
- `readHumidity()` - If supported
- Any other sensor reads

---

## Device Addresses

Make sure you use the correct Modbus addresses:

| Device | Address | Purpose |
|--------|---------|---------|
| MB8ART | 0x01 | 8-channel temperature sensors |
| RYN4 | 0x02 | 8-channel relay module |
| ANDRTF3 | 0x03 | Room temperature sensor |

---

## After Library Integration

### 1. Rebuild Libraries

```bash
cd ESP32-MB8ART
pio run

cd ESP32-RYN4
pio run

cd ESP32-ANDRTF3
pio run
```

### 2. Clean and Rebuild Main Project

```bash
cd esp32-boiler-controller
rm -rf .pio
pio run -e esp32dev_usb_debug_selective
```

### 3. Upload and Monitor

```bash
pio run -e esp32dev_usb_debug_selective -t upload --upload-port /dev/ttyACM0
```

### 4. Monitor MQTT Topics

```bash
# Subscribe to diagnostics
mosquitto_sub -h BROKER_IP -u USER -P PASS -t "boiler/diagnostics/modbus/#" -v
```

---

## Expected MQTT Output (Every 5 Minutes)

```json
boiler/diagnostics/modbus/01 {
  "address": 1,
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

boiler/diagnostics/modbus/03 {
  "address": 3,
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

---

## Interpretation of Results

### Healthy Device Example
```json
{
  "address": 1,
  "crc_errors": 2,      // Few CRC errors (EMI is minimal)
  "timeouts": 0,        // No timeouts (device responsive)
  "error_rate_pct": 0.13
}
```
**Diagnosis:** Device is healthy, minimal bus noise

### Bus Noise Problem
```json
{
  "address": 1,
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

### No Retry Logic Added

The error tracking is **diagnostic only** - it does NOT change retry behavior:

- ✅ Tracks errors by category
- ✅ Publishes statistics to MQTT
- ❌ Does NOT add automatic retries
- ❌ Does NOT change ModbusCoordinator tick schedule

Your fixed tick schedule remains intact!

### Why No Retries?

With your fixed tick schedule:
```
Tick 0: READ MB8ART, READ ANDRTF3, READ RYN4
Tick 1: WRITE RYN4
Tick 2: READ MB8ART, READ ANDRTF3, READ RYN4
Tick 3: WRITE RYN4
```

Adding automatic retry would break the schedule. The tracker just helps you understand:
- "Is this CRC error (bus noise) or timeout (device failure)?"
- "Which device has the most problems?"
- "Error rate trending over time"

---

## API Reference (ESP32-ModbusDevice)

### Recording Errors/Successes

```cpp
// After Modbus operation
if (result.isError()) {
    auto category = ModbusErrorTracker::categorizeError(result.error());
    ModbusErrorTracker::recordError(deviceAddress, category);
} else {
    ModbusErrorTracker::recordSuccess(deviceAddress);
}
```

### Querying Statistics (Main Project)

```cpp
uint32_t crc = ModbusErrorTracker::getCrcErrors(address);
uint32_t timeouts = ModbusErrorTracker::getTimeouts(address);
uint32_t invalidData = ModbusErrorTracker::getInvalidDataErrors(address);
uint32_t deviceErrors = ModbusErrorTracker::getDeviceErrors(address);
uint32_t otherErrors = ModbusErrorTracker::getOtherErrors(address);
uint32_t successCount = ModbusErrorTracker::getSuccessCount(address);
uint32_t totalErrors = ModbusErrorTracker::getTotalErrors(address);
float errorRate = ModbusErrorTracker::getErrorRate(address);  // Percentage
uint32_t lastErrorTime = ModbusErrorTracker::getLastErrorTime(address);
```

### Resetting Statistics

```cpp
ModbusErrorTracker::resetDevice(address);
```

---

## Troubleshooting

### MQTT Shows All Zeros

**Problem:** Device libraries haven't integrated tracking calls yet

**Solution:** Follow integration steps above for each library

### MQTT Messages Not Published

**Check 1:** Is MonitoringTask running?
```bash
mosquitto_sub -h BROKER_IP -u USER -P PASS -t "boiler/status/health" -v
```

**Check 2:** Check serial logs for "Modbus stats" debug messages

---

## Summary

**Current State:**
- Framework ✅ Implemented in ESP32-ModbusDevice (commit a3eb56e)
- Main project ✅ Publishing enabled (will show zeros until library integration)
- Device libraries ❌ Not integrated yet (requires modifications)

**Next Steps:**
1. Add `ModbusErrorTracker::recordError()` and `recordSuccess()` calls to device libraries
2. Rebuild libraries: `cd ~/Documents/PlatformIO/git/ESP32-{MB8ART,RYN4,ANDRTF3} && pio run`
3. Clean main project: `rm -rf .pio`
4. Build and upload main project
5. Monitor MQTT topic `boiler/diagnostics/modbus/#`

**Benefits:**
- Distinguish bus noise (CRC) from device failures (timeout)
- Track error trends over time
- Identify problematic devices
- Remote diagnostics without serial access

---

**Document Version**: 2.0.0
**Last Updated**: 2026-01-01
**Status**: Main project ready, device libraries pending
