# Control Algorithms Documentation

This document describes the key algorithms used in the ESP32 Boiler Controller for safety monitoring, temperature control, and system coordination.

---

## 1. Rate-of-Change Detection (Thermal Runaway Prevention)

### Purpose
Detects rapid temperature increases that could indicate thermal runaway, loss of circulation, or sensor malfunction.

### Location
`src/modules/control/BurnerSafetyValidator.cpp:295`

### Algorithm

**Formula:**
```
Rate = (T_current - T_past) × 60000 / Δt_ms

Where:
- T_current = Most recent temperature (tenths of °C)
- T_past = Temperature from timeSpan ago
- Δt_ms = Actual time elapsed in milliseconds
- 60000 = Conversion factor (ms to minutes)
- Result = Temperature change in tenths of °C per minute
```

**Implementation:**
```cpp
Temperature_t calculateRateOfChange(
    const std::vector<Temperature_t>& history,
    uint32_t timeSpanMs) {

    // Find entry from timeSpanMs ago
    for (size_t i = timestamps.size() - 1; i > 0; i--) {
        if (now - timestamps[i] >= timeSpanMs) {
            startIdx = i;
            break;
        }
    }

    // Calculate: (newest - oldest) / time
    Temperature_t tempChange = history[0] - history[startIdx];
    uint32_t timeMs = now - timestamps[startIdx];

    // Rate per minute (avoid overflow with int32_t)
    int32_t rate = (tempChange * 60000) / timeMs;
    return static_cast<Temperature_t>(rate);
}
```

### Parameters
- **Time Window**: 60 seconds (60000 ms)
- **History Buffer**: 120 seconds maximum (circular buffer)
- **Sample Interval**: ~2.5 seconds (MB8ART sensor read rate)
- **Max Samples**: ~48 samples in buffer

### Thresholds
- **Normal Rate Limit**: Configurable via `maxTempRateOfChange` (typically 100 = 10.0°C/min)
- **Runaway Detection**: Combined with absolute temperature check:
  - Temperature > 800°C (8000 tenths) AND rate > 50 (5.0°C/min)

### Example
```
Current temp: 245 (24.5°C)
Past temp (60s ago): 220 (22.0°C)
Actual time: 60000 ms

Rate = (245 - 220) × 60000 / 60000
     = 25 × 1
     = 25 tenths/min
     = 2.5°C/min ✓ SAFE
```

### Edge Cases
- **Insufficient History**: Returns 0 if < 2 samples
- **Clock Rollover**: Uses 32-bit unsigned arithmetic (safe for ~49 days uptime)
- **Negative Rates**: Cooling is allowed (only excessive heating is flagged)

---

## 2. PID Temperature Control

### Purpose
Maintains target temperature using Proportional-Integral-Derivative control with fixed-point arithmetic.

### Location
`src/modules/control/PIDControlModuleFixedPoint.cpp`

### Algorithm

**PID Formula:**
```
output = Kp × error + Ki × integral + Kd × derivative

Where:
- error = setpoint - current_temperature
- integral = Σ(error × dt)  [accumulated over time]
- derivative = (error - previous_error) / dt
- Kp, Ki, Kd = Tuning gains
```

**Fixed-Point Implementation:**
```cpp
// PIDControlModuleFixedPoint::calculatePIDAdjustment() (simplified)
// Temperatures in tenths of °C, gains int32 scaled by PID_FIXED_POINT_SCALE (1000)
Temperature_t error = setPoint - currentTemp;

PIDValue_t P = (Kp * error) / SCALE;
PIDValue_t D = (Kd * -(currentTemp - previousPV) * SCALE / dtMs) / SCALE;  // derivative on PV
PIDValue_t I = (Ki * integral) / SCALE;

// Clamp in the wide type, THEN narrow to int16 (PIDGainFixedPoint::clampToAdjustment).
// Casting P+I+D to int16 first wrapped -44055 to +21481 (burner FULL above target).
Temperature_t tentative = PIDGainFixedPoint::clampToAdjustment(P + I + D, outputMin, outputMax);

// Anti-windup: integrate only while the output is not saturated in the error direction
if (!(tentative >= outputMax && error > 0) && !(tentative <= outputMin && error < 0)) {
    integral += (error * dtMs) / SCALE;
    integral = clamp(integral, integralMin, integralMax);
    I = (Ki * integral) / SCALE;
}

Temperature_t adjustment = PIDGainFixedPoint::clampToAdjustment(P + I + D, outputMin, outputMax);

// BoilerTempController::calculateModulating(): 50% = at target, clamped to 0-100,
// then mapped to OFF/HALF/FULL with threshold hysteresis
int32_t pidPower = 50 + (adjustment / 10);
```

### Tuning Parameters

**Boiler Temperature Control** (`BoilerTempController`, run by BoilerTempControlTask):
- **Gain sets**: `pid/spaceHeating/kp|ki|kd` and `pid/waterHeater/kp|ki|kd` (float, both 0-100 / 0-10 / 0-50)
- **Active set**: a WATER burner request selects the water gains, otherwise the space heating gains. `updateMode()` reads the active gains from SystemSettings every cycle and resets the PID when the mode or the gains change, so MQTT/UI gain changes apply without reboot
- **Gain scaling**: `PIDGainFixedPoint::fromFloat()` multiplies by 1000 and rounds; negative or non-finite gains become 0, the result saturates at INT32_MAX. Unscaled float gains were truncated (Kp 34.206 became 0.034, Ki 0.189 became 0) and left the output near 50%, so the controller never commanded OFF
- **Sample Rate**: every boiler output sensor update (~2.5 seconds)
- **Pause reset**: if more than 10 s passed since the last PID run (no active request, autotune), the PID is reset and a nominal 2.5 s step is used instead of integrating the pause
- **Output**: 0-100% (50% = at target), mapped to OFF/HALF/FULL

