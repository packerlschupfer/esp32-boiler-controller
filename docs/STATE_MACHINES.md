# State Machine Architecture

## Overview

The boiler controller uses explicit state machines for managing burner operation and other stateful components. State machines provide:
- Predictable behavior
- Safety through explicit state transitions
- Timeout handling
- Anti-flapping protection
- Clear error handling paths

## Burner State Machine

**File**: `src/modules/control/BurnerStateMachine.h/cpp`
**Implementation**: `StateMachine<BurnerSMState>` template

### States

| State | ID | Description | Typical Duration |
|-------|----|-----------| -----------------|
| IDLE | 0 | Burner off, waiting for demand | Variable |
| PRE_PURGE | 1 | Pre-purge airflow before ignition | 10 seconds |
| IGNITION | 2 | Ignition sequence (spark + gas) | 5 seconds |
| RUNNING_LOW | 3 | Running at 50% power | Variable |
| RUNNING_HIGH | 4 | Running at 100% power | Variable |
| POST_PURGE | 5 | Post-purge airflow after shutdown | 60 seconds |
| LOCKOUT | 6 | Safety lockout after failures | 5 minutes |
| ERROR | 7 | Error state (manual reset required) | Until fixed |

### State Transitions

```
┌────────┐
│  IDLE  │◄──────────────────────────────────┐
└───┬────┘                                    │
    │ Heat Demand + Safety OK                │
    │ + Anti-flap OK                         │
    ▼                                         │
┌──────────┐                                 │
│PRE_PURGE │  (10s timeout)                  │
└─────┬────┘                                 │
      │                                       │
      │ Safety Failed                        │
      ├──────────────────────┐               │
      │                      ▼               │
      │                  ┌───────┐           │
      │                  │ ERROR │           │
      │                  └───────┘           │
      │                                      │
      │ Timeout (10s)                       │
      ▼                                      │
┌──────────┐                                │
│ IGNITION │  (5s max)                      │
└─────┬────┘                                │
      │                                      │
      │ Flame Detected                      │
      ├────────────┬───────────┐            │
      │ Low Power  │ High Power│            │
      ▼            ▼            │            │
┌────────────┐ ┌─────────────┐│            │
│RUNNING_LOW │ │RUNNING_HIGH ││            │
└──┬─────────┘ └──┬──────────┘│            │
   │ Power ▲      │ Power ▼   │            │
   │ Increase     │ Decrease  │            │
   └──────┬───────┴─────┬─────┘            │
          │             │                   │
          │ No Demand   │                   │
          │ OR Safety   │                   │
          │ Failed      │                   │
          ▼             │                   │
     ┌────────────┐    │                   │
     │POST_PURGE  │◄───┘                   │
     └──────┬─────┘                         │
            │ (60s timeout)                 │
            └───────────────────────────────┘

     Ignition Failed (3× retries)
              ▼
         ┌─────────┐
         │ LOCKOUT │  (5 min timeout)
         └─────────┘
              │ Timeout OR Manual Reset
              └──────────► IDLE
```

### State Transition Logic

#### IDLE → PRE_PURGE
```cpp
// Conditions:
✓ Heat demand present
✓ All safety checks passed
✓ Anti-flapping timer expired (20s min off time)

// Actions on entry:
- Ensure burner relays OFF
- Start exhaust fan (if present)
- Log sequence start
```

#### PRE_PURGE → IGNITION
```cpp
// Conditions:
✓ 10 second timeout elapsed
✓ Safety conditions still OK

// Actions on entry:
- Enable ignition spark
- Open gas valve (controlled power)
- Start flame detection monitoring
- Increment burner start counter
```

#### IGNITION → RUNNING_HIGH/LOW
```cpp
// Conditions:
✓ Flame detected (via sensor OR assumed if no sensor)
✓ Within 5 second timeout

// Power level selection:
if (shouldIncreasePower()) → RUNNING_HIGH
else → RUNNING_LOW

// Actions on entry:
- Disable ignition spark
- Set power level (half/full)
- Reset ignition retry counter
- Start anti-flapping on-timer (2 min minimum runtime)
```

#### IGNITION → LOCKOUT
```cpp
// Conditions:
✗ No flame after 3 ignition attempts
✗ Each attempt includes PRE_PURGE + IGNITION cycle

// Actions on entry:
- Close gas valve
- Disable ignition
- Log failure
- Start 5 minute lockout timer
```

#### RUNNING → POST_PURGE
```cpp
// Conditions (any of):
✗ Heat demand removed
✗ Safety check failed
✗ Flame lost
✓ Anti-flapping minimum runtime elapsed (2 min)

// Actions on entry:
- Close gas valve
- Keep fan running for purge
- Start 60 second post-purge timer
```

