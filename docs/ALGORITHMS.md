# Control Algorithms Documentation

This document describes the key algorithms used in the ESP32 Boiler Controller for safety monitoring, temperature control, and system coordination.

**Not implemented:** the firmware has no temperature rate-of-change (thermal runaway) check, no sensor cross-validation and no temperature history buffer; sections 1, 5 and 6 were removed for that reason.

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
int32_t pidPower = PIDGainFixedPoint::powerPercentFromAdjustment(adjustment);  // 50 + adjustment / 10
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
- **Output Limits**: the boiler PID is limited to ±500 tenths (`PIDGainFixedPoint::POWER_ADJUSTMENT_LIMIT`, `setOutputLimits()` in `BoilerTempController::initialize()`), exactly where the power mapping saturates at 0/100 %. With the module default ±1000 (`OUTPUT_MIN`/`OUTPUT_MAX`) the integral kept growing while power was already 100 % and held FULL above target
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
`src/modules/tasks/MQTTTask.cpp` (`publish()`, `processPublishQueue()`, `getQueueUtilization()`, `isUnderPressure()`, `shouldThrottle()`), `src/modules/tasks/MQTTTask.h`

### Algorithm

**Dual Queue System:**
```
High priority queue:   5 messages (HIGH_PRIORITY_QUEUE_SIZE)   - PRIORITY_CRITICAL, PRIORITY_HIGH
Normal priority queue: 5 messages (NORMAL_PRIORITY_QUEUE_SIZE) - PRIORITY_MEDIUM, PRIORITY_LOW

Total capacity: 10 messages
Overflow strategy: QueueManager DROP_OLDEST (both queues)
```

A message that cannot be queued is counted as dropped; the count is logged at most once per `QUEUE_DROP_LOG_INTERVAL_MS`: `MQTT queue overflow - dropped H:<n> N:<n> messages`.

**Utilization** (`getQueueUtilization()`):
```
util = 0.6 × high queue fill % + 0.4 × normal queue fill %
```

**Publishing Order** (`processPublishQueue()`):
```
1. High priority queue first, up to MAX_ITEMS_PER_ITERATION per run (5 ms yield every 4 messages)
2. Normal priority queue with the remaining capacity of that run
3. On CONNECTION_FAILED the message is dropped and processing stops
```

### Backpressure Handling
- **Pressure on**: util >= 80 % sets `MQTT_QUEUE_PRESSURE` (`Queue pressure HIGH (util: N%) - throttling non-critical messages`)
- **Pressure off**: util < 30 % (50 % - 20 % hysteresis) clears the bit (`Queue pressure released (util: N%)`)
- **Throttling** (`shouldThrottle()`, message not queued): PRIORITY_LOW at util >= 50 %, PRIORITY_MEDIUM at util >= 80 %; HIGH and CRITICAL are never throttled (`Backpressure active: throttled N messages`, every 10 s)
- **CRITICAL bypass**: while under pressure, PRIORITY_CRITICAL is published directly; if that fails it falls back to the high priority queue

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
3. **Ultimate Gain and Period**: `Ku = 4 × d / (π × a)` (d = relay amplitude, a = (mean peak - mean trough) / 2), `Tu` = mean of the peak-to-peak and trough-to-trough periods (lowest and highest 20 % dropped when there are more than 5 periods)
4. **Calculate PID Gains** with the configured method (`pid/autotune/method`):

| Method | Kp | Ki | Kd |
|--------|----|----|----|
| 0 ZN_PI (default) | 0.45 × Ku | Kp / (0.83 × Tu) | 0 |
| 1 ZN_PID | 0.6 × Ku | Kp / (0.5 × Tu) | Kp × 0.125 × Tu |
| 2 Tyreus-Luyben | 0.3125 × Ku | Kp / (2.2 × Tu) | Kp × 0.37 × Tu |
| 3 Cohen-Coon (approximation) | 0.35 × Ku | Kp / (1.2 × Tu) | Kp × 0.25 × Tu |
| 4 Lambda | 0.2 × Ku | Kp / Tu | 0 |