### Anti-Windup
- **Integral Limits**: `SafetyConfig::pidIntegralMin`/`pidIntegralMax`, default ±100000 (NVS `pid_int_min`/`pid_int_max`, allowed ±500000), set by `BoilerTempController::initialize()`. The integral accumulates error (tenths °C) × dt (ms) / 1000, so the unit is tenths-°C·s
- **Output Limits**: PID adjustment clamped to ±1000 tenths (±100.0°C, `OUTPUT_MIN`/`OUTPUT_MAX`)
- **Prevents**: Integral accumulation during saturation
- **Method**: Conditional integration (no accumulation while saturated) plus clamp to `integralMin`/`integralMax`

### Auto-Tuning Support
- **Location**: `PIDAutoTuner.cpp`
- **Method**: Relay feedback (see section 7)
- **Process**: Oscillation analysis → Ku, Tu → PID gains
- **Trigger**: MQTT `boiler/cmd/pid_autotune` with payload `start`

---

## 3. Pump Motor Protection

### Purpose
Prevents pump motor damage by enforcing minimum time between state changes.

### Location
`src/modules/tasks/RelayControlTask.cpp` (`processSingleRelay`, `checkPumpProtection`), `src/modules/tasks/RelayVerificationManager.cpp`, `include/shared/RelayCommandPolicy.h`

### Algorithm

**Rule:**
```
Minimum SafetyConfig::pumpProtectionMs (default 15 s) between pump state changes

For the heating pump and water pump relays:
  IF command does not change the desired relay state OR is an emergency command
  THEN accept without protection check (timestamp unchanged)
  ELSE IF (current_time - last_state_change_time) < pumpProtectionMs
  THEN block state change
  ELSE allow state change AND update last_state_change_time
```

**Implementation:**
```cpp
// RelayControlTask::processSingleRelay() (simplified), relayIndex is 1-based
const bool desiredState = g_relayState.getRelay(relayIndex - 1);
const bool realChange = !RelayCommandPolicy::isNoOp(desiredState, state);
const bool protect = RelayCommandPolicy::appliesProtection(desiredState, state, emergencyBypass);

if (protect && !checkRateLimit(relayIndex)) {
    return false;  // MIN_RELAY_SWITCH_INTERVAL_MS / MAX_RELAY_TOGGLE_RATE_PER_MIN
}
if (protect && !checkPumpProtection(relayIndex, state)) {
    return false;  // elapsed < SafetyConfig::pumpProtectionMs
}

g_relayState.setRelay(relayIndex - 1, state);

// Pump relays only: restart the protection window on a real state change
if (realChange && relayIndex == heatingPumpPhysical) {
    pumpLastStateChangeTime[0] = xTaskGetTickCount();
}
```

### Parameters
- **Protection Period**: `SafetyConfig::pumpProtectionMs` (default 15,000 ms, MQTT `boiler/cmd/config/pump_protection_ms`)
- **Applies To**: Heating pump and water pump relays (`RelayIndex::HEATING_PUMP`, `RelayIndex::WATER_PUMP`)
- **Does NOT Apply To**: Other relays (burner, valve, etc.), commands that do not change the relay state, emergency/failsafe commands

### Relay Rate Limiting
All relays: at least `MIN_RELAY_SWITCH_INTERVAL_MS` (150 ms) between toggles and at most `MAX_RELAY_TOGGLE_RATE_PER_MIN` (30) toggles per minute. Like pump protection it applies only to real, non-emergency state changes (`RelayCommandPolicy::appliesProtection`). `BurnerSystemController::executeRelayBatch()` re-sends unchanged burner relays on a mode switch; counting those as toggles rejected the real POWER_BOOST change 2 ms later and escalated to an emergency stop.

### Example Timeline (default 15 s)
```
t=0s:    Pump ON  → allowed (first start), timestamp t=0s
t=10s:   Pump OFF → BLOCKED (only 10s elapsed, need 15s)
t=20s:   Pump OFF → allowed (20s > 15s threshold)
         ↳ timestamp updated to t=20s
t=25s:   Pump OFF re-sent → no-op, accepted, timestamp unchanged
t=30s:   Pump ON  → BLOCKED (only 10s since the last real change)
t=35s:   Pump ON  → allowed (15s elapsed)
```

### Motor Protection Rationale
- Circulation pumps have mechanical inertia
- Rapid cycling causes:
  - Thermal stress on motor windings
  - Mechanical wear on impeller/bearings
  - Reduced pump lifespan
- Industry standard: 30-60 second minimum

---

## 4. MQTT Queue Prioritization

### Purpose
Manage MQTT message backlog with priority-based queue and backpressure handling.

### Location
`src/modules/tasks/MQTTTask.cpp:150-280`

### Algorithm

**Dual Queue System:**
```
High Priority Queue (3 messages, HIGH_PRIORITY_QUEUE_SIZE in MQTTTask.h):
- Safety alerts
- Error notifications
- Critical state changes

Normal Priority Queue (5 messages, NORMAL_PRIORITY_QUEUE_SIZE):
- Sensor data
- Status updates
- Routine telemetry

Total Capacity: 8 messages
Overflow: DROP_OLDEST (both queues)
Backpressure: MQTT_QUEUE_PRESSURE bit
```

**Queue Selection Logic:**
```cpp
bool enqueueMessage(const char* topic, const char* payload,
                   bool highPriority) {

    // Check total utilization
    int totalUsed = highQueue.size() + normalQueue.size();

    if (totalUsed >= 48) {
        // Set backpressure flag (stops non-critical publishes)
        setBackpressureFlag();
    }

    if (highPriority) {
        if (highQueue.size() < 20) {
            highQueue.push(message);
            return true;
        } else {
            // High queue full - drop message
            return false;
        }
    } else {
        if (normalQueue.size() < 40) {
            normalQueue.push(message);
            return true;
        } else {
            // Normal queue full - drop oldest
            normalQueue.pop();  // Make space
            normalQueue.push(message);
            return true;  // Dropped old, added new
        }
    }
}
```