#### POST_PURGE → IDLE
```cpp
// Conditions:
✓ 60 second timeout elapsed

// Actions on entry:
- Stop all burner activity
- Ready for next demand cycle
```

#### ANY → ERROR
```cpp
// Conditions (any of):
✗ Critical safety failure
✗ Emergency stop triggered
✗ Communication lost
✗ Sensor failure

// Actions on entry:
- Immediate burner shutdown
- Set BURNER_ERROR event bit
- Save emergency state to FRAM
- Require manual reset
```

### Anti-Flapping Protection

Prevents rapid cycling that damages equipment:

| Timer | Duration | Purpose |
|-------|----------|---------|
| Minimum ON time | 120 seconds | Prevent short cycles |
| Minimum OFF time | 20 seconds | Allow cooling |
| Power change delay | 30 seconds | Prevent power oscillation |

**Implementation**: `BurnerAntiFlapping.cpp`

```cpp
// Before turning on
if (!BurnerAntiFlapping::canTurnOn()) {
    LOG_DEBUG(TAG, "Anti-flap: Wait %lu ms",
             BurnerAntiFlapping::getTimeUntilCanTurnOn());
    return IDLE;  // Stay in IDLE
}

// Notify anti-flapping system
BurnerAntiFlapping::onBurnerTurnedOn();
```

### Safety Interlocks

Before transitioning to PRE_PURGE, all checks must pass:

```cpp
bool checkSafetyConditions() {
    ✓ Pump running (circulation verified)
    ✓ Temperature sensors valid (minimum 2 sensors)
    ✓ Temperature in range (< 85°C typical)
    ✓ No thermal shock (< 30°C differential)
    ✓ Pressure in range (1.0-3.5 BAR)
    ✓ No emergency stop
    ✓ Communication OK (Modbus healthy)
    ✓ No system errors
}
```

**File**: `SafetyInterlocks.cpp`

## Power Level State Machine

Nested within RUNNING states:

```
RUNNING_LOW ←──────────────┐
     │                      │ shouldDecreasePower()
     │ shouldIncreasePower()│ + canChangePowerLevel()
     ▼                      │
RUNNING_HIGH ──────────────┘
```

**Decision Logic** (`shouldIncreasePower()`, `shouldDecreasePower()`):
- Based on PID output
- Temperature error magnitude
- Current vs target temperature
- Mode (heating vs water)

## Relay State Management

**File**: `RYN4.cpp` (relay module)

```
Relay State: OFF ──┐
                   │ controlRelay(ON)
                   ▼
                  ON
                   │ controlRelay(OFF)
                   ▼
                  OFF
```

**Relay Functions**:
1. Burner Enable (R3)
2. Power Select (R4) - OFF=Full, ON=Half
3. Water Mode (R5)
4. Heating Pump (R1)
5. Water Pump (R2)

**State Tracking**: `SharedRelayReadings` updated atomically with mutex protection

## Pump Control State Machines

### Heating Pump Control

```
OFF ──────────┐
              │ Heating active
              ▼
     ┌───────────────┐
     │  WAIT_START   │ (delay for burner)
     └───────┬───────┘
             │ Burner confirmed
             ▼
            ON ◄─────── (continue while heating)
             │
             │ Heating stopped
             ▼
     ┌───────────────┐
     │  WAIT_STOP    │ (cooldown delay)
     └───────┬───────┘
             │ Cooldown complete
             ▼
            OFF
```

### Water Pump Control

```
OFF ─────────────┐
                 │ Water mode active
                 ▼
    ┌─────────────────┐
    │  WAIT_BURNER    │
    └────────┬────────┘
             │ Burner ready
             ▼
            ON ◄────── (continue while water active)
             │
             │ Water mode stopped
             ▼
            OFF (immediate - no cooldown for water pump)
```

## Temperature Control State Machine

Simplified view of PID control state:

```
DISABLED ────────────┐
                     │ Enable command
                     ▼
    ┌───────────────────────────┐
    │      MONITORING           │
    │                           │
    │  ┌─────────────────────┐ │
    │  │   BELOW_SETPOINT    │ │
    │  │   (request burner)  │ │
    │  └──────────┬──────────┘ │
    │             │ Temp rising │
    │             ▼             │
    │  ┌─────────────────────┐ │
    │  │    AT_SETPOINT      │ │
    │  │ (PID control active)│ │
    │  └──────────┬──────────┘ │
    │             │ Temp falling│
    │             ▼             │
    │  ┌─────────────────────┐ │
    │  │   ABOVE_SETPOINT    │ │
    │  │  (reduce/stop burner)│ │
    │  └─────────────────────┘ │
    │                           │
    └───────────────────────────┘
                     │ Disable command
                     ▼
                 DISABLED
```

