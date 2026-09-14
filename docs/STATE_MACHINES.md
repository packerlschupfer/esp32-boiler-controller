# State Machine Architecture

## Overview

The boiler controller uses explicit state machines for managing burner operation and other stateful components. State machines provide:
- Predictable behavior
- Safety through explicit state transitions
- Timeout handling
- Anti-flapping protection
- Clear error handling paths

## Burner State Machine

**Files**:
- `include/modules/control/BurnerSMState.h` - `BurnerSMState` enum (no FreeRTOS/Arduino dependencies)
- `include/modules/control/BurnerTransitions.h` - next-state logic, `BurnerTransitions::step()` (header-only, native-testable)
- `include/modules/control/BurnerTransitionPolicy.h` - pure decision rules and state machine limits
- `include/modules/control/BurnerDemandGate.h` - rules for arming the heat demand
- `src/modules/control/BurnerStateMachine.h/cpp` - state registration, entry/exit actions, POST_PURGE completion, LOCKOUT and ERROR handlers

**Implementation**: `StateMachine<BurnerSMState>` template

### States

| State | ID | Description | Typical Duration |
|-------|----|-------------|------------------|
| IDLE | 0 | Burner off, waiting for demand | Variable |
| PRE_PURGE | 1 | Burner relays forced off before ignition | 2 seconds |
| IGNITION | 2 | Burner relays switched on for the requested mode, flame proxy checked | 3-5 seconds |
| RUNNING_LOW | 3 | Running at half power (POWER_BOOST off) | Variable |
| RUNNING_HIGH | 4 | Running at full power (POWER_BOOST on) | Variable |
| MODE_SWITCHING | 5 | Seamless water <-> heating switch while burning | Milliseconds, wait max 15 s, hard limit 30 s |
| POST_PURGE | 6 | Burner relays off after a stop, pumps follow their modes | `SafetyConfig::postPurgeMs` (default 90 s) |
| LOCKOUT | 7 | Safety lockout after failed ignition attempts | 5 minutes |
| ERROR | 8 | Emergency stop / safety failure | At least `SafetyConfig::errorRecoveryMs` (default 5 min) |

### State Transitions

```
        ┌────────┐
        │  IDLE  │◄──────────────────────────────────────────────┐
        └───┬────┘                                               │
            │ heat demand + active mode request                  │
            │ + safety OK + minimum off-time (20 s)              │
            ▼                                                    │
      ┌───────────┐  mode request or demand gone ──► IDLE        │
      │ PRE_PURGE │  safety failed ────────────────► ERROR       │
      └─────┬─────┘                                              │
            │ timeout (2 s)                                      │
            ▼                                                    │
      ┌───────────┐  no flame at 5 s ──► PRE_PURGE (retry)       │
      │ IGNITION  │  3rd failed attempt ──► LOCKOUT              │
      └─────┬─────┘                                              │
            │ flame (checked from 3 s)                           │
            ▼                                                    │
  ┌─────────────┐  power request  ┌──────────────┐               │
  │ RUNNING_LOW │◄───────────────►│ RUNNING_HIGH │               │
  └──────┬──────┘                 └──────┬───────┘               │
         │ mode change while safe and burning                    │
         ├──────────► MODE_SWITCHING ◄───┤                       │
         │              │ switched ──► RUNNING_LOW/HIGH          │
         │              │ no request, switch failed,             │
         │              │ 30 s hard timeout ──► POST_PURGE       │
         │              │ safety failed ──► ERROR                │
         │                                                       │
         │ stop (see "RUNNING → POST_PURGE")                     │
         ▼                                                       │
   ┌────────────┐  postPurgeMs elapsed ──────────────────────────┘
   │ POST_PURGE │  heat demand returns ──► PRE_PURGE
   └────────────┘

  LOCKOUT ── LOCKOUT_TIME_MS (5 min) or resetLockout() ──► IDLE
  ERROR   ── errorRecoveryMs elapsed + safety OK ──────────► IDLE
  IGNITION / RUNNING_LOW / RUNNING_HIGH:
          continuousSafetyMonitor() failed ──► emergencyStop() ──► ERROR
```

### State Machine Configuration

`StateMachine::update()` checks the configured timeout first (`timeInState > timeoutMs`) and only then runs the state handler.

| State | Handler | `timeoutMs` | On timeout |
|-------|---------|-------------|------------|
| IDLE | `BurnerTransitions::step()` | 0 | - |
| PRE_PURGE | `BurnerTransitions::step()` | `PRE_PURGE_TIME_MS` (2000) | IGNITION |
| IGNITION | `BurnerTransitions::step()` | `IGNITION_TIME_MS + IGNITION_BACKSTOP_MARGIN_MS` (7000) | LOCKOUT (backstop only) |
| RUNNING_LOW / RUNNING_HIGH | `BurnerTransitions::step()` | 0 | - |
| MODE_SWITCHING | `BurnerTransitions::step()` | `MODE_SWITCH_HARD_TIMEOUT_MS` (30000) | POST_PURGE |
| POST_PURGE | `handlePostPurgeState()` (duration) + `step()` (early restart) | 0 (manual check of `SafetyConfig::postPurgeMs`) | - |
| LOCKOUT | `handleLockoutState()` (stays) | `LOCKOUT_TIME_MS` (300000) | IDLE |
| ERROR | `handleErrorState()` | 0 | - |

### Transition Logic (`BurnerTransitions::step`)

The handlers of IDLE, PRE_PURGE, IGNITION, RUNNING_LOW/HIGH and MODE_SWITCHING, and the early restart check in POST_PURGE, call `BurnerStateMachine::runTransitionStep()`, which delegates to the pure function:

```cpp
BurnerTransitions::Decision BurnerTransitions::step(
    const Context& c,      // state, timeInStateMs, nowMs, heatDemand, requestedHighPower
    const Timing& t,       // ignitionMinTimeMs, ignitionTimeoutMs, maxIgnitionRetries, modeDemandLossGraceMs
    Environment& env,      // inputs and the mode switch action
    Memory& m);            // ignitionRetries, runningModeIsWater, noModeDemandSinceMs
```