**Publishing Priority:**
```
1. Always process high priority queue first
2. Process normal queue only if high queue empty
3. Publish rate: Max 10 msg/second (100ms between publishes)
```

### Backpressure Handling
- **Trigger**: 80% capacity (48/60 messages)
- **Action**: Set `MQTT_QUEUE_PRESSURE` event bit
- **Effect**: Tasks reduce publish frequency
- **Recovery**: Flag cleared when < 70% (42/60 messages)

### Drop Strategy
- **High Priority**: NEVER drop (queue full = reject)
- **Normal Priority**: Drop OLDEST message (FIFO eviction)
- **Rationale**: Recent data more valuable than old data

### Example Scenario
```
State: 45 messages queued (75% - no backpressure yet)
→ Add 5 high priority alerts: Total 50 (83%)
→ Backpressure triggered
→ Sensor tasks reduce publish rate
→ Process 10 messages: Total 40 (67%)
→ Backpressure cleared
```

---

## 5. Sensor Cross-Validation

### Purpose
Validate temperature readings by comparing redundant sensors, detect sensor failures.

### Location
`src/modules/control/BurnerSafetyValidator.cpp:120`

### Algorithm

**Redundant Sensor Pairs:**
```
Boiler Output ←→ Boiler Return (should be within tolerance)
Water Tank ←→ Water Output (should be close)
```

**Validation Formula:**
```
difference = |sensor1 - sensor2|

IF difference > tolerance:
    FAIL (sensor disagreement)
ELSE:
    PASS (sensors agree)
```

**Implementation:**
```cpp
bool validateCrossSensors(Temperature_t sensor1, Temperature_t sensor2,
                         bool valid1, bool valid2,
                         Temperature_t tolerance) {
    // Both must be valid
    if (!valid1 || !valid2) {
        return false;
    }

    // Calculate absolute difference
    Temperature_t diff = tempAbs(tempSub(sensor1, sensor2));

    // Check tolerance
    if (diff > tolerance) {
        LOG_ERROR(TAG, "Sensor cross-validation failed: diff=%d.%d°C > tolerance=%d.%d°C",
                 diff / 10, diff % 10,
                 tolerance / 10, tolerance % 10);
        return false;
    }

    return true;
}
```

### Parameters
- **Tolerance**: Typically 50 (5.0°C)
- **Applied To**: Boiler output vs return, water tank vs output
- **Sample Rate**: Every safety check (~2.5 seconds)

### Example
```
Boiler Output: 650 (65.0°C)
Boiler Return: 620 (62.0°C)
Difference: 30 (3.0°C)
Tolerance: 50 (5.0°C)
Result: PASS ✓

Boiler Output: 650 (65.0°C)
Boiler Return: 580 (58.0°C)
Difference: 70 (7.0°C)
Tolerance: 50 (5.0°C)
Result: FAIL ✗ (possible sensor fault)
```

---

## 6. Temperature History Buffer (Circular)

### Purpose
Maintain sliding window of temperature readings for rate-of-change analysis.

### Location
`src/modules/control/BurnerSafetyValidator.cpp:368`

### Algorithm

**Circular Buffer Structure:**
```
temperatureHistory: std::vector<Temperature_t> (max 48 samples)
temperatureTimestamps: std::vector<uint32_t> (max 48 timestamps)

Newest ← [T₀, T₁, T₂, ..., T₄₇] → Oldest
         [t₀, t₁, t₂, ..., t₄₇]
```

**Insert Operation:**
```cpp
void updateTemperatureHistory(Temperature_t temp) {
    // Insert at front (newest)
    temperatureHistory.insert(temperatureHistory.begin(), temp);
    temperatureTimestamps.insert(temperatureTimestamps.begin(), millis());

    // Trim if exceeded max size
    const size_t MAX_HISTORY_SIZE = 48;
    if (temperatureHistory.size() > MAX_HISTORY_SIZE) {
        temperatureHistory.pop_back();  // Remove oldest
        temperatureTimestamps.pop_back();
    }
}
```

### Time Window
- **Max Duration**: 120 seconds (48 samples × 2.5s interval)
- **Used For**: 60-second rate-of-change calculation
- **Overhead**: Only stores up to 1-minute worth for actual calculation

### Memory Usage
- **Temperature History**: 48 × 2 bytes = 96 bytes
- **Timestamp History**: 48 × 4 bytes = 192 bytes
- **Total**: 288 bytes per validator instance

---

## 7. PID Auto-Tuner (Relay Feedback Method)

### Purpose
Automatically determine optimal PID gains using system response analysis.

### Location
`src/modules/control/PIDAutoTuner.cpp`

### Algorithm

**Method**: Relay feedback test, gains from a selectable tuning rule

**Steps:**
1. **Oscillation Induction**: Relay control around the setpoint with hysteresis; BoilerTempControlTask drives the burner OFF or FULL
2. **Record Peaks/Troughs**: Extremes over whole relay phases (`RelayExtrema::Tracker`, see below)
3. **Ultimate Gain and Period**: `Ku = 4 × d / (π × a)` (d = relay amplitude, a = oscillation amplitude), `Tu` = average period
4. **Calculate PID Gains** with the configured method (`pid/autotune/method`):

| Method | Kp | Ki | Kd |
|--------|----|----|----|
| 0 ZN_PI (default) | 0.45 × Ku | Kp / (0.83 × Tu) | 0 |
| 1 ZN_PID | 0.6 × Ku | Kp / (0.5 × Tu) | Kp × 0.125 × Tu |
| 2 Tyreus-Luyben | 0.3125 × Ku | Kp / (2.2 × Tu) | Kp × 0.37 × Tu |
| 3 Cohen-Coon (approximation) | 0.35 × Ku | Kp / (1.2 × Tu) | Kp × 0.25 × Tu |
| 4 Lambda | 0.2 × Ku | Kp / Tu | 0 |

Results are limited to Kp 0.1-100, Ki 0-10, Kd 0-10. The method is loaded from `SystemSettings::autotuneMethod` at init (default 0 = ZN_PI) and the MQTT command `method:<name>` persists it.