Results are limited to Kp 0.1-100, Ki 0-10, Kd 0-10. The method is loaded from `SystemSettings::autotuneMethod` at init (default 0 = ZN_PI) and the MQTT command `method:<name>` persists it.

**Tuner States** (`PIDAutoTuner::TuningState`):
```
IDLE → RELAY_TEST (startTuning)

RELAY_TEST
  - Relay ON when temp < setpoint - hysteresis, OFF when temp > setpoint + hysteresis
  - Record peaks and troughs on relay switches
  - Timeout → FAILED; stopTuning() → IDLE

ANALYZING (once min(peaks, troughs) >= MIN_CYCLES)
  - Compute Tu, a and Ku
  - Apply the tuning method

COMPLETE  - gains valid
FAILED    - fewer than 2 peaks or troughs, period or amplitude <= 0, or timeout
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
EMERGENCY_STOP set                                      | ON (heat dissipation) until boiler output < 60.0°C,
                                                        | ON again from 65.0°C, always ON without a usable reading
EMERGENCY_STOP released while still dissipating         | ON until boiler output < 60.0°C (no usable reading:
                                                        | at most pumpCooldownMs)
```

A change sets the relay request bit (`RelayRequest::HEATING_PUMP_ON/OFF`, `WATER_PUMP_ON/OFF`); RelayControlTask switches Relay 5 / Relay 6. While the relay's desired state differs from the pump task's state (command refused by motor protection, or relay switched directly by the failsafe) the request is re-sent every 2 s.

### Protection
- **Pump overrun**: after the mode ends the pump keeps running for `SystemSettings::pumpCooldownMs` (default 300000 ms = 5 min) to dissipate residual heat; both pumps
- **Motor protection**: RelayControlTask enforces `SafetyConfig::pumpProtectionMs` (default 15 s) between real pump state changes
- **No burner interlock**: there is no pump pre-start and the burner does not check the pumps (the pump check was removed from `BurnerSafetyValidator`); the burner requires an active mode request instead

---

## 9. Sensor Fallback Strategy

### Purpose
Stop burner operation when a sensor required for the active operation is missing. There is no substitute value: no estimate from another sensor and no last known good reading.

### Location
`src/modules/control/TemperatureSensorFallback.cpp`, `include/modules/control/TemperatureSensorFallback.h`

### Required Sensors (`hasRequiredSensors()`)

| Operation mode | Required (valid and fresh) |
|----------------|----------------------------|
| NONE | boiler output |
| SPACE_HEATING | boiler output, boiler return, room |
| WATER_HEATING | boiler output, water tank |
| BOTH | boiler output, boiler return, room, water tank |

A reading is valid when its valid flag is set, it is not `TEMP_INVALID`/`TEMP_UNKNOWN` and it lies within -50.0 to 150.0°C. Freshness is checked per source against `SafetyConfig::sensorStaleMs` (default 60 s): MB8ART values (boiler output/return, tank, outside) use `lastBoilerTempUpdateTimestamp`, the ANDRTF3 room sensor uses `lastUpdateTimestamp`.

### Modes (`updateSensorStatus()`)
```
STARTUP  → NORMAL    1 valid check   (VALID_COUNT_TO_ENTER_NORMAL)
STARTUP  → SHUTDOWN  1 invalid check (INVALID_COUNT_TO_SHUTDOWN), only after STARTUP_PERIOD_MS (5 s)
NORMAL   → SHUTDOWN  1 invalid check
SHUTDOWN → NORMAL    1 valid check
```
`canContinueOperation()` returns true only in NORMAL. `getSafeOperatingParams()` gives 110.0°C / 100 % in NORMAL and 0 in STARTUP and SHUTDOWN.

**Entering SHUTDOWN:** ERROR `SHUTDOWN: Missing sensors: Boiler Return, Room Temperature (required for space heating)` (example), sets `SENSOR_FAILURE` and `SENSOR_DEGRADED`, publishes retained `boiler/status/sensor_fallback` (JSON with `mode` and `missing`) and `boiler/status/sensor_mode`.

**SHUTDOWN → NORMAL:** clears both bits, calls `CentralizedFailsafe::releaseAfterSensorRecovery()` (releases `EMERGENCY_STOP` only if stale sensor data caused it) and publishes `boiler/status/sensor_fallback/recovery`.