`Decision` carries the next state and a `Reason` (e.g. `STALE_DEMAND`, `EXPLICIT_DISABLE`, `NO_MODE_DEMAND`, `RESTART_FROM_POST_PURGE`) that `logTransitionDecision()` turns into log messages.

In firmware the `Environment` is `FirmwareTransitionEnvironment` (in `BurnerStateMachine.cpp`). It reads the SystemState event bits and the burner request bits once per tick; safety check, flame proxy, power limit, anti-flapping and settings are probed lazily, only when the logic asks for them, because `BurnerSystemController::performSafetyCheck()` can perform an emergency shutdown and several probes take mutexes.

**Active mode request** - used by IDLE, PRE_PURGE, RUNNING and POST_PURGE:

```cpp
(HEATING_ON && BurnerRequest::HEATING) || (WATER_ON && BurnerRequest::WATER)
```

**Mode selection** - when both `WATER_ON` and `HEATING_ON` are set, `WATER_PRIORITY` decides:

```cpp
bool isWater = WATER_ON && (!HEATING_ON || WATER_PRIORITY);
```

#### IDLE → PRE_PURGE
```cpp
// Conditions:
✓ heatDemand set
✓ Active mode request present
✓ BurnerSafetyChecks::checkSafetyConditions() passed
✓ BurnerAntiFlapping::canTurnOn() (MIN_OFF_TIME_MS, 20 s)

// Stale demand:
// heatDemand is latched and survives emergencyStop()/ERROR recovery.
// Without an active mode request IDLE ignores it (Reason::STALE_DEMAND,
// "Ignoring stale heat demand" logged at most once per 60 s).

// Actions on entry (onEnterPrePurge):
- BurnerSystemController::deactivate() - burner relays OFF
- Deactivate failure -> emergencyStop()
```

#### PRE_PURGE → IGNITION / IDLE / ERROR
```cpp
// Checked every tick, in this order:
✗ Safety check failed           -> ERROR
✗ No active mode request        -> IDLE (Reason::MODE_WITHDRAWN)
✗ heatDemand cleared            -> IDLE (Reason::DEMAND_WITHDRAWN)

// PRE_PURGE_TIME_MS (2 s) elapsed -> IGNITION (StateMachine timeout)
```

#### IGNITION
```cpp
// Actions on entry (onEnterIgnition):
- Increment COUNTER_BURNER_STARTS (FRAM)
- Select mode (WATER_ON / HEATING_ON / WATER_PRIORITY), remember it as running mode
- Record start power level for anti-flapping (from requestedHighPower)
- activateWaterMode() or activateHeatingMode() with FULL or HALF power
- Activation failure -> set BURNER_ERROR bit, flame proxy stays false
- vTaskDelay(BURNER_IGNITION_DELAY_MS)   // 500 ms

// Handler:
timeInState < BURNER_MIN_IGNITION_TIME_MS (3 s)  -> stay
flame detected                                   -> retries = 0,
    RUNNING_HIGH if BurnerPowerController::shouldIncreasePower(requestedHighPower)
    else RUNNING_LOW
timeInState >= IGNITION_TIME_MS (5 s)            -> retries++
    retries >= MAX_IGNITION_RETRIES (3)          -> LOCKOUT
    else                                         -> PRE_PURGE (retry)
```

The StateMachine timeout of IGNITION (`IGNITION_TIME_MS + IGNITION_BACKSTOP_MARGIN_MS`, 7 s) is only a backstop behind the handler. With the same 5 s value the timeout (checked before the handler) won almost every tick and a failed start went straight to LOCKOUT without the retries.

**Flame detection**: no flame sensor is installed. `BurnerSafetyChecks::isFlameDetected()` returns `BurnerSystemController::isActive()` (burner relays commanded on).

**Retry counter**: reset on successful ignition and by `resetLockout()`. It is not reset when LOCKOUT expires automatically and not persisted (a reboot starts at 0).

#### Entering RUNNING_LOW / RUNNING_HIGH
```cpp
// Actions on entry (onEnterRunningLow / onEnterRunningHigh):
- BurnerSystemController::setPowerLevel(HALF / FULL)
- Set BURNER_ON bit (RUNNING_LOW also clears BURNER_ERROR)
- BurnerRuntimeTracker::recordStartTime()

// Power level change refused (relay rate limit, queue full, Modbus error):
- CentralizedFailsafe::triggerFailsafe(DEGRADED, RELAY_OPERATION_FAILED,
                                       "Failed to set power level to HIGH/LOW")
- Fault 1-2 within POWER_FAULT_WINDOW_MS (10 min) -> POST_PURGE
- Fault 3 (POWER_FAULT_MAX_COUNT) within the window -> emergencyStop() -> ERROR

// Actions on exit (onExitRunning):
- Clear BURNER_ON bit
- BurnerRuntimeTracker::updateRuntimeCounters()
```

#### RUNNING → MODE_SWITCHING / POST_PURGE
```cpp
// Checked every tick, in this order:
1. Mode change (running mode != WATER_ON && (!HEATING_ON || WATER_PRIORITY))
     safety OK and flame                   -> MODE_SWITCHING (Reason::SWITCH_SEAMLESS)
     otherwise                             -> POST_PURGE     (Reason::SWITCH_NEEDS_STOP)
   heatDemand is not checked here: the old mode clears it before the new mode sets it.

2. Explicit disable                        -> POST_PURGE     (Reason::EXPLICIT_DISABLE)
   BurnerTransitionPolicy::stopForExplicitDisable(): BOILER_ENABLED cleared, or the
   *_ENABLED bit of the running mode cleared (running mode from the relays,
   BurnerSystemController::getCurrentMode()). Bypasses MIN_ON_TIME_MS.
   Disabling water while heating runs does not stop the burner.

3. No active mode request for MODE_DEMAND_LOSS_GRACE_MS (10 s)
                                           -> POST_PURGE     (Reason::NO_MODE_DEMAND)
   Bypasses anti-flapping: with no mode active nothing guarantees circulation.

4. heatDemand cleared or safety check failed
     BurnerAntiFlapping::canTurnOff() (MIN_ON_TIME_MS, 2 min)
                                           -> POST_PURGE     (Reason::DEMAND_ENDED)
     otherwise keep running ("Delaying burner stop")

5. Flame lost (burner relays no longer active)
                                           -> POST_PURGE     (Reason::FLAME_LOST)
   Bypasses anti-flapping.

6. Power level (see "Power Level State Machine")
```