**Implementation Phases:**
```
Phase 1: IDLE → START
  - Initialize state
  - Start relay feedback

Phase 2: RELAY_TEST
  - Toggle output at setpoint crossing
  - Record peaks and periods

Phase 3: CALCULATION
  - Analyze oscillation
  - Compute Ku and Tu
  - Calculate PID gains

Phase 4: COMPLETE
  - Return calculated gains
```

### Peak/Trough Detection (Lagging Plant)
The boiler keeps heating after the relay switches OFF (burner minimum on-time, stored heat) and keeps cooling after it switches ON (pre-purge, ignition). Values at the switch points understate the swing (2026-09-14 water run: 61.3/58.8°C recorded, 65.6/56.9°C real, amplitude about 3.4x too small).

```
Every sample:      extrema_.sample(temp, time)
On a relay switch: extrema_.onSwitch(newRelayOn, temp, time)
  relay now ON  → OFF phase ended → peak   = max over that OFF phase
  relay now OFF → ON phase ended  → trough = min over that ON phase
```

The warm-up phase is ignored: a phase not started by a switch, and a switch at the very first sample (a cold boiler switches ON at once), report nothing. Samples are counted by timestamp, so `sample()` followed by `onSwitch()` for the same sample (as `PIDAutoTuner::relayControl()` does) behaves the same as `onSwitch()` alone.

### Parameters
- **Relay Amplitude**: `pid/autotune/amplitude`, default 50 % (range 10-100). The relay test drives the burner OFF <-> FULL, a half-swing of 50 %; another value is used as-is in `Ku` and scales the tuned gains by amplitude / 50 (a warning with the factor is logged). Intended for burners whose output really swings by that amount.
- **Hysteresis**: `pid/autotune/hysteresis`, default 1.0°C (range 0.5-10). A wider band gives larger oscillations and more noise tolerance; the formula does not correct for it.
- **Setpoint**: target of the active burner request (55°C if none)
- **Minimum Cycles**: `MIN_CYCLES` = 3 complete oscillations
- **Timeout**: `MAX_TUNING_TIME_SECONDS` = 5400 seconds (90 minutes)
- **Start Window**: boiler output valid, not stale, 15-75°C (`MIN_BOILER_TEMP` / `MAX_BOILER_TEMP`), otherwise rejected
- **Abort**: boiler output invalid or stale, above 80°C (`MAX_TEMP_EXCURSION`), or no active heating/water request; the heat demand is withdrawn
- **Power-On**: requires BurnerControlTask's `BurnerDemandGate` permission and `BurnerSafetyValidator`

---

## 8. Pump Control (Mode-Based)

### Purpose
Run the heating and water pumps from the mode bits, independent of the burner.

### Location
`src/modules/control/PumpControlModule.cpp` (`PumpControlTask()`, started as HeatingPumpTask and WaterPumpTask)

### Algorithm

Each pump task recomputes its desired state every 500 ms (`PUMP_CHECK_INTERVAL_MS`); later rows override earlier ones:
```
Condition                                              | Pump
-------------------------------------------------------|--------------------------------
BOILER_ENABLED && mode bit (HEATING_ON / WATER_ON)     | ON
BOILER_ENABLED && mode bit just cleared                | ON for pumpCooldownMs (overrun)
BOILER_ENABLED cleared                                  | OFF (no overrun)
Heating pump only: ReturnPreheater PREHEATING          | ReturnPreheater::shouldPumpBeOn()
EMERGENCY_STOP set                                      | ON (heat dissipation)
```

A change sets the relay request bit (`RelayRequest::HEATING_PUMP_ON/OFF`, `WATER_PUMP_ON/OFF`); RelayControlTask switches Relay 5 / Relay 6.

### Protection
- **Pump overrun**: after the mode ends the pump keeps running for `SystemSettings::pumpCooldownMs` (default 300000 ms = 5 min) to dissipate residual heat; both pumps
- **Motor protection**: RelayControlTask enforces `SafetyConfig::pumpProtectionMs` (default 15 s) between real pump state changes
- **No burner interlock**: there is no pump pre-start and the burner does not check the pumps (the pump check was removed from `BurnerSafetyValidator`); the burner requires an active mode request instead

---

## 9. Sensor Fallback Strategy

### Purpose
Maintain operation when sensors fail using redundant readings.

### Location
`src/modules/control/TemperatureSensorFallback.cpp`

### Algorithm

**Fallback Priority:**
```
Primary Sensor Failed → Use Backup Sensor → Use Last Known Good

Example (Boiler Output):
1. Primary: Boiler Output sensor
2. Backup: Boiler Return sensor + offset
3. Fallback: Last known good value (max 60s old)
4. Critical Fail: Shutdown if > 60s stale
```

**Implementation:**
```cpp
Temperature_t getBoilerTemp() {
    if (readings.isBoilerTempOutputValid) {
        return readings.boilerTempOutput;  // Primary
    }

    if (readings.isBoilerTempReturnValid) {
        // Estimate output from return + typical differential
        return readings.boilerTempReturn + TYPICAL_DIFFERENTIAL;
    }

    if (lastKnownGood.age_ms < 60000) {
        return lastKnownGood.value;  // Stale but recent
    }

    triggerSensorFailure();  // Critical - shutdown
    return 0;
}
```

### Sensor Pairs
- **Boiler**: Output ↔ Return (±10°C typical differential)
- **Water**: Tank ↔ Output (±5°C typical)
- **Heating**: Return ↔ Boiler Return (±15°C typical)

---

## 10. Mutex Retry with Exponential Backoff

### Purpose
Resilient mutex acquisition with retry logic to prevent deadlock and handle contention.

### Location
`src/utils/MutexRetryHelper.cpp`

### Algorithm

**Retry Strategy:**
```
Attempt 1: Wait 100ms
Attempt 2: Wait 200ms
Attempt 3: Wait 400ms
Max: 3 attempts total

Total max wait: 700ms
```