## State Machine Framework

**Template**: `include/utils/StateMachine.h`

```cpp
template<typename StateEnum>
class StateMachine {
public:
    // Current state tracking
    StateEnum getCurrentState() const;
    uint32_t getTimeInState() const;  // Milliseconds

    // Transitions
    void transitionTo(StateEnum newState);
    bool isInState(StateEnum state) const;

    // History
    StateEnum getPreviousState() const;
    uint32_t getTransitionCount() const;
};
```

**Features**:
- Automatic timing (time in state)
- Transition logging
- Previous state tracking
- Thread-safe state queries

**Usage Pattern**:
```cpp
// In BurnerStateMachine.cpp
static StateMachine<BurnerSMState> stateMachine;

void update() {
    BurnerSMState currentState = stateMachine.getCurrentState();

    // Handle current state
    BurnerSMState nextState = handleCurrentState(currentState);

    // Transition if needed
    if (nextState != currentState) {
        stateMachine.transitionTo(nextState);
        onStateExit(currentState);
        onStateEnter(nextState);
    }
}
```

## State Persistence

Critical states are persisted to FRAM for crash recovery:

```cpp
struct EmergencyState {
    uint32_t magic = 0xDEADBEEF;
    BurnerSMState burnerState;
    Temperature_t lastBoilerTemp;
    Pressure_t lastPressure;
    bool wasHeating;
    bool wasWaterActive;
    uint32_t errorCode;
    uint32_t timestamp;
    uint32_t crc;
} __attribute__((packed));
```

**File**: `src/utils/CriticalDataStorage.h`

**On Emergency Stop**:
1. Save current state to FRAM
2. Save sensor readings
3. Save error code
4. Calculate CRC32

**On Startup**:
1. Check for valid emergency state (magic + CRC)
2. Log previous conditions
3. Decide if safe to auto-restart or require manual intervention

## State Transition Rules

### Timing-Based Transitions

States with automatic timeout transitions:

```cpp
// PRE_PURGE: Always transitions after 10s
if (timeInState >= PRE_PURGE_TIME_MS) {
    return IGNITION;
}

// IGNITION: Max 5s, retry or lockout
if (timeInState >= IGNITION_TIME_MS) {
    if (++retries >= MAX_RETRIES) return LOCKOUT;
    return PRE_PURGE;  // Retry
}

// POST_PURGE: Always transitions after 60s
if (timeInState >= POST_PURGE_TIME_MS) {
    return IDLE;
}

// LOCKOUT: Auto-reset after 5 minutes
if (timeInState >= LOCKOUT_TIME_MS) {
    return IDLE;
}
```

### Condition-Based Transitions

States that transition based on conditions:

```cpp
// IDLE: Transition when demand appears
if (heatDemand && safetyOK && antiFlappingOK) {
    return PRE_PURGE;
}

// RUNNING: Check continuously
if (!heatDemand || !safetyOK || !flameDetected) {
    return POST_PURGE;  // Shutdown sequence
}

// ERROR: Only clear when safe
if (safetyConditionsRestored()) {
    clearErrorBit();
    return IDLE;
}
```

### Power Level Transitions

Within RUNNING states, power can change:

```cpp
// RUNNING_LOW → RUNNING_HIGH
if (shouldIncreasePower() && canChangePowerLevel()) {
    return RUNNING_HIGH;
}

// RUNNING_HIGH → RUNNING_LOW
if (shouldDecreasePower() && canChangePowerLevel()) {
    return RUNNING_LOW;
}
```

**Power Decision Factors**:
- PID controller output
- Temperature error (target - actual)
- Mode (water heating typically needs more power)
- Anti-flapping timer (30s minimum between power changes)

## Safety State Transitions

### Emergency Transitions (Bypass Normal Flow)

Certain conditions trigger immediate state changes:

```cpp
// From ANY state → ERROR
if (criticalSafetyFailure) {
    emergencyStop();  // Immediate shutdown
    return ERROR;
}

// Examples of critical failures:
- Pressure < 0.5 BAR OR > 4.0 BAR
- Temperature > 95°C
- Flame loss during operation
- Communication timeout > 5 seconds
- Emergency stop button pressed
```

### Non-Blocking Safety

Some failures don't force ERROR state:

```cpp
// Pressure sensor missing → degraded mode (not ERROR)
if (!pressureSensorValid) {
    LOG_WARN(TAG, "Operating without pressure sensor");
    // Continue operation (degraded mode)
}

// Single temperature sensor failed → use redundant sensor
if (!sensor1Valid && sensor2Valid) {
    useSensor2();  // Automatic failover
}
```

## State Entry/Exit Actions

### onEnterPrePurge()
```cpp
- Ensure burner OFF
- Start exhaust fan (if equipped)
- Clear any stale error bits
```

