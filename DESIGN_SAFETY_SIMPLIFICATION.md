# Safety System Simplification Design

## Overview

Simplify the safety system by removing redundant/counterproductive checks and making essential parameters configurable via MQTT.

## Changes Summary

| # | Feature | Action | Default | Range | MQTT Topic |
|---|---------|--------|---------|-------|------------|
| 1 | Rate-of-Change | **REMOVE** | - | - | - |
| 2 | Pump Protection | **Configurable** | 15s | 5-60s | `boiler/cmd/config/pump_protection_ms` |
| 3 | Sensor Fallback | **Configurable** | 60s | 30-300s | `boiler/cmd/config/sensor_stale_ms` |
| 4 | Thermal Runaway | **REMOVE** | - | - | - |
| 5 | Post-Purge | **Configurable** | 90s | 30-180s | `boiler/cmd/config/post_purge_ms` |
| 6 | Cross-Validation | **REMOVE** | - | - | - |

---

## What Gets Removed

### 1. Rate-of-Change Detection
- **Location**: `BurnerSafetyValidator.cpp:295` (`calculateRateOfChange()`)
- **Why remove**: Over-temp limit (90°C) is sufficient; rate check too aggressive during normal heat-up
- **Code to remove**:
  - `calculateRateOfChange()` function
  - `temperatureHistory` vector
  - `temperatureTimestamps` vector
  - `updateTemperatureHistory()` function
  - Rate check in `validateBurnerOperation()`
- **Memory saved**: ~288 bytes (history buffers)

### 2. Thermal Runaway Detection (80°C + rate)
- **Location**: `BurnerSafetyValidator.cpp`
- **Why remove**: Redundant with 90°C hard over-temp limit
- **Code to remove**: Thermal runaway check logic

### 3. Sensor Cross-Validation
- **Location**: `BurnerSafetyValidator.cpp:277` (`crossValidateSensors()`)
- **Why remove**: Physically nonsensical - output/return SHOULD differ by 10-25°C during normal operation
- **Code to remove**:
  - `crossValidateSensors()` function
  - `CROSS_VALIDATION_FAILED` enum value
  - Cross-validation call in `validateBurnerOperation()`

---

## What Gets Made Configurable

### 1. Pump Protection Interval
- **Location**: `RelayControlTask.cpp:620`
- **Current**: Hardcoded 30000ms
- **New default**: 15000ms (15s)
- **Range**: 5000-60000ms
- **Storage**: NVS key `pump_prot_ms`

### 2. Sensor Staleness Timeout
- **Location**: `TemperatureSensorFallback.cpp` / sensor validation
- **Current**: Hardcoded 60000ms
- **New default**: 60000ms (unchanged)
- **Range**: 30000-300000ms
- **Storage**: NVS key `sensor_stale_ms`

### 3. Post-Purge Duration
- **Location**: `PumpCoordinator.cpp` / burner shutdown sequence
- **Current**: Hardcoded 30000ms
- **New default**: 90000ms (90s)
- **Range**: 30000-180000ms
- **Storage**: NVS key `post_purge_ms`

---

## Configuration Architecture

### New Header: `include/config/SafetyConfig.h`

```cpp
#pragma once
#include <cstdint>

namespace SafetyConfig {
    // Compile-time defaults
    namespace Defaults {
        constexpr uint32_t PUMP_PROTECTION_MS = 15000;      // 15s
        constexpr uint32_t SENSOR_STALE_MS = 60000;         // 60s
        constexpr uint32_t POST_PURGE_MS = 90000;           // 90s
    }

    // Valid ranges
    namespace Limits {
        constexpr uint32_t PUMP_PROTECTION_MIN_MS = 5000;   // 5s
        constexpr uint32_t PUMP_PROTECTION_MAX_MS = 60000;  // 60s

        constexpr uint32_t SENSOR_STALE_MIN_MS = 30000;     // 30s
        constexpr uint32_t SENSOR_STALE_MAX_MS = 300000;    // 5min

        constexpr uint32_t POST_PURGE_MIN_MS = 30000;       // 30s
        constexpr uint32_t POST_PURGE_MAX_MS = 180000;      // 3min
    }

    // Runtime values (loaded from NVS, modifiable via MQTT)
    extern uint32_t pumpProtectionMs;
    extern uint32_t sensorStaleMs;
    extern uint32_t postPurgeMs;

    // Initialize from NVS (call at startup)
    void loadFromNVS();

    // Save to NVS (call after MQTT update)
    void saveToNVS();

    // Validate and set (returns false if out of range)
    bool setPumpProtection(uint32_t ms);
    bool setSensorStale(uint32_t ms);
    bool setPostPurge(uint32_t ms);
}
```

### New Source: `src/config/SafetyConfig.cpp`