**Implementation:**
```cpp
MutexGuard acquireGuard(SemaphoreHandle_t mutex,
                       const char* context,
                       TickType_t timeout) {
    const int MAX_RETRIES = 3;
    TickType_t waitTime = pdMS_TO_TICKS(100);

    for (int i = 0; i < MAX_RETRIES; i++) {
        MutexGuard guard(mutex, waitTime);

        if (guard.hasLock()) {
            return guard;  // Success
        }

        LOG_WARN(TAG, "Mutex retry %d/%d for %s",
                 i+1, MAX_RETRIES, context);

        waitTime *= 2;  // Exponential backoff
    }

    LOG_ERROR(TAG, "Mutex acquisition failed for %s after %d retries",
             context, MAX_RETRIES);
    return MutexGuard();  // Failed (no lock)
}
```

### Usage
```cpp
auto guard = MutexRetryHelper::acquireGuard(
    sensorMutex,
    "ReadTemperature"
);

if (guard) {
    // Critical section
    temperature = sensorData.temp;
}
// Auto-release when guard goes out of scope
```

### Benefits
- **Resilience**: Handles transient contention
- **Backoff**: Reduces lock thrashing
- **Logging**: Tracks retry attempts for debugging
- **RAII**: Automatic unlock on scope exit

---

## 11. Temperature Arithmetic (Fixed-Point)

### Purpose
Provide safe temperature operations using integer arithmetic (no float).

### Location
`src/shared/Temperature.h`

### Operations

**Addition:**
```cpp
Temperature_t tempAdd(Temperature_t a, Temperature_t b) {
    return a + b;  // Simple addition
}
```

**Subtraction:**
```cpp
Temperature_t tempSub(Temperature_t a, Temperature_t b) {
    return a - b;  // Handles negative results
}
```

**Absolute Value:**
```cpp
Temperature_t tempAbs(Temperature_t t) {
    return (t < 0) ? -t : t;
}
```

**Conversion from Float:**
```cpp
Temperature_t tempFromFloat(float celsius) {
    return static_cast<Temperature_t>(celsius * 10.0f + 0.5f);  // Round
}
```

**Display Formatting:**
```cpp
void formatTemp(char* buf, size_t size, Temperature_t temp) {
    snprintf(buf, size, "%d.%d°C", temp / 10, abs(temp % 10));
}
```

### Range
- **Min**: -32768 (−3276.8°C) - theoretical limit
- **Practical Min**: -400 (−40.0°C) - ANDRTF3 sensor limit
- **Practical Max**: 8500 (850.0°C) - MB8ART sensor limit
- **Resolution**: 0.1°C (one tenth)

### Overflow Protection
```cpp
// Safe multiplication for rates
int32_t rate = (static_cast<int32_t>(tempChange) * 60000) / timeMs;

// Clamp to int16_t range
if (rate > 32767) rate = 32767;
if (rate < -32768) rate = -32768;

return static_cast<Temperature_t>(rate);
```

---

## 12. Relay Verification with DELAY-Aware Mismatch Counting

### Purpose
Verify relay states match desired commands while accounting for hardware DELAY timers and distinguishing timing issues from real failures.

### Location
`src/modules/tasks/RYN4ProcessingTask.cpp:73` (handleReadTick)

### Algorithm

**Problem**: RYN4 hardware DELAY commands (0x06XX) create expected mismatches:
- DELAY command turns relay ON immediately, auto-OFF after XX seconds
- Physical state = ON during countdown, desired state = OFF
- Reading status returns physical state (0x0001), never command value (0x06XX)
- Creates race condition between SET and READ ticks (1000ms apart)

**Solution**: Multi-tier verification with DELAY tracking

**Step 1: DELAY Exclusion**
```cpp
// Identify which mismatches are from active DELAY timers
uint8_t mismatchMask = actual ^ desired;       // XOR to find differences
uint8_t delayMask = g_relayState.delayMask;    // Relays with active DELAY
uint8_t realMismatch = mismatchMask & ~delayMask;  // Exclude DELAY relays

if (realMismatch == 0) {
    // All mismatches are from DELAY - expected behavior!
    LOG_DEBUG("Verification deferred (DELAY active)");
    return;  // Don't increment counter or retry
}
```

**Step 2: Mismatch Counting**
```cpp
uint8_t mismatches = g_relayState.consecutiveMismatches.fetch_add(1) + 1;

if (mismatches == 1) {
    // First mismatch - likely timing race condition
    LOG_DEBUG("Verification pending (attempt 1/2)");
    // Silent retry on next READ tick
} else {
    // Persistent mismatch - real hardware/communication problem!
    LOG_ERROR("Verification FAILED after %d attempts!", mismatches);
    xEventGroupSetBits(relayStatusEventGroup, COMM_ERROR);
}

// Queue retry for next SET tick
g_relayState.pendingWrite.store(true);
```

**Step 3: Success Reset**
```cpp
if (actual == desired) {
    uint8_t previousMismatches = g_relayState.consecutiveMismatches.exchange(0);

    if (previousMismatches > 0) {
        LOG_INFO("Verification SUCCESS after %d attempts", previousMismatches + 1);
    } else {
        LOG_DEBUG("Relay states verified");
    }
}
```

### DELAY Tracking (RelayState Structure)

**Fields**:
- `delayMask` (atomic uint8_t): Bitmask of relays with active DELAY
- `delayExpiry[8]` (uint32_t): Timestamp when each DELAY expires (milliseconds)
- `delayMutex` (SemaphoreHandle_t): Protects delayExpiry array

**Methods**:
```cpp
// Set DELAY for relay (called when sending 0x06XX command)
void setDelayCommand(uint8_t relay, uint8_t delaySeconds) {
    delayExpiry[relay] = millis() + (delaySeconds * 1000);
    delayMask.fetch_or(1 << relay);
}

// Check if DELAY still active
bool isDelayActive(uint8_t relay) const {
    if (!(delayMask & (1 << relay))) return false;
    return (delayExpiry[relay] > millis());
}

// Clear expired DELAY
void clearDelay(uint8_t relay) {
    delayExpiry[relay] = 0;
    delayMask.fetch_and(~(1 << relay));
}
```