`performSafetyCheck()` itself calls `BurnerSystemController::emergencyShutdown()` when the `EMERGENCY_STOP` bit is set or the boiler output or return temperature reaches `MAX_BOILER_TEMP_C` (110.0°C). The burner relays are then off, so step 5 stops the burner on the same tick. A failure from `checkSystemErrors()` alone (`SENSOR_FAILURE`, `MODBUS` or `RELAY` error bits) waits for the minimum on-time.

#### MODE_SWITCHING
```cpp
// Checked every tick:
✗ Safety check failed -> ERROR

// New mode from the burner request bits (the ON bits may not be set yet):
toWater = BurnerRequest::WATER && (!BurnerRequest::HEATING || WATER_PRIORITY)

// a) New mode has no request (water -> heating handover):
//    BurnerTransitionPolicy::onNoDemandForNewMode()
    heating likely wanted and timeInState < MODE_SWITCH_MAX_WAIT_MS (15 s) -> wait
    otherwise                                                              -> POST_PURGE

// b) Request points back to the running mode:
//    BurnerTransitionPolicy::onModeReverted()
    running mode's ON bit set                    -> RUNNING_LOW (safe low power)
    ON bit not set, timeInState < 15 s           -> wait (no RUNNING <-> MODE_SWITCHING bounce)
    ON bit still not set after 15 s              -> POST_PURGE

// c) Otherwise switch the relays: BurnerSystemController::switchMode()
    failure                                      -> POST_PURGE
    success                                      -> RUNNING_HIGH / RUNNING_LOW
                                                    (shouldIncreasePower)

// MODE_SWITCH_HARD_TIMEOUT_MS (30 s) -> POST_PURGE (StateMachine timeout)
```

"Heating likely wanted" (`BurnerTransitionPolicy::heatingLikelyWanted()`) mirrors HeatingControlTask's turn-on decision: `HEATING_ENABLED` set and no heating override off; with weather compensation the outside temperature must be valid and below the heating threshold and the room not above target + overheat margin; without weather compensation the room temperature must be valid and below target.

Anti-flapping power levels are not recorded for transitions into or out of MODE_SWITCHING.

#### RUNNING → POST_PURGE
Summary of the stop rules above:

| Trigger | Minimum on-time (2 min) |
|---------|-------------------------|
| Heat demand ended / safety check failed | Respected |
| Explicit disable of the running mode or the boiler | Bypassed |
| No active mode request for 10 s | Bypassed |
| Flame lost | Bypassed |
| Mode change without safe, burning state | Bypassed |
| Power level change refused on entering RUNNING (fault 1-2) | Bypassed |

```cpp
// Actions on entry (onEnterPostPurge):
- Record entry time
- BurnerSystemController::deactivate() - burner relays 1-3 OFF, pumps untouched
- Deactivate failure -> emergencyStop()
```

#### POST_PURGE → IDLE / PRE_PURGE
```cpp
// Duration (handlePostPurgeState):
elapsed >= SafetyConfig::postPurgeMs (default 90 s, 30-180 s via MQTT) -> IDLE

// Early restart (BurnerTransitions::step), same conditions as a start from IDLE:
✓ heatDemand set
✓ Active mode request present
✓ Mode that would start not explicitly disabled (stopForExplicitDisable)
   - its request can still be set for one control cycle after an explicit-disable stop
✓ Safety check passed
✓ BurnerAntiFlapping::canTurnOn() (MIN_OFF_TIME_MS, 20 s)
-> PRE_PURGE (Reason::RESTART_FROM_POST_PURGE)
```

POST_PURGE only keeps the burner off; the pumps are controlled independently by PumpControlModule. The duration is `SafetyConfig::postPurgeMs`.

#### LOCKOUT
```cpp
// Entered from IGNITION after MAX_IGNITION_RETRIES failed attempts
// (or by the IGNITION backstop timeout)

// Actions on entry (onEnterLockout):
- BurnerSystemController::deactivate() (fallback: emergencyShutdown())
- ALARM relay (relay 8) ON
- HealthMonitor records IGNITION_FAILURE

// Exit:
- LOCKOUT_TIME_MS (5 min) elapsed -> IDLE (retry counter kept)
- resetLockout() (MQTT burner_reset command, payload "lockout" or "reset")
                                  -> IDLE (retry counter reset)

// Actions on exit (onExitLockout):
- Clear BURNER_ERROR bit, ALARM relay OFF
```

Because the retry counter is kept on automatic expiry, one more failed ignition after the lockout expires locks out again.

#### ANY → ERROR
```cpp
// BurnerStateMachine::emergencyStop() is called on:
✗ SafetyInterlocks::continuousSafetyMonitor() failed in IGNITION / RUNNING_LOW / RUNNING_HIGH
✗ Invalid state value
✗ PRE_PURGE or POST_PURGE entry action could not deactivate the burner
✗ 3rd power level fault within 10 min
✗ BurnerControlTask: EMERGENCY_STOP system bit, failed safety event check,
  sensor fallback cannot continue while demand is active, maximum runtime exceeded,
  BurnerSafetyValidator failure other than thermal shock / sensor / pump failure
// PRE_PURGE and MODE_SWITCHING also return ERROR directly on a failed safety check.

// emergencyStop():
- BurnerSystemController::emergencyShutdown() - burner relays 1-3 OFF
- Clear BURNER_ON bit
- transitionTo(ERROR)

// Actions on entry (onEnterError):
- Record entry time
- BurnerSystemController::emergencyShutdown()
```