---

## 10. Mutex Retry with Escalation

### Purpose
Resilient mutex acquisition with retries and escalation of persistent contention.

### Location
`src/utils/MutexRetryHelper.cpp`, `src/utils/MutexRetryHelper.h`

### Algorithm

**Retry Strategy** (defaults of `RetryConfig()`):
```
Per attempt: xSemaphoreTake(mutex, timeout), timeout = MUTEX_DEFAULT_TIMEOUT_MS (100 ms)
maxRetries = 3        → up to 4 attempts
retryDelayTicks = 10 ms fixed delay between attempts (no backoff)
escalationThreshold = 5 consecutive failed acquisitions

Worst case with defaults: 4 × 100 ms + 3 × 10 ms = 430 ms
```

**Implementation** (`MutexRetryHelper::acquire()`, simplified):
```cpp
for (uint8_t attempt = 0; attempt <= config.maxRetries; attempt++) {
    if (xSemaphoreTake(mutex, timeout) == pdTRUE) {
        // reset this mutex's consecutive failure count
        return acquired;
    }
    if (attempt < config.maxRetries) {
        vTaskDelay(config.retryDelayTicks);
        if (config.logFailures && attempt > 0) {
            LOG_WARN(TAG, "Mutex '%s' retry %d/%d", name, attempt + 1, config.maxRetries);
        }
    }
}
LOG_WARN(TAG, "Mutex '%s' acquisition FAILED after %d attempts", name, attemptsUsed);
// consecutiveFailures++; at escalationThreshold (once until the next success):
//   LOG_ERROR "MUTEX CONTENTION: '%s' - %d consecutive failures, escalating to HealthMonitor"
//   set MUTEX_CONTENTION bit, HealthMonitor CONTROL error MUTEX_TIMEOUT
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
- **Escalation**: Persistent contention reaches HealthMonitor
- **Logging**: Tracks retry attempts for debugging
- **RAII**: Automatic unlock on scope exit

---

## 11. Temperature Arithmetic (Fixed-Point)

### Purpose
Provide safe temperature operations using integer arithmetic (no float).

### Location
`include/shared/Temperature.h` (constants in `include/config/TemperatureConstants.h`)

### Operations

**Addition / Subtraction** (saturating, invalid propagates):
```cpp
Temperature_t tempAdd(Temperature_t a, Temperature_t b) {
    if (a == TEMP_INVALID || b == TEMP_INVALID) return TEMP_INVALID;
    int32_t result = static_cast<int32_t>(a) + static_cast<int32_t>(b);
    if (result > 32767) return 32767;
    if (result < -32768) return -32768;
    return static_cast<Temperature_t>(result);
}
// tempSub: same with a - b
```

**Absolute Value:**
```cpp
Temperature_t tempAbs(Temperature_t t) {
    if (t == TEMP_INVALID) return TEMP_INVALID;
    if (t == -32768) return 32767;  // abs(INT16_MIN) does not fit
    return t < 0 ? -t : t;
}
```

**Conversion from Float:**
```cpp
Temperature_t tempFromFloat(float f) {
    if (std::isnan(f) || std::isinf(f)) return TEMP_INVALID;
    if (f > 3276.7f) return 32767;
    if (f < -3276.8f) return -32768;
    return static_cast<Temperature_t>(f * 10.0f + (f >= 0 ? 0.5f : -0.5f));  // round half away from zero
}
```

**Display Formatting** (returns characters written, no unit):
```cpp
int formatTemp(char* buf, size_t size, Temperature_t t);
// TEMP_INVALID → "N/A"; -5 → "-0.5"; 245 → "24.5"
```

### Range
- **Markers**: `TEMP_INVALID` = -32768, `TEMP_UNKNOWN` = -32767
- **Min**: -32768 (−3276.8°C) - theoretical limit
- **Practical Min**: -400 (−40.0°C) - ANDRTF3 sensor limit
- **Practical Max**: 8500 (850.0°C) - MB8ART sensor limit
- **Resolution**: 0.1°C (one tenth)

### Overflow Protection
`tempAdd()`/`tempSub()` compute in `int32_t` and saturate to the `int16_t` range. Wider intermediate results (PID terms, heating curve) are computed in `int32_t`/`int64_t` and clamped before narrowing, see `PIDGainFixedPoint::clampToAdjustment()` and `HeatingCurve::target()`.

---

## 12. Relay Verification with DELAY-Aware Mismatch Counting

### Purpose
Verify relay states match desired commands while accounting for hardware DELAY timers and distinguishing timing issues from real failures.

### Location
`src/modules/tasks/RYN4ProcessingTask.cpp` (`handleReadTick()`)

### Algorithm

**Problem**: RYN4 hardware DELAY commands (0x06XX) create expected mismatches:
- DELAY command turns relay ON immediately, auto-OFF after XX seconds
- Physical state = ON during countdown, desired state = OFF
- Reading status returns physical state (0x0001), never command value (0x06XX)
- Creates race condition between SET and READ ticks (1000ms apart)

**Solution**: Multi-tier verification with DELAY tracking

**Step 1: DELAY Exclusion (direction-aware)**
```cpp
// A relay only gets a DELAY when commanded ON. A DELAY relay reading OFF while
// commanded ON is a real failure. Only DELAY relays commanded OFF (coasting down
// on a previous DELAY) are masked.
uint8_t mismatchMask = actual ^ sent;
uint8_t delayMask = g_relayState.delayMask.load();
uint8_t maskableDelay = delayMask & ~sent;             // DELAY + commanded OFF
uint8_t realMismatch = mismatchMask & ~maskableDelay;
```
Each relay's result also feeds `RelayVerificationManager::checkRelayHealthAndEscalate()`: a persistent mismatch on BURNER_ENABLE escalates to a CRITICAL failsafe (emergency shutdown), other relays to WARNING.

**Step 2: Mismatch Counting**
```cpp
if (realMismatch != 0) {
    uint8_t mismatches = g_relayState.consecutiveMismatches.fetch_add(1) + 1;

    if (mismatches == 1) {
        // First mismatch - likely timing issue
        LOG_DEBUG(TAG, "Relay verification pending (attempt 1/2): Sent: 0x%02X, Actual: 0x%02X", ...);
    } else {
        // Persistent mismatch - real problem
        LOG_ERROR(TAG, "Relay verification FAILED after %d attempts! Sent: 0x%02X, Actual: 0x%02X", ...);
        // plus "  Relay N: sent=ON, actual=OFF" for each mismatching relay
        xEventGroupSetBits(relayStatusEventGroup, COMM_ERROR);
    }

    // Queue retry for next SET tick
    g_relayState.pendingWrite.store(true);
}
```

**Step 3: Success Reset** (no real mismatch)
```cpp
uint8_t previousMismatches = g_relayState.consecutiveMismatches.exchange(0);