### Timing Diagram
```
Time:     0ms        500ms       1000ms      1500ms
Tick:     SET                    READ

Actions:
  SET:    Write 0x06  ←──────────┐
          relay ON               │ Physical ON
          DELAY start            │ (countdown)
                                 ├─→ Actual=ON
  READ:                          │   Desired=OFF
                    Skip verify  │   delayMask=1
                    (DELAY)      │   realMismatch=0
                                 │   ✓ Expected!

  Time:   [after DELAY expires]
  READ:   Actual=OFF, Desired=OFF, Match! ✓
```

### Parameters
- **SET → READ interval**: 1000ms (2 ModbusCoordinator ticks × 500ms)
- **Max mismatches before ERROR**: 2 consecutive
- **DELAY tracking**: Per-relay, millisecond precision
- **Mutex timeout**: 100ms for delayExpiry access

### Benefits
1. **Eliminates false positives** from hardware DELAY timers
2. **Distinguishes timing issues** (1 mismatch) from real failures (2+)
3. **Silent first retry** - doesn't spam logs during normal timing variations
4. **Accurate error detection** - persistent mismatches logged as ERROR
5. **Auto-recovery** - resets counter on successful verification

### Hardware Reference
See `docs/EQUIPMENT_SPECS.md` - "RYN4 8-Channel Relay Module" section for complete DELAY command behavior and mbpoll test results.

---

## 13. Modbus Bus Arbitration (Tick-Based Scheduling)

### Purpose
Prevents Modbus RTU bus collisions by using time-division multiplexing. Ensures only one device communicates at a time while maintaining responsive sensor reads and relay control.

### Location
`src/core/ModbusCoordinator.h:116-120`, `src/core/ModbusCoordinator.cpp`

### Algorithm

**Tick-Based Scheduling**:
- **Tick interval**: 500ms (configurable)
- **Cycle length**: 10 ticks = 5 seconds
- **Coordinator**: FreeRTOS timer notifies tasks on their designated ticks
- **Devices**: 3 Modbus slaves (MB8ART 0x01, RYN4 0x02, ANDRTF3 0x03)

**Timing Diagram**:
```
Tick  Time   Device   Operation        Purpose
----  ----   ------   ---------        -------
 0    0.0s   ANDRTF3  Read room temp   Room temperature (5s interval)
 1    0.5s   RYN4     SET (write)      Apply relay changes
 2    1.0s   MB8ART   Read temps       Boiler temperatures
 3    1.5s   RYN4     READ (verify)    Verify relay states (1s after SET)
 4    2.0s   (idle)   -                Bus quiet
 5    2.5s   MB8ART   Read temps       Boiler temperatures (2.5s interval)
 6    3.0s   RYN4     SET (write)      Apply relay changes (2.5s interval)
 7    3.5s   (idle)   -                Bus quiet
 8    4.0s   RYN4     READ (verify)    Verify relay states
 9    4.5s   (idle)   -                Bus quiet
[repeat from tick 0]
```

### Implementation

**Tick Arrays** (defined in `ModbusCoordinator.h`):
```cpp
static constexpr uint32_t TICK_INTERVAL_MS = 500;
static constexpr uint32_t TICKS_PER_CYCLE = 10;  // 10 × 500ms = 5 seconds

static constexpr uint32_t ANDRTF3_TICKS[] = {0};       // Ticks: 0
static constexpr uint32_t RYN4_SET_TICKS[] = {1, 6};   // Ticks: 1, 6
static constexpr uint32_t MB8ART_TICKS[] = {2, 5};     // Ticks: 2, 5
static constexpr uint32_t RYN4_READ_TICKS[] = {3, 8};  // Ticks: 3, 8
```

**Task Notification Flow**:
```cpp
void ModbusCoordinator::processTick() {
    uint32_t tick = currentTick % TICKS_PER_CYCLE;

    // Check each device's tick schedule
    if (tick matches ANDRTF3_TICKS) {
        xTaskNotify(andrtf3TaskHandle, NOTIFY_READ_SENSOR);
    }
    if (tick matches RYN4_SET_TICKS) {
        xTaskNotify(ryn4TaskHandle, NOTIFY_SET_RELAYS);
    }
    // ... etc
}
```

### Benefits

1. **No Bus Collisions**: Only one Modbus transaction per 500ms window
2. **Predictable Timing**: Deterministic operation sequence
3. **Verification Window**: 1-second gap between SET and READ (2 ticks)
4. **Fast Response**: Relays updated every 2.5s maximum latency
5. **Temperature Priority**: MB8ART reads twice per cycle (2.5s interval) for responsive control
6. **Scalable**: Easy to add new devices by assigning unused ticks

### Timing Analysis

**Relay Control Loop**:
```
T=0.5s:  RYN4 SET (write desired states)
T=1.5s:  RYN4 READ (verify actual states)
         └─ 1.0 second verification window
         └─ Allows hardware DELAY commands to take effect

Worst-case relay update: 2.5 seconds (if request arrives just after SET tick)
Best-case relay update: 0.5 seconds (if request arrives before SET tick)
```

**Temperature Sensor Updates**:
```
MB8ART:   Tick 2, 5 → 2.5 second interval (responsive burner control)
ANDRTF3:  Tick 0    → 5.0 second interval (room temp changes slowly)
```

**Bus Utilization**:
```
Active ticks: 6 out of 10 (60% utilization)
Idle ticks:   4 out of 10 (40% margin for future expansion)
```

### Device-Specific Notes

**RYN4 Relay Module**:
- SET and READ must be separate to allow hardware DELAY commands
- DELAY commands (0x06XX format): Auto-off timer in hardware
- READ returns physical state (0x0001/0x0000), never command value
- Verification skips relays with active DELAY countdown