**Recovery is automatic**:
- ERROR is held for `SafetyConfig::errorRecoveryMs` (default 5 min, range 1-30 min)
- Every `STATUS_PUBLISH_INTERVAL_MS` (30 s) `{"state":"error","recovery_in":<seconds>}` is published to `boiler/status/burner`
- After the delay, if `checkSafetyConditions()` passes: clear `BURNER_ERROR`, go to IDLE
- `resetLockout()` only acts in LOCKOUT, not in ERROR
- `checkSafetyConditions()` fails while `EMERGENCY_STOP` is set (`CentralizedFailsafe::emergencyStop()`). Release it with `boiler/cmd/emergency_reset` (payload `reset`, refused while the causes persist, see [SAFETY_SYSTEM.md](SAFETY_SYSTEM.md)); the recovery delay still applies
- `heatDemand` is not cleared by ERROR; IDLE ignores it until an active mode request exists

### Heat Demand Arming (`BurnerDemandGate`)

Two tasks write `BurnerStateMachine::setHeatDemand(demand, target, highPower)`:

**BurnerControlTask** (`updateBurnerState()`) on request changes, and every 10 s while a request is present but not permitted:
1. Runs its blocks: sensor staleness, `BurnerSafetyValidator`, return preheating, sensor fallback limits
2. Publishes a `Permission { permitted, highPowerAllowed, maxTargetTemp }`
3. Not permitted -> `setHeatDemand(false)`
4. Permitted -> arms ON only if `BurnerDemandGate::controlTaskMayArm()`:
   - boiler output temperature invalid -> arm (sensor fallback operation)
   - BoilerTempControlTask decision fresh (`DECISION_MAX_AGE_MS`, 6 s) and made for about the same target (`DECISION_TARGET_TOLERANCE`, 1.0°C) -> follow that decision
   - otherwise -> arm only if boiler output < target

**BoilerTempControlTask** on every boiler output update (~2.5 s) while a heating or water request exists:
1. Caps the target with `permission.maxTargetTemp`, runs the PID, publishes its decision
2. Applies `BurnerDemandGate::decide(permitted, pidWantsHeat, outputChanged, demandArmed)` on every cycle:
   - `ARM` - permitted, PID wants heat, demand not armed
   - `SET_POWER` - armed and PID output changed
   - `DISARM` - armed but not permitted or PID wants OFF
3. `ARM` and `SET_POWER` still require `BurnerSafetyValidator`; high power only if the PID wants FULL and `permission.highPowerAllowed`

The demand therefore follows "permitted AND PID wants heat". Previously BurnerControlTask armed ON on every request start and BoilerTempControlTask only re-asserted OFF on its next cycle, after the 2 s pre-purge, so the burner could ignite while the boiler was above target.

When `BOILER_ENABLED` is cleared, BurnerControlTask deactivates the burner relays, revokes the permission and clears the demand.

### Anti-Flapping Protection

Prevents rapid cycling that damages equipment:

| Timer | Duration | Purpose |
|-------|----------|---------|
| Minimum ON time | 120 seconds (`MIN_ON_TIME_MS`) | Prevent short cycles |
| Minimum OFF time | 20 seconds (`MIN_OFF_TIME_MS`) | Allow cooling |
| Power change interval | 15 seconds (`MIN_POWER_CHANGE_INTERVAL_MS`) | Prevent power oscillation |

**Implementation**: `BurnerAntiFlapping.cpp`

The minimum ON time only delays a stop for "heat demand ended" or "safety check failed". Explicit disable, loss of the mode request, flame loss, a mode change that cannot be done seamlessly and a refused power level change stop the burner immediately. Faults that call `emergencyStop()` go to ERROR regardless.

Power levels are recorded in the transition callback (`logStateTransition()`): IDLE, PRE_PURGE, POST_PURGE, LOCKOUT and ERROR count as OFF. IGNITION records the start power in `onEnterIgnition()`.

### Safety Interlocks

**Before a start** (IDLE / POST_PURGE restart) and on every RUNNING, PRE_PURGE and MODE_SWITCHING tick, `BurnerSafetyChecks::checkSafetyConditions()` calls `BurnerSystemController::performSafetyCheck()`:

```cpp
Result<void> performSafetyCheck() {
    ✓ No EMERGENCY_STOP bit                  // else emergencyShutdown()
    ✓ Boiler temperatures < MAX_BOILER_TEMP_C (110.0°C)   // else emergencyShutdown()
    ✓ No SENSOR_FAILURE / MODBUS / RELAY error bits
}
```

**During IGNITION / RUNNING_LOW / RUNNING_HIGH**, `BurnerStateMachine::update()` also runs `SafetyInterlocks::continuousSafetyMonitor()` (skipped while `BOILER_ENABLED` is cleared); a failure calls `emergencyStop()`:

```cpp
bool continuousSafetyMonitor() {
    ✓ No EMERGENCY_STOP bit
    ✓ Boiler temperatures < CRITICAL_BOILER_TEMP_C (115.0°C)
    ✓ Boiler output sensor not stale      // else also CentralizedFailsafe::emergencyStop()
    // Periodic full check while HEATING_ON or WATER_ON is set:
    ✓ Minimum 2 valid temperature sensors
    ✓ Temperature limits and thermal shock differential
    ✓ Communication, pressure, system errors
}
```

Request validation before the demand is armed (`BurnerSafetyValidator`) is described in `docs/SAFETY_SYSTEM.md`.

**Files**: `BurnerSafetyChecks.cpp`, `BurnerSystemController.cpp`, `SafetyInterlocks.cpp`

## Power Level State Machine

Nested within RUNNING states:

```
RUNNING_LOW ←──────────────┐
     │                      │ !requestedHighPower
     │ shouldIncreasePower()│ + canChangePowerLevel()
     ▼                      │
RUNNING_HIGH ──────────────┘
```

**Decision Logic**:
- `requestedHighPower` comes from `setHeatDemand()` (BoilerTempControlTask PID output FULL, limited by the sensor fallback permission)
- `BurnerPowerController::shouldIncreasePower()` blocks high power while the boiler output is >= 80.0°C
- `BurnerAntiFlapping::canChangePowerLevel()` enforces `MIN_POWER_CHANGE_INTERVAL_MS` (15 s)