### onEnterIgnition()
```cpp
- Enable ignition spark
- Control gas valve to low
- Start flame monitoring
- Increment burner start counter (RuntimeCounters)
```

### onEnterRunningHigh()
```cpp
- Set relays: Burner=ON, POWER_BOOST=ON (full power 42.2kW)
- Notify anti-flapping system (start ON timer)
- Update burner state event bits
- Log transition
```

### onEnterRunningLow()
```cpp
- Set relays: Burner=ON, POWER_BOOST=OFF (half power 23.3kW)
- Notify anti-flapping system
- Update burner state event bits
```

### onEnterPostPurge()
```cpp
- Close gas valve immediately
- Keep fan running for purge
- Notify anti-flapping (start OFF timer)
- Log shutdown reason
```

### onEnterLockout()
```cpp
- All relays OFF
- Set BURNER_ERROR event bit
- Log lockout reason and duration
- Require manual intervention or timeout
```

### onEnterError()
```cpp
- Immediate safe state (all OFF)
- Set ERROR event bits
- Save emergency state to FRAM
- Trigger alarm relay (if configured)
- Send MQTT alert
```

## State Monitoring

### Via MQTT
```json
{
  "burner": {
    "state": "RUNNING_HIGH",
    "timeInState": 1234,  // milliseconds
    "transitions": 42,     // total count
    "previousState": "IGNITION"
  }
}
```

**Topic**: `boiler/status/burner`

### Via Serial Logs
```
[BurnerSM] State transition: IDLE -> PRE_PURGE
[BurnerSM] State transition: PRE_PURGE -> IGNITION
[BurnerSM] State 1 completed after 10015 ms, transitioning to next state
[BurnerSM] Ignition successful - transitioning to high power
[BurnerSM] State transition: IGNITION -> RUNNING_HIGH
```

## State Machine Timing Constants

**From**: `SystemConstants::Burner`

```cpp
PRE_PURGE_TIME_MS = 10000      // 10 seconds (safety requirement)
IGNITION_TIME_MS = 5000        // 5 seconds max per attempt
POST_PURGE_TIME_MS = 60000     // 60 seconds (safety requirement)
LOCKOUT_TIME_MS = 300000       // 5 minutes (safety timeout)
MAX_IGNITION_RETRIES = 3       // Maximum attempts before lockout

MIN_ON_TIME_MS = 120000        // 2 minutes (anti-flapping)
MIN_OFF_TIME_MS = 20000        // 20 seconds (anti-flapping)
MIN_POWER_CHANGE_INTERVAL_MS = 30000  // 30 seconds (anti-flapping)
```

## Failure Modes & Recovery

### Ignition Failure
```
Attempt 1: IDLE → PRE_PURGE → IGNITION → (no flame) → PRE_PURGE
Attempt 2: PRE_PURGE → IGNITION → (no flame) → PRE_PURGE
Attempt 3: PRE_PURGE → IGNITION → (no flame) → LOCKOUT

After 5 minutes in LOCKOUT:
LOCKOUT → IDLE (auto-retry) OR manual reset
```

### Flame Loss During Operation
```
RUNNING_HIGH → POST_PURGE → IDLE
(No lockout - single failure)

Next demand → normal ignition sequence
If flame loss repeats → lockout after 3 attempts
```

### Safety Interlock Failure
```
RUNNING → ERROR (immediate)
ERROR → IDLE (only after safety restored + manual clear)
```

### Communication Loss
```
If Modbus timeout > 5 seconds:
  ANY_RUNNING_STATE → ERROR
  (Cannot safely operate without sensor feedback)
```

## Watchdog Integration

State machine is monitored by watchdog:

```cpp
// BurnerControlTask feeds watchdog every cycle
void taskFunction() {
    while (true) {
        BurnerStateMachine::update();
        Watchdog::feed();  // Must occur every 15 seconds
    }
}
```

**Watchdog Timeout**: 15 seconds
**Action on Timeout**: System reset

## Testing State Machines

### Manual State Testing
```cpp
// Force transition (for testing only)
#ifdef TESTING
    BurnerStateMachine::forceState(BurnerSMState::IGNITION);
#endif
```

### State History Logging
```cpp
// Get transition history
uint32_t transitions = stateMachine.getTransitionCount();
BurnerSMState previous = stateMachine.getPreviousState();
uint32_t timeInPrevious = stateMachine.getTimeInPreviousState();
```

### Simulation Mode
```cpp
// Simulate flame sensor (when no hardware)
#ifndef FLAME_SENSOR_INSTALLED
    bool isFlameDetected() {
        // Assume flame present if burner enabled
        return burnerRelaysAreOn();
    }
#endif
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