**MB8ART Temperature Sensors**:
- 8 channels, 5 active (boiler output, DHW tank, outdoor, heating supply, water supply)
- 2.5s interval balances responsiveness vs. bus load
- Faster than required for PID (2s cycle time)

**ANDRTF3 Room Sensor**:
- Single channel (room temperature)
- 5s interval sufficient (room temp changes slowly)
- Lowest priority (tick 0, start of cycle)

### Modifying the Schedule

**To add a new device**:
1. Assign unused tick(s): 4, 7, or 9
2. Add tick array: `static constexpr uint32_t NEW_DEVICE_TICKS[] = {4};`
3. Register in `processTick()`: Check tick against array, notify task
4. Verify total cycle time meets requirements

**Constraints**:
- Modbus transaction time: ~100-200ms typical
- Tick interval: Must exceed transaction time (500ms > 200ms ✓)
- Verification gap: SET and READ should be ≥1 tick apart

---

## 14. Water Heating Two-Threshold Control

### Purpose
Provides simple, reliable hysteresis control for water tank heating without complex symmetric calculations. Prevents rapid on/off cycling while maintaining target temperature range.

### Location
`include/modules/control/WaterChargePolicy.h` (`limitsValid`, `nextChargeNeeded`), called from `checkIfWaterHeatingNeededEvent()` in `src/modules/tasks/WheaterControlTask.cpp`

### Algorithm

**Two-Threshold Hysteresis**:
```
State: OFF → Turn ON when:  temp < tempLimitLow
State: ON  → Turn OFF when: temp > tempLimitHigh

Stay in current state when: tempLimitLow ≤ temp ≤ tempLimitHigh
```

**Implementation**:
```cpp
// WaterChargePolicy (tenths of °C)
inline bool limitsValid(int16_t tempLimitLow, int16_t tempLimitHigh) {
    return tempLimitLow > 0 && tempLimitHigh > 0 && tempLimitLow < tempLimitHigh;
}

inline bool nextChargeNeeded(bool charging, int16_t tankTemp,
                             int16_t tempLimitLow, int16_t tempLimitHigh) {
    return charging ? !(tankTemp > tempLimitHigh) : (tankTemp < tempLimitLow);
}

// WheaterControlTask::checkIfWaterHeatingNeededEvent()
if (readings.isWaterHeaterTempTankValid && limitsValid) {
    heatingNeeded = WaterChargePolicy::nextChargeNeeded(
        waterState.lastHeatingNeeded, currentTemp, lowLimit, highLimit);
    waterState.lastHeatingNeeded = heatingNeeded;
} else {
    heatingNeeded = false;
}
```

### Parameters

**MQTT Configuration**:
- `boiler/params/wheater/tempLimitLow` - Start heating threshold (default: 45.0°C / 450 tenths, range 300-600)
- `boiler/params/wheater/tempLimitHigh` - Stop heating threshold (default: 65.0°C / 650 tenths, range 500-850)

**Typical Values**:
- Low limit: 45-55°C (DHW comfort minimum)
- High limit: 55-65°C (DHW comfort target)
- Hysteresis band: 5-15°C (prevents cycling)

### Inverted Limits

Each limit is range-checked on its own (low 30-60°C, high 50-85°C), so the pair can be inverted, for example when both are raised and low is sent first. `limitsValid()` treats `low >= high` as invalid: no charge, one WARN (`Water limits inconsistent: low X°C >= high Y°C - water heating paused`) and an INFO (`Water limits consistent again`) once the pair is valid. This also covers inverted values loaded from NVS. To raise both limits set `tempLimitHigh` first; to lower both set `tempLimitLow` first.

### Charge Latch Reset

The latch (`waterState.lastHeatingNeeded`) is cleared when water heating is switched off:
- water heating or the boiler disabled (on every run while disabled)
- durable water OFF override (`waterOverrideOff`) set
- the transient `WATER_OFF_OVERRIDE` ends a charge
- `notifyWheaterTaskSwitchedOff()` (StateManager disable/override, `CentralizedFailsafe::emergencyStop()`): the next run ends a running charge even if water heating was re-enabled within the same cycle

After re-enabling, a new charge starts only below `tempLimitLow`. Heating preemption and sensor loss do not clear the latch; the interrupted charge resumes.

### Benefits

1. **Simple & Reliable**: No complex symmetric hysteresis calculations
2. **Prevents Cycling**: Wide band prevents rapid on/off
3. **Configurable**: Thresholds adjustable via MQTT
4. **Energy Efficient**: Burner runs continuously in band (modulates power level)
5. **Legionella Prevention**: High limit can be set to 60°C+ for safety

### Interaction with PID

Water heating uses **dual control strategy**:
1. **Two-threshold**: Decides IF heating is needed (ON/OFF decision)
2. **PID**: Decides WHAT power level when ON (HALF/FULL modulation)

**Combined operation**:
```
Temp < 50°C:  Two-threshold → Turn ON
              PID → Modulate between HALF/FULL based on boiler output temp
                    (target = tank + charge delta)
Temp > 60°C:  Two-threshold → Turn OFF
              PID → Disabled (not heating)
```

**Result**: Burner stays ON continuously in 50-60°C band, switching between half and full power for efficient heat transfer.

### Example Scenario

```
Initial state: Tank at 48°C, heating OFF

T=0s:    Tank 48°C < 50°C (low limit)  → Turn heating ON
T=30s:   Tank 52°C, burner on HALF     → Stay ON (in band)
T=60s:   Tank 56°C, burner on FULL     → Stay ON (in band)
T=90s:   Tank 59°C, burner on HALF     → Stay ON (in band)
T=120s:  Tank 61°C > 60°C (high limit) → Turn heating OFF
T=150s:  Tank 59°C                     → Stay OFF (in band)
...later...
T=600s:  Tank 49°C < 50°C              → Turn heating ON again
```

**Anti-Cycling**: 10-15 minute heating cycles typical, preventing burner wear.