## Relay State Management

**Files**: `src/modules/tasks/RelayControlTask.cpp`, `include/config/RelayIndices.h`

```
Relay State: OFF ──┐
                   │ setRelayState(ON)
                   ▼
                  ON
                   │ setRelayState(OFF)
                   ▼
                  OFF
```

**Relay Functions** (physical relay numbers):
1. BURNER_ENABLE (1) - heating mode, half power
2. POWER_BOOST (2) - ON = full power, OFF = half power
3. WATER_MODE (3) - water mode, half power
4. VALVE (4)
5. HEATING_PUMP (5)
6. WATER_PUMP (6)
7. ALARM (8)

The burner (`BurnerSystemController`) only commands relays 1-3. Pumps are owned by PumpControlModule.

**Command protection** (`RelayControlTask::processSingleRelay()`):
- Rate limiting (`MIN_RELAY_SWITCH_INTERVAL_MS` 150 ms, `MAX_RELAY_TOGGLE_RATE_PER_MIN` 30) and pump motor protection (`SafetyConfig::pumpProtectionMs`) apply only to real state changes (`RelayCommandPolicy::appliesProtection()`). A mode switch sends all three burner relays; unchanged relays in that batch no longer count as toggles and cannot make the following power level change fail.
- `setRelayStateEmergency()` bypasses rate limiting and pump protection.
- `setRelayState()` skips a command for a relay already in the desired state, except OFF commands to burner relays 1-3.

**State Tracking**: `SharedRelayReadings` updated atomically with mutex protection

## Pump Control

The pumps have no multi-step state machine. `PumpControlModule::PumpControlTask()` runs once per pump (HeatingPumpTask, WaterPumpTask) and keeps a logical `PumpState` (`Off`/`On`; `Error` is defined but never set). Every 500 ms (`PUMP_CHECK_INTERVAL_MS`) it recomputes the desired state; later rows override earlier ones:

```
desired = OFF
BOILER_ENABLED && mode bit set (HEATING_ON / WATER_ON)  -> ON, overrun cancelled
BOILER_ENABLED && mode bit cleared since last check      -> start overrun
overrun running && elapsed < pumpCooldownMs              -> ON  (SystemSettings, default 300000 ms = 5 min)
BOILER_ENABLED cleared                                    -> OFF (no overrun)
heating pump only: ReturnPreheater PREHEATING            -> ReturnPreheater::shouldPumpBeOn()
EMERGENCY_STOP set                                        -> ON  (heat dissipation, overrides all) until boiler
                                                             output < 60.0°C, ON again from 65.0°C; always ON
                                                             without a valid, fresh reading
```

On a change the task sets the relay request bit (`RelayRequest::HEATING_PUMP_ON/OFF`, `WATER_PUMP_ON/OFF`), sets or clears `SystemState::HEATING_PUMP_ON`/`WATER_PUMP_ON` and counts pump starts in FRAM. RelayControlTask applies pump motor protection (`SafetyConfig::pumpProtectionMs`). Both pumps behave the same (the water pump also has the overrun). The pumps do not wait for the burner, and the burner does not check the pumps.

### Return Preheating (`ReturnPreheater`)

```
IDLE --start()--> COMPLETE      preheatEnabled false, or differential already below 25.0°C
IDLE --start()--> PREHEATING    heating pump cycling, cycle 1
PREHEATING -----> COMPLETE      output - return below SAFE_DIFFERENTIAL (25.0°C)
PREHEATING -----> TIMEOUT       preheatTimeoutMs (default 600 s) or more than preheatMaxCycles (default 8)
COMPLETE/TIMEOUT --reset()--> IDLE   BurnerControlTask, once there is no heat demand
```

- `start()`: BurnerControlTask or BoilerTempControlTask, when `BurnerSafetyValidator` returns `THERMAL_SHOCK_RISK` (output more than 35.0°C above return)
- `update()`: BurnerControlTask
- Per cycle the ON time grows and the OFF time shrinks (`ReturnPreheat::CYCLE_n_ON_SEC`/`CYCLE_n_OFF_SEC`, OFF scaled by `preheatOffMultiplier`), at least `preheatPumpMinMs` (default 3 s) between pump changes

## Temperature Control

There is no temperature control state enum. BoilerTempControlTask waits for `SensorUpdate::BOILER_OUTPUT` (~2.5 s) and, while a burner request is active and no autotune runs, calls `BoilerTempController::calculate(target, boilerOutput)`. The only state carried between cycles is the last power level (OFF/HALF/FULL). An invalid target or boiler temperature gives OFF.

**`useBoilerTempPID` true (default, `BurnerType::MODULATING`)**: PID output 0-100% (50% = at target), gains of the active mode (`pid/spaceHeating/*` or `pid/waterHeater/*`), mapped with hysteresis (`offThreshold` 35, `halfThreshold` 45, `fullThreshold` 75, `thresholdHysteresis` 10):

```
OFF  -> HALF   output > 55 (FULL if output > 75)
HALF -> OFF    output < 35
HALF -> FULL   output > 75
FULL -> OFF    output < 35
FULL -> HALF   output < 65
```

**`useBoilerTempPID` false (`BurnerType::TWO_STAGE`)**: bang-bang on error = target - current: error below `-offHysteresis` -> OFF, above `fullPowerThreshold` -> FULL, above `onHysteresis` -> HALF, otherwise the previous level.

In both modes a level change is dropped when `BurnerAntiFlapping::canChangePowerLevel()` refuses it. The resulting level drives the heat demand through `BurnerDemandGate` (see Heat Demand Arming).

## State Machine Framework

**Template**: `src/utils/StateMachine.h`

```cpp
template<typename StateEnum>
class StateMachine {
public:
    struct StateConfig {
        StateHandler handler;           // Returns the next state
        ActionCallback onEntry;
        ActionCallback onExit;
        uint32_t timeoutMs;             // 0 = no timeout
        StateEnum timeoutNextState;
    };

    void registerState(StateEnum state, const StateConfig& config);
    void setTransitionCallback(TransitionCallback callback);
    void initialize();
    void update();                      // Timeout check, then handler

    void transitionTo(StateEnum newState);
    void reset(StateEnum initialState);

    StateEnum getCurrentState() const;
    StateEnum getPreviousState() const;
    uint32_t getTimeInState() const;    // Milliseconds
    bool isInState(StateEnum state) const;
};
```