if (previousMismatches > 0) {
    LOG_INFO(TAG, "Relay verification SUCCESS after %d attempts: 0x%02X", ...);
} else if (actual != sent) {
    LOG_DEBUG(TAG, "Relay verification deferred (DELAY coast-down): Sent: 0x%02X, Actual: 0x%02X, Delay mask: 0x%02X", ...);
} else {
    LOG_DEBUG(TAG, "Relay states verified: 0x%02X", actual);
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
- `boiler/params/set/wheater/tempLimitLow` - Start heating threshold (default: 45.0°C / 450 tenths, range 300-600)
- `boiler/params/set/wheater/tempLimitHigh` - Stop heating threshold (default: 65.0°C / 650 tenths, range 500-850)

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
4. **Power from PID**: Within the band the boiler PID chooses OFF, HALF or FULL
5. **Legionella Prevention**: High limit can be set to 60°C+ for safety

### Interaction with PID

Water heating uses **dual control strategy**:
1. **Two-threshold**: Decides IF a charge is needed (water burner request on/off)
2. **PID**: Decides the power level during the charge (OFF/HALF/FULL)

**Combined operation** (default limits 45/65°C):
```
Tank < 45°C:  Two-threshold → charge ON (water burner request)
              Boiler target = tank + wheater/tempChargeDelta (default 10°C),
              clamped to the water heating low/high limits
              PID → OFF/HALF/FULL from boiler output vs that target
Tank > 65°C:  Two-threshold → charge OFF, request withdrawn
```

**Result**: The water request stays active from below 45°C until above 65°C. Within the band the PID sets the power level. It also commands OFF when its output drops below 35 % (boiler output above target) and switches on again above 55 % (`BoilerTempController::calculateModulating()`), so the burner does not necessarily run continuously during a charge.

### Example Scenario

```
Initial state: Tank at 44°C, charge OFF

Tank 44°C < 45°C (low limit)   → charge ON
Tank 52°C                      → stay ON (in band), power level from PID
Tank 64°C                      → stay ON (in band)
Tank 66°C > 65°C (high limit)  → charge OFF
Tank 60°C                      → stay OFF (in band)
...later...
Tank 44°C < 45°C               → charge ON again
```

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

## 17. Heating Curve and Space Heating Mode

### Purpose
Compute the boiler target for space heating from outside and room temperature, and decide when space heating is needed (weather mode or room mode).

### Location
`include/modules/control/HeatingCurve.h` (`HeatingCurve::target()`), `src/modules/control/HeatingControlModuleFixedPoint.cpp`, `src/modules/control/HeatingControlModule.cpp` (`calculateSpaceHeatingTargetTemp()`), `include/modules/control/SpaceHeatingPolicy.h`, `src/modules/tasks/HeatingControlTask.cpp`

### Heating Curve

**Formula:**
```
diff   = outside - inside        (°C, inside = measured room temperature)
target = inside + shift - coeff × diff × (1.4347 + 0.021 × diff + 0.000248 × diff²)
```

**Fixed point** (`HeatingCurve::target()`): temperatures in tenths of °C, `coeff` scaled by 100 (`heating_curve_coeff * 100`, truncated), `shift` in tenths.
```cpp
diff        = outside - inside;                       // tenths
diffSquared = diff * diff / 10;                       // °C² × 10
polynomial  = 14347 + 210 * diff / 10 + 248 * diffSquared / 1000;   // × 10000
adjustment  = coeffX100 * diff * polynomial / ADJUSTMENT_SCALE;     // tenths, ADJUSTMENT_SCALE = 1000000
target      = clamp(inside + shift - adjustment, lowerLimit, upperLimit);
```
All divisions are integer divisions (truncated toward zero). `calculateSpaceHeatingTargetTemp()` passes `burner_low_limit` (`h/bLo`, default 38.0°C) and `heating_high_limit` (`h/sHi`, default 75.0°C) as limits; `BurnerRequestManager::setHeatingRequest()` then clamps the request to `heating_low_limit` (`h/sLo`, default 40.0°C) .. `heating_high_limit` (absolute 20.0-110.0°C).

**Weather mode room shift**: with `heating/weatherControl` on and a valid room reading, `shift += (targetTemp - room) × heating/roomCurveShiftFactor`. Room mode uses the curve without this shift.

### Parameters (`boiler/params/set/<name>`)

| Parameter | Default | Range |
|-----------|---------|-------|
| `heating/curveCoeff` | 1.4 | 0.5-4.0 |
| `heating/curveShift` | 0.0°C | -20.0 to 40.0°C |
| `heating/weatherControl` | on (struct default) | bool |
| `heating/outsideThreshold` | 15.0°C (150 tenths) | 5.0-25.0°C |
| `heating/roomOverheatMargin` | 2.0°C (20 tenths) | 1.0-5.0°C |
| `heating/roomCurveShiftFactor` | 2.0 | 1.0-4.0 |
| `heating/hysteresis` | 0.5°C (5 tenths) | 0.1-2.0°C |
| `heating/targetTemp` | 18.0°C (180 tenths) | 10.0-30.0°C |

### Worked Example (coeff 1.4, shift 0, room 20.0°C)

| Outside | diff (tenths) | polynomial | adjustment (tenths) | Curve result | Heating request |
|---------|---------------|------------|---------------------|--------------|-----------------|
| 10.0°C | -100 | 12495 | -174 | 37.4 → 38.0°C (h/bLo) | 40.0°C (h/sLo) |
| 0.0°C | -200 | 11139 | -311 | 51.1°C | 51.1°C |
| -10.0°C | -300 | 10279 | -431 | 63.1°C | 63.1°C |

Outside 0.0°C in detail: diffSquared = 40000 / 10 = 4000; polynomial = 14347 - 4200 + 992 = 11139; adjustment = 140 × (-200) × 11139 / 1000000 = -311 (truncated from -311.9); target = 200 + 0 + 311 = 511 = 51.1°C.

Weather mode, room 19.0°C, target 20.0°C, factor 2.0, outside 0.0°C: shift = 1.0 × 2.0 = +2.0°C (20); diff = -190, polynomial = 11252, adjustment = -299; target = 190 + 20 + 299 = 509 = 50.9°C.

### Heating Needed Decision (`HeatingControlTask`)

- **Heating OFF override** (`heatingOverrideOff`): not needed. While an autotune runs the demand is kept.
- **Weather mode** (`heating/weatherControl` on): needs a valid outside reading, otherwise not needed.
  - `SpaceHeatingPolicy::outsideColdForHeating()`: start when outside < `outsideThreshold`; while heating, stop only when outside >= `outsideThreshold` + `OUTSIDE_THRESHOLD_HYSTERESIS` (10 = 1.0°C). Threshold 15.0°C: starts at 14.9°C, keeps running at 15.9°C, stops at 16.0°C (`Heating not needed: outside 16.0°C >= stop limit 16.0°C (threshold + hysteresis)`).
  - Room overheat limit = `targetTemp` + `roomOverheatMargin`: while heating stop when room > limit (`Heating stopped: room X°C > overheat limit Y°C`); while off restart only when room < limit - `hysteresis`. Skipped without a valid room reading.
  - Needed = outside cold and room not overheated.
- **Room mode** (`heating/weatherControl` off): needs a valid room reading and `targetTemp` > 0, otherwise not needed. Start when room < `targetTemp`, stop when room >= `targetTemp` + `hysteresis` (target 20.0°C, hysteresis 0.5°C: stops at 20.5°C). The outside temperature is not used for the decision.

---

## Summary Table

| Algorithm | Purpose | Key Parameter | Location |
|-----------|---------|---------------|----------|
| **PID Control** | Temperature regulation | Kp/Ki/Kd gains, ±500 output limit | PIDControlModuleFixedPoint |
| **Pump Protection** | Motor lifespan | pumpProtectionMs (15s default), real changes only | RelayControlTask |
| **MQTT Queuing** | Message prioritization | 5 + 5 queues, pressure on 80 % / off 30 % | MQTTTask |
| **Sensor Fallback** | Stop on missing sensors | STARTUP/NORMAL/SHUTDOWN, no substitute values | TemperatureSensorFallback |
| **Mutex Retry** | Contention handling | 4 attempts, 10 ms delay, escalation after 5 failures | MutexRetryHelper |
| **Temperature Math** | Fixed-point ops | int16_t tenths, saturating | Temperature.h |
| **Relay Verification** | DELAY-aware checking | 2 mismatches = error, only commanded-OFF DELAY masked | RYN4ProcessingTask (handleReadTick) |
| **Modbus Arbitration** | Bus collision prevention | 500ms ticks, 10-tick cycle | ModbusCoordinator.h:116 |
| **Water Two-Threshold** | Tank heating control | tempLimitLow/High hysteresis, low < high | WaterChargePolicy.h |
| **Burner Demand Gate** | Heat demand arming | 6s decision age, 1°C tolerance, 10s retry | BurnerDemandGate.h |
| **Burner Transitions** | Burner state decisions | 10s no-mode grace, 15s/30s mode switch bounds | BurnerTransitions.h |
| **Heating Curve** | Space heating boiler target, weather/room mode | coeff 1.4, shift 0, outside hysteresis 1.0°C | HeatingCurve.h, SpaceHeatingPolicy.h |

---

**Document Version**: 0.1.0
**Last Updated**: 2026-09-15
**Author**: System Documentation