---

## 15. Burner Demand Gate

### Purpose
Two tasks write the burner heat demand (`BurnerStateMachine::setHeatDemand()`): BurnerControlTask on request start/change and safety blocks, BoilerTempControlTask from the PID power level. The gate makes them agree, so a request that starts while the boiler is already above target does not ignite and then run the minimum on-time.

### Location
`include/modules/control/BurnerDemandGate.h`, used by `src/modules/tasks/BurnerControlTask.cpp` and `src/modules/tasks/BoilerTempControlTask.cpp`

### Algorithm

**Permission** (published by BurnerControlTask, read with `getBurnerDemandPermission()`):
- `permitted`: request present and BurnerControlTask's blocks passed (sensors fresh, safety validation, no return preheating); withdrawn when the boiler is disabled or sensor fallback cannot continue
- `highPowerAllowed`: false while sensor fallback reduces the power factor
- `maxTargetTemp`: sensor fallback target cap, 0 = none

**BurnerControlTask arming** (`controlTaskMayArm`, new or changed request):
```cpp
if (!boilerTempValid) return true;           // PID does not run: arm as before
if (decisionFresh) {                         // age <= DECISION_MAX_AGE_MS (6000)
    if (abs(decisionTarget - target) <= DECISION_TARGET_TOLERANCE)  // 10 = 1.0°C
        return decisionOn;                   // follow BoilerTempControlTask
}
return boilerTemp < target;
```

**BoilerTempControlTask sync** (`decide`, every cycle):

| Permitted and PID wants heat | Demand armed | PID output changed | Action |
|------------------------------|--------------|--------------------|--------|
| yes | no | any | ARM (after safety validation) |
| yes | yes | yes | SET_POWER (after safety validation) |
| yes | yes | no | NONE |
| no | yes | any | DISARM |
| no | no | any | NONE |

**Blocked request retry**: a request that is present but not permitted is re-evaluated by BurnerControlTask every 10 s, because request change events fire only when the request bits change.

**Autotune**: relay power-on also requires `permitted`.

---

## 16. Burner Transition Rules

### Purpose
Next-state decisions of the burner state machine as header-only functions that native tests replay tick by tick. For the state diagram see [STATE_MACHINES.md](STATE_MACHINES.md).

### Location
`include/modules/control/BurnerTransitions.h` (`BurnerTransitions::step()`), `include/modules/control/BurnerTransitionPolicy.h`

### Rules

| Rule | Condition | Result |
|------|-----------|--------|
| Active mode demand | `HEATING_ON` and heating request, or `WATER_ON` and water request | Required to leave IDLE; PRE_PURGE aborts to IDLE without it or without heat demand |
| Stale demand | Heat demand without active mode demand in IDLE | Stay IDLE |
| No mode demand while running | Missing for `MODE_DEMAND_LOSS_GRACE_MS` (10 s) | POST_PURGE, anti-flapping bypassed |
| Explicit disable | Running mode's `*_ENABLED` bit or `BOILER_ENABLED` cleared | POST_PURGE at once, minimum on-time bypassed (`stopForExplicitDisable`) |
| Handover wait | MODE_SWITCHING without a heating request | Wait only while `heatingLikelyWanted()`, at most `MODE_SWITCH_MAX_WAIT_MS` (15 s) |
| Mode revert | Demand points back to the running mode | RUNNING_LOW only if its ON bit is set, otherwise wait (15 s max), then POST_PURGE |
| Hard timeout | MODE_SWITCHING for `MODE_SWITCH_HARD_TIMEOUT_MS` (30 s) | POST_PURGE |
| Restart from post-purge | Heat demand and active mode demand, mode not just disabled, safety OK, minimum off-time over | PRE_PURGE |
| Ignition | No flame after `IGNITION_TIME_MS` (5 s) | Retry via PRE_PURGE; `MAX_IGNITION_RETRIES` (3) → LOCKOUT. The StateMachine timeout `IGNITION_TIME_MS + IGNITION_BACKSTOP_MARGIN_MS` is a backstop |
| Power level fault | `setPowerLevel()` fails entering RUNNING_LOW/HIGH | DEGRADED failsafe and POST_PURGE; the third fault within 10 min (`recordPowerFault`) escalates to an emergency stop |

---

## Summary Table

| Algorithm | Purpose | Key Parameter | Location |
|-----------|---------|---------------|----------|
| **Rate-of-Change** | Thermal runaway detection | 60s window, 10°C/min limit | BurnerSafetyValidator:295 |
| **PID Control** | Temperature regulation | Kp/Ki/Kd gains | PIDControlModuleFixedPoint |
| **Pump Protection** | Motor lifespan | pumpProtectionMs (15s default), real changes only | RelayControlTask |
| **MQTT Queuing** | Message prioritization | 20/40 queue split | MQTTTask:150 |
| **Sensor Fallback** | Redundancy | 3-tier fallback | TemperatureSensorFallback |
| **Mutex Retry** | Deadlock prevention | 3 retries, exponential | MutexRetryHelper |
| **Cross-Validation** | Sensor agreement | 5°C tolerance | BurnerSafetyValidator:120 |
| **Temperature Math** | Fixed-point ops | int16_t tenths | Temperature.h |
| **Relay Verification** | DELAY-aware checking | 2 mismatches = error | RYN4ProcessingTask:73 |
| **Modbus Arbitration** | Bus collision prevention | 500ms ticks, 10-tick cycle | ModbusCoordinator.h:116 |
| **Water Two-Threshold** | Tank heating control | tempLimitLow/High hysteresis, low < high | WaterChargePolicy.h |
| **Burner Demand Gate** | Heat demand arming | 6s decision age, 1°C tolerance, 10s retry | BurnerDemandGate.h |
| **Burner Transitions** | Burner state decisions | 10s no-mode grace, 15s/30s mode switch bounds | BurnerTransitions.h |

---

**Document Version**: 0.1.0
**Last Updated**: 2025-12-16
**Author**: System Documentation