**Features**:
- Automatic timing (time in state)
- Per-state timeout with target state (checked before the handler, `>` comparison)
- Transition logging and transition callback
- Previous state tracking

**Transition order** (`transitionTo()`): exit action of the old state, state update, transition callback, entry action of the new state. An entry action may itself call `transitionTo()` (e.g. `handlePowerLevelFault()` moves to POST_PURGE from `onEnterRunningLow()`).

**Usage Pattern**:
```cpp
// In BurnerStateMachine.cpp
StateMachine<BurnerSMState> BurnerStateMachine::stateMachine("BurnerSM", BurnerSMState::IDLE);

BurnerSMState BurnerStateMachine::runTransitionStep() {
    const BurnerTransitions::Context ctx = {
        stateMachine.getCurrentState(), stateMachine.getTimeInState(),
        millis(), heatDemand, requestedHighPower
    };
    FirmwareTransitionEnvironment env(TAG, targetTemperature);
    const BurnerTransitions::Decision decision =
        BurnerTransitions::step(ctx, TRANSITION_TIMING, env, transitionMemory);
    logTransitionDecision(ctx, decision, env.systemBits(), env.requestBits());
    return decision.next;   // StateMachine::update() transitions if it differs
}
```

## State Persistence

The emergency state is persisted to FRAM by `CentralizedFailsafe::saveEmergencyState()` when the failsafe level first reaches CRITICAL or higher, and on an orderly shutdown. `BurnerStateMachine::emergencyStop()` does not write it.

```cpp
struct EmergencyState {
    uint32_t magic;           // 0xDEADBEEF when valid
    uint32_t timestamp;       // When emergency occurred
    uint8_t reason;           // Emergency reason code
    uint8_t activeRelays;     // Relay state before shutdown
    float lastBoilerTemp;     // Last known boiler temp
    float lastPressure;       // Last known pressure
    bool wasHeating;          // Was heating active
    bool wasWaterActive;      // Was water heating active
    uint32_t errorCode;       // Associated error code
    uint32_t crc;             // CRC32 of data
} __attribute__((packed));
```

**File**: `src/utils/CriticalDataStorage.h`

The burner ignition retry counter is intentionally not persisted.

## State Transition Rules

### Timing-Based Transitions

States with automatic timeout transitions:

```cpp
// PRE_PURGE: StateMachine timeout
if (timeInState > PRE_PURGE_TIME_MS) {                                   // 2 s
    return IGNITION;
}

// IGNITION: handler retries or locks out, StateMachine timeout is a backstop
if (timeInState >= IGNITION_TIME_MS) {                                   // 5 s
    if (++retries >= MAX_IGNITION_RETRIES) return LOCKOUT;
    return PRE_PURGE;  // Retry
}
if (timeInState > IGNITION_TIME_MS + IGNITION_BACKSTOP_MARGIN_MS) {      // 7 s
    return LOCKOUT;
}

// MODE_SWITCHING: bounded waits in the handler, StateMachine hard timeout
if (timeInState > MODE_SWITCH_HARD_TIMEOUT_MS) {                         // 30 s
    return POST_PURGE;
}

// POST_PURGE: manual check, runtime-configurable
if (timeInPostPurge >= SafetyConfig::postPurgeMs) {                      // default 90 s
    return IDLE;
}

// LOCKOUT: automatic reset
if (timeInState > LOCKOUT_TIME_MS) {                                     // 5 min
    return IDLE;
}

// ERROR: recovery delay, then safety check
if (timeInError >= SafetyConfig::errorRecoveryMs && checkSafetyConditions()) {
    return IDLE;
}
```

### Condition-Based Transitions

States that transition based on conditions:

```cpp
// IDLE: start only for an active mode request
if (heatDemand && hasActiveModeDemand() && safetyOK && canTurnOn()) {
    return PRE_PURGE;
}

// PRE_PURGE: abort if the request or the demand goes away
if (!safetyOK) return ERROR;
if (!hasActiveModeDemand() || !heatDemand) return IDLE;

// RUNNING: checked continuously (order matters)
if (modeChanged)                       return (safetyOK && flame) ? MODE_SWITCHING : POST_PURGE;
if (stopForExplicitDisable(...))       return POST_PURGE;
if (noModeRequestFor >= 10 s)          return POST_PURGE;
if ((!heatDemand || !safetyOK) && canTurnOff()) return POST_PURGE;
if (!flameDetected)                    return POST_PURGE;

// POST_PURGE: restart when demand returns
if (heatDemand && hasActiveModeDemand() && !modeDisabled && safetyOK && canTurnOn()) {
    return PRE_PURGE;
}
```

### Power Level Transitions

Within RUNNING states, power can change:

```cpp
// RUNNING_LOW → RUNNING_HIGH
if (shouldIncreasePower(requestedHighPower) && canChangePowerLevel(POWER_HIGH)) {
    return RUNNING_HIGH;
}

// RUNNING_HIGH → RUNNING_LOW
if (!requestedHighPower && canChangePowerLevel(POWER_LOW)) {
    return RUNNING_LOW;
}
```

**Power Decision Factors**:
- PID power level from BoilerTempControlTask (`requestedHighPower`)
- High power blocked while boiler output >= 80.0°C
- High power blocked while sensor fallback reduces the power factor
- Anti-flapping timer (15 s minimum between power changes)

## Safety State Transitions

### Emergency Transitions (Bypass Normal Flow)

Certain conditions trigger immediate state changes:

```cpp
// IGNITION / RUNNING_LOW / RUNNING_HIGH → ERROR
if (!SafetyInterlocks::continuousSafetyMonitor()) {
    emergencyStop();  // Burner relays off, then ERROR
    return;
}

// Examples of critical failures:
- EMERGENCY_STOP bit set
- Boiler temperature >= CRITICAL_BOILER_TEMP_C (115.0°C)
- Boiler output sensor stale during operation
- Full interlock check failed (sensors, pressure, communication, thermal shock)
```