```cpp
#include "config/SafetyConfig.h"
#include <Preferences.h>

namespace SafetyConfig {
    // Runtime values initialized to defaults
    uint32_t pumpProtectionMs = Defaults::PUMP_PROTECTION_MS;
    uint32_t sensorStaleMs = Defaults::SENSOR_STALE_MS;
    uint32_t postPurgeMs = Defaults::POST_PURGE_MS;

    static Preferences prefs;
    static const char* NVS_NAMESPACE = "safety";

    void loadFromNVS() {
        prefs.begin(NVS_NAMESPACE, true);  // read-only
        pumpProtectionMs = prefs.getUInt("pump_prot", Defaults::PUMP_PROTECTION_MS);
        sensorStaleMs = prefs.getUInt("sensor_stale", Defaults::SENSOR_STALE_MS);
        postPurgeMs = prefs.getUInt("post_purge", Defaults::POST_PURGE_MS);
        prefs.end();

        // Validate loaded values
        if (pumpProtectionMs < Limits::PUMP_PROTECTION_MIN_MS ||
            pumpProtectionMs > Limits::PUMP_PROTECTION_MAX_MS) {
            pumpProtectionMs = Defaults::PUMP_PROTECTION_MS;
        }
        // ... similar for others
    }

    void saveToNVS() {
        prefs.begin(NVS_NAMESPACE, false);  // read-write
        prefs.putUInt("pump_prot", pumpProtectionMs);
        prefs.putUInt("sensor_stale", sensorStaleMs);
        prefs.putUInt("post_purge", postPurgeMs);
        prefs.end();
    }

    bool setPumpProtection(uint32_t ms) {
        if (ms < Limits::PUMP_PROTECTION_MIN_MS || ms > Limits::PUMP_PROTECTION_MAX_MS) {
            return false;
        }
        pumpProtectionMs = ms;
        return true;
    }

    // ... similar for others
}
```

---

## MQTT Integration

### New Topics in `MQTTTopics.h`

```cpp
#define MQTT_CMD_CONFIG_PUMP_PROTECTION  "boiler/cmd/config/pump_protection_ms"
#define MQTT_CMD_CONFIG_SENSOR_STALE     "boiler/cmd/config/sensor_stale_ms"
#define MQTT_CMD_CONFIG_POST_PURGE       "boiler/cmd/config/post_purge_ms"

#define MQTT_STATUS_SAFETY_CONFIG        "boiler/status/safety_config"
```

### Handler in `MQTTTask.cpp`

```cpp
void handleSafetyConfigMessage(const char* topic, const char* payload) {
    uint32_t value = atol(payload);
    bool success = false;

    if (strcmp(topic, MQTT_CMD_CONFIG_PUMP_PROTECTION) == 0) {
        success = SafetyConfig::setPumpProtection(value);
    } else if (strcmp(topic, MQTT_CMD_CONFIG_SENSOR_STALE) == 0) {
        success = SafetyConfig::setSensorStale(value);
    } else if (strcmp(topic, MQTT_CMD_CONFIG_POST_PURGE) == 0) {
        success = SafetyConfig::setPostPurge(value);
    }

    if (success) {
        SafetyConfig::saveToNVS();
        publishSafetyConfig();  // Confirm new values
    } else {
        LOG_WARN(TAG, "Invalid safety config value: %s = %lu", topic, value);
    }
}

void publishSafetyConfig() {
    char json[128];
    snprintf(json, sizeof(json),
        "{\"pump_prot\":%lu,\"sensor_stale\":%lu,\"post_purge\":%lu}",
        SafetyConfig::pumpProtectionMs,
        SafetyConfig::sensorStaleMs,
        SafetyConfig::postPurgeMs);

    mqttPublish(MQTT_STATUS_SAFETY_CONFIG, json);
}
```

---

## Files to Modify

### Remove Code:
- `src/modules/control/BurnerSafetyValidator.cpp`
  - Remove `calculateRateOfChange()`
  - Remove `crossValidateSensors()`
  - Remove temperature history buffers
  - Remove thermal runaway logic
  - Remove cross-validation calls
- `include/modules/control/BurnerSafetyValidator.h`
  - Remove related declarations
  - Remove `CROSS_VALIDATION_FAILED` enum

### Add Configurable Parameters:
- `src/modules/tasks/RelayControlTask.cpp` - Use `SafetyConfig::pumpProtectionMs`
- `src/modules/control/PumpCoordinator.cpp` - Use `SafetyConfig::postPurgeMs`
- Sensor validation code - Use `SafetyConfig::sensorStaleMs`

### New Files:
- `include/config/SafetyConfig.h`
- `src/config/SafetyConfig.cpp`

### Update:
- `include/MQTTTopics.h` - Add new topics
- `src/modules/tasks/MQTTTask.cpp` - Add handlers
- `src/main.cpp` - Call `SafetyConfig::loadFromNVS()` at startup
- `docs/ALGORITHMS.md` - Update documentation

---

## What Remains for Safety

After simplification, these protections remain:

1. **90°C hard over-temp limit** - Emergency shutdown
2. **Pump interlock** - Pump must run before burner enabled
3. **Sensor validity checks** - Invalid sensor triggers safe state
4. **Sensor staleness** (configurable) - Old data triggers safe state
5. **Pump protection** (configurable) - Motor protection
6. **Post-purge** (configurable) - Heat dissipation after burner off
7. **Hardware over-temp** - Physical thermostat (independent of software)

---

## Testing Checklist

- [ ] Remove rate-of-change: Build succeeds, no references remain
- [ ] Remove cross-validation: Build succeeds, no references remain
- [ ] Remove thermal runaway: Build succeeds, over-temp still works
- [ ] Pump protection configurable: Test 5s, 15s, 60s values
- [ ] Sensor staleness configurable: Test 30s, 60s, 300s values
- [ ] Post-purge configurable: Test 30s, 90s, 180s values
- [ ] NVS persistence: Values survive reboot
- [ ] MQTT commands work: All three config topics respond correctly
- [ ] Invalid values rejected: Out-of-range values return error
- [ ] Safety config published on startup and change

---

## Implementation Order

1. Create `SafetyConfig.h/.cpp`
2. Remove dead code (rate-of-change, cross-validation, thermal runaway)
3. Update pump protection to use SafetyConfig
4. Update post-purge to use SafetyConfig
5. Update sensor staleness to use SafetyConfig
6. Add MQTT handlers
7. Add NVS load to startup
8. Update documentation
9. Test all scenarios