### Emergency Shutdown Paths

| Function | Relays switched | Pumps | Further actions |
|----------|-----------------|-------|-----------------|
| `BurnerSystemController::emergencyShutdown()` | BURNER_ENABLE, POWER_BOOST, WATER_MODE OFF via `setRelayStateEmergency()` | Not touched (PumpControlModule keeps following the mode bits) | On relay failure: set `Error::RELAY` and `Error::SAFETY` bits, return `RELAY_OPERATION_FAILED` |
| `BurnerStateMachine::emergencyStop()` | Via `emergencyShutdown()` | Not touched | Clear `BURNER_ON`, go to ERROR |
| `CentralizedFailsafe::emergencyStop()` | Via `emergencyShutdown()`, then burner relays OFF again | Both pumps forced ON via `setRelayStateEmergency()`; PumpControlModule keeps them on until the boiler output is below 60.0°C | Set `EMERGENCY_STOP`, clear `BOILER_ENABLED`, log error |

`CentralizedFailsafe::emergencyStop()` is reached through `SafetyInterlocks::triggerEmergencyShutdown()` (e.g. stale sensor data during operation, critical temperature, burner request watchdog after `REQUEST_EXPIRATION_MS`) and the EMERGENCY failsafe level.

`EMERGENCY_STOP` is a level latch: BurnerControlTask reads it without clearing and calls `BurnerStateMachine::emergencyStop()` once per onset (`EmergencyStopRelease::onsetDetected()`). While it is set the safety check fails, so the burner stays in ERROR. It is released by `TemperatureSensorFallback` on sensor recovery (SHUTDOWN -> NORMAL) or by the MQTT command `boiler/cmd/emergency_reset` (`CentralizedFailsafe::clearEmergencyStop()`), which also sets `BOILER_ENABLED` again if the saved boiler setting is enabled. The burner then leaves ERROR through the normal recovery delay.

### Non-Blocking Safety

Some failures don't force ERROR state:

```cpp
// Refused power level change: DEGRADED failsafe + POST_PURGE
// (only the 3rd fault within 10 min escalates to ERROR)

// Thermal shock risk at request validation: demand blocked, return preheating started
// Sensor or pump failure at request validation: demand blocked, no emergency stop

// Pressure sensor missing -> degraded mode when ALLOW_NO_PRESSURE_SENSOR is defined
```

## State Entry/Exit Actions

### onEnterPrePurge()
```cpp
- BurnerSystemController::deactivate() (burner relays OFF)
- Failure -> emergencyStop()
```

### onEnterIgnition()
```cpp
- Increment burner start counter (COUNTER_BURNER_STARTS)
- Select mode, store running mode for switch detection
- Record start power level for anti-flapping
- activateWaterMode() / activateHeatingMode() (FULL or HALF)
- Failure -> set BURNER_ERROR bit (no flame -> retry path)
- Wait BURNER_IGNITION_DELAY_MS (500 ms)
```

### onEnterRunningHigh()
```cpp
- setPowerLevel(FULL): POWER_BOOST=ON
- Failure -> handlePowerLevelFault(true)
- Set BURNER_ON bit
- BurnerRuntimeTracker::recordStartTime()
```

### onEnterRunningLow()
```cpp
- setPowerLevel(HALF): POWER_BOOST=OFF
- Failure -> handlePowerLevelFault(false)
- Clear BURNER_ERROR, set BURNER_ON bit
- BurnerRuntimeTracker::recordStartTime()
```

### onExitRunning()
```cpp
- Clear BURNER_ON bit
- BurnerRuntimeTracker::updateRuntimeCounters()
```

### onEnterModeSwitching()
```cpp
- Log only; the switch is done by the MODE_SWITCHING handler
```

### onEnterPostPurge()
```cpp
- Record entry time for SafetyConfig::postPurgeMs
- BurnerSystemController::deactivate() (burner relays OFF, pumps untouched)
- Failure -> emergencyStop()
```

### onEnterLockout()
```cpp
- BurnerSystemController::deactivate() (fallback: emergencyShutdown())
- ALARM relay ON
- HealthMonitor: IGNITION_FAILURE
```

### onExitLockout()
```cpp
- Clear BURNER_ERROR bit
- ALARM relay OFF
```

### onEnterError()
```cpp
- Record entry time for SafetyConfig::errorRecoveryMs
- BurnerSystemController::emergencyShutdown() (burner relays OFF)
```

## State Monitoring

### Via MQTT

**Topic**: `boiler/status/burner`

```json
{"state":"error","recovery_in":240}
```

Published every 30 s while in ERROR. `lockout_reset` is published (retained) after a `burner_reset` command; `emergency_released`, `emergency_not_active` or `emergency_release_refused:<reason>` (not retained) after an `emergency_reset` command.

### Via Serial Logs
```
[BurnerStateMachine] State transition: IDLE -> PRE_PURGE
[BurnerSM] State 1 completed after 2010 ms, transitioning to next state
[BurnerStateMachine] State transition: PRE_PURGE -> IGNITION
[BurnerStateMachine] Ignition successful after 3010 ms - transitioning to low power
[BurnerStateMachine] State transition: IGNITION -> RUNNING_LOW
[BurnerStateMachine] Space heating disabled - stopping burner now (minimum on-time bypassed)
[BurnerStateMachine] Heat demand returned during post-purge - restarting after 25000 ms
```

## State Machine Timing Constants

```cpp
// SystemConstants::Burner
PRE_PURGE_TIME_MS = 2000               // 2 seconds (atmospheric burner)
IGNITION_TIME_MS = 5000                // 5 seconds per attempt
LOCKOUT_TIME_MS = 300000               // 5 minutes
MAX_IGNITION_RETRIES = 3               // Failed attempts before lockout
MIN_ON_TIME_MS = 120000                // 2 minutes (anti-flapping)
MIN_OFF_TIME_MS = 20000                // 20 seconds (anti-flapping)
MIN_POWER_CHANGE_INTERVAL_MS = 15000   // 15 seconds (anti-flapping)
REQUEST_EXPIRATION_MS = 600000         // 10 minutes (request watchdog)

// SystemConstants::Timing
BURNER_MIN_IGNITION_TIME_MS = 3000     // Flame not checked before this
BURNER_IGNITION_DELAY_MS = 500         // Delay in onEnterIgnition()

// BurnerTransitions
MODE_DEMAND_LOSS_GRACE_MS = 10000      // RUNNING without active mode request

// BurnerTransitionPolicy
MODE_SWITCH_MAX_WAIT_MS = 15000        // MODE_SWITCHING bounded waits
MODE_SWITCH_HARD_TIMEOUT_MS = 30000    // MODE_SWITCHING StateMachine timeout
IGNITION_BACKSTOP_MARGIN_MS = 2000     // IGNITION StateMachine timeout = 7 s
POWER_FAULT_MAX_COUNT = 3              // Power level faults before emergency stop
POWER_FAULT_WINDOW_MS = 600000         // 10 minutes

// BurnerDemandGate
DECISION_MAX_AGE_MS = 6000             // BoilerTempControlTask decision freshness
DECISION_TARGET_TOLERANCE = 10         // 1.0°C

// SafetyConfig (runtime, NVS)
postPurgeMs      default 90000   range 30000-180000
errorRecoveryMs  default 300000  range 60000-1800000
```

## Failure Modes & Recovery

### Ignition Failure
```
Attempt 1: IDLE → PRE_PURGE → IGNITION → (no flame at 5 s) → PRE_PURGE
Attempt 2: PRE_PURGE → IGNITION → (no flame) → PRE_PURGE
Attempt 3: PRE_PURGE → IGNITION → (no flame) → LOCKOUT

After 5 minutes in LOCKOUT:
LOCKOUT → IDLE (automatic, retry counter kept) OR resetLockout() (counter reset)
```

### Flame Loss During Operation
```
RUNNING_HIGH → POST_PURGE → IDLE
(No lockout, minimum on-time bypassed)

Next demand → normal ignition sequence
```

### Mode Disabled While Running
```
RUNNING (heating) + HEATING_ENABLED cleared → POST_PURGE immediately
RUNNING (heating) + WATER_ENABLED cleared   → keeps running
RUNNING + BOILER_ENABLED cleared            → POST_PURGE immediately
```

### Mode Request Lost While Running
```
RUNNING, no (HEATING_ON + HEATING request) or (WATER_ON + WATER request)
  → after 10 s → POST_PURGE
```

### Power Level Relay Fault
```
Fault 1 and 2 within 10 min: RUNNING_x entry → DEGRADED failsafe → POST_PURGE
                             (restart from POST_PURGE when demand remains)
Fault 3 within 10 min:       emergencyStop() → ERROR
```

### Safety Interlock Failure
```
IGNITION / RUNNING → ERROR (immediate)
ERROR → IDLE (automatic after errorRecoveryMs if safety check passes)
```

### Communication Loss
```
Boiler output sensor stale during IGNITION / RUNNING:
  CentralizedFailsafe::emergencyStop() + BurnerStateMachine::emergencyStop() → ERROR
  (Cannot safely operate without sensor feedback)
```

## Watchdog Integration

State machine is monitored by watchdog:

```cpp
// BurnerControlTask feeds the watchdog after each event handler
while (true) {
    xEventGroupWaitBits(...);                 // 100 ms active, 3 s idle, else 1 s
    ...
    BurnerStateMachine::update();             // on STATE_TIMEOUT (1 s timer) and sensor updates
    (void)SRP::getTaskManager().feedWatchdog();
}
```

**Watchdog Timeout**: 15 seconds (`WDT_BURNER_CONTROL_MS`)
**Action on Timeout**: System reset (critical task)

## Testing State Machines

### Native Tests

The transition logic and policies are header-only and run without hardware:

```bash
pio test -e native_test
```

| File | Covers |
|------|--------|
| `test/test_native/test_burner_transitions.cpp` | `BurnerTransitions::step()` scenarios with a fake `Environment` (stale demand, start sequence, pre-purge abort, explicit disable, grace period, flame loss, mode switch and handover, post-purge restart, ignition retries) |
| `test/test_native/test_burner_transition_policy.cpp` | `BurnerTransitionPolicy` rules |
| `test/test_native/test_burner_demand_gate.cpp` | `BurnerDemandGate` arming rules |
| `test/test_native/test_relay_command_policy.cpp` | `RelayCommandPolicy` no-op handling |
| `test/test_native/test_stage_c_policies.cpp` | Power level fault escalation, water limit consistency |

### State History Logging
```cpp
BurnerSMState current = stateMachine.getCurrentState();
BurnerSMState previous = stateMachine.getPreviousState();
uint32_t timeInState = stateMachine.getTimeInState();
```

State changes to or from ERROR, and into LOCKOUT or IGNITION, are logged to FRAM (`EVENT_STATE_CHANGE`).

### Flame Proxy
```cpp
// No flame sensor installed: flame is assumed while the burner relays are active
bool BurnerSafetyChecks::isFlameDetected() {
    BurnerSystemController* controller = SRP::getBurnerSystemController();
    return controller ? controller->isActive() : false;
}
```

## State Diagram Legend

```
┌─────┐
│STATE│  Rectangle = Normal operating state
└─────┘

  ┌───┐
  │ERR│   Small rectangle = Error/fault state
  └───┘

  ─────▶  Solid arrow = Normal transition
  ┈┈┈┈▶  Dashed arrow = Error transition
  ✓       Condition that must be true
  ✗       Condition that must be false
  ◄─────  Bidirectional (can transition both ways)
```

## Future Enhancements

1. **State History Buffer** - Save last N transitions to FRAM
2. **State Duration Tracking** - Average time in each state
3. **Predictive Maintenance** - Detect degrading performance from timing changes
4. **Remote State Control** - MQTT commands to force states (testing)
5. **State Machine Visualization** - Generate diagrams from code
