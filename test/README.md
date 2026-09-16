# ESPlan Boiler Controller Test Suite

This directory contains unit and integration tests for the ESPlan Boiler Controller project.

## Test Structure

```
test/
├── test_native/              # Tests that run on development machine
│   ├── test_main.cpp                        # Unity runner: declares and runs all tests
│   ├── test_temperature_conversion.cpp
│   ├── test_memory_pool.cpp
│   ├── test_burner_transitions.cpp          # BurnerTransitions::step() scenarios
│   ├── test_burner_transition_policy.cpp    # BurnerTransitionPolicy
│   ├── test_burner_demand_gate.cpp          # BurnerDemandGate
│   ├── test_stage_c_policies.cpp            # Power fault escalation, WaterChargePolicy
│   ├── test_relay_command_policy.cpp        # RelayCommandPolicy
│   ├── test_relay_extrema_tracker.cpp       # RelayExtrema::Tracker (autotune)
│   ├── test_pid_gain_fixed_point.cpp        # PIDGainFixedPoint
│   ├── test_pid_autotuner.cpp
│   ├── test_error_recovery_manager.cpp
│   ├── test_concurrency.cpp
│   ├── test_control_loop_integration.cpp
│   ├── test_mqtt_integration.cpp
│   ├── test_relay_integration.cpp
│   ├── test_sensor_integration.cpp
│   ├── test_persistent_storage_integration.cpp
│   ├── test_system_e2e.cpp
│   ├── test_burner_safety_rules.cpp # BurnerSafetyRules::evaluate() (validator checks)
│   ├── test_boiler_pid_control.cpp  # FixedPointPIDStep + BoilerPowerLevel (firmware PID)
│   └── mocks/                        # Mock implementations
│       ├── MockTime.h/cpp
│       ├── MockBurnerStateMachine.h
│       ├── MockBurnerRequestManager.h
│       ├── MockRYN4.h
│       ├── MockMB8ART.h
│       └── ...
├── test_embedded/            # Tests that run on ESP32 hardware
│   └── test_relay_control.cpp
└── README.md                 # This file
```

## Running Tests

### Prerequisites
- PlatformIO Core installed
- Unity test framework (automatically installed by PlatformIO)

### Run All Tests
```bash
# Run native tests only
python scripts/run_tests.py --native-only

# Run embedded tests (requires connected ESP32)
python scripts/run_tests.py --port /dev/ttyUSB0

# Run both native and embedded tests
python scripts/run_tests.py --port /dev/ttyUSB0
```

### Run Specific Tests with PlatformIO
```bash
# Run native tests
pio test -e native_test

# Run embedded tests
pio test -e esp32_test --upload-port /dev/ttyUSB0

# Run tests in verbose mode
pio test -e native_test -v
```

## Test Categories

### Native Tests (`test_native/`)
These tests run on your development machine and test pure logic without hardware dependencies.

All native tests are declared and run from `test_main.cpp` (233 `RUN_TEST` calls); the other files only define test functions, and `setUp()`/`tearDown()` live in `test_main.cpp`. A new test file needs its functions declared and a `RUN_TEST` line there.

`test_pid_autotuner.cpp` exercises a simplified local model (no firmware headers, only Unity, standard headers and `MockTime`), not the firmware class. The firmware autotune peak/trough detection is covered by `test_relay_extrema_tracker.cpp`, the firmware burner transition logic by `test_burner_transitions.cpp`.

#### Burner Transition Tests (`test_burner_transitions.cpp`)
Replay tick sequences through the firmware's header-only `BurnerTransitions::step()`. The simulator checks state timeouts before the step like `StateMachine::update()` (PRE_PURGE 2 s → IGNITION, IGNITION backstop 7 s → LOCKOUT).
- Start sequence, minimum off-time, stale demand never starts the burner, request withdrawn during pre-purge
- Demand end respects the minimum on-time; lost mode request stops after the grace period; flame loss bypasses the minimum on-time
- Heating disable stops during the minimum on-time; water disable does not stop heating
- Seamless HEATING → WATER, WATER → HEATING waiting for the heating request, bounded handover wait, revert with and without the ON bit, failed mode switch, mode change without flame, safety failure during mode switch
- Power level follows the request with anti-flapping
- Restart from POST_PURGE when demand returns; no restart without a mode request, of a disabled mode, or when safety fails
- Ignition failures retry, then lock out; a successful retry resets the counter

#### Burner Transition Policy Tests (`test_burner_transition_policy.cpp`)
- `stopForExplicitDisable()`: only the running mode or the boiler stops the burner
- `heatingLikelyWanted()`: enable/override, room mode, weather mode
- Bounded mode switch wait; mode revert requires the ON bit

#### Burner Demand Gate Tests (`test_burner_demand_gate.cpp`)
- A request with a hot boiler is not armed; a cold boiler arms immediately
- A fresh matching PID decision wins over the temperature; a decision for another target falls back to the temperature (handover)
- Without a boiler temperature BurnerControlTask arms
- BoilerTempControlTask arms a demand it did not see armed, drops a demand re-armed while coasting, never arms without permission, updates the power only on a PID change
- Sensor fallback target cap

#### Stage C Policy Tests (`test_stage_c_policies.cpp`)
- `BurnerTransitionPolicy::recordPowerFault()`: escalation on the third fault within the window, window restart after ten minutes
- `WaterChargePolicy::limitsValid()` (low below high) and `nextChargeNeeded()` (charge latch)

#### Relay Command Policy Tests (`test_relay_command_policy.cpp`)
- No-op commands skip rate limiting and pump protection; real changes are protected; emergency commands bypass protection
- Replay of a mode-switch relay batch followed by a power level change (counted once)

#### Relay Extrema Tracker Tests (`test_relay_extrema_tracker.cpp`)
- Lagging plant replay: peaks include the overshoot after switching OFF, troughs the undershoot after switching ON
- Cold-start (warm-up) phase ignored; recorded extreme times lie after the switch

#### PID Fixed-Point Gain Tests (`test_pid_gain_fixed_point.cpp`)
- `PIDGainFixedPoint::fromFloat()` scales by 1000 and rejects invalid gains
- Scaled gains command OFF above target and FULL below target
- `clampToAdjustment()` keeps the sign of large outputs

#### Other Unit Tests
- `test_error_recovery_manager.cpp`: recovery strategies, backoff, error history, escalation (simplified mock implementation)
- `test_concurrency.cpp`: race conditions simulated by sequential calls with `MockTime` (mutex order, circuit breaker, mode switch races, sensor atomicity, anti-flapping)
- `test_pid_autotuner.cpp`: circular buffer, relay control, peak detection, tuning method math (simplified model)

#### Temperature Conversion Tests
- Tests the Temperature_t fixed-point conversion functions
- Validates arithmetic operations on temperatures
- Tests formatting and validation functions

#### Memory Pool Tests
- Tests memory pool allocation/deallocation
- Validates pool statistics tracking
- Tests RAII wrapper functionality
- Stress tests allocation patterns

#### Burner Safety Rules Tests (`test_burner_safety_rules.cpp`)
Tests the firmware's Layer 1 pre-start checks (`include/modules/control/BurnerSafetyRules.h`, used by `BurnerSafetyValidator`): check order (emergency stop first), sensor ranges and count, stale data, inclusive boiler limit, water limit only in water mode, pressure bounds, missing pressure sensor, hardware interlock before thermal shock, thermal shock above 35 °C, result codes. `test_safety_cascade.cpp` was removed 2026-09-16: it tested a copy with different limits and order.

#### Boiler PID Tests (`test_boiler_pid_control.cpp`)
Tests the firmware PID step (`FixedPointPIDStep.h`) and power level mapping (`BoilerPowerLevel.h`) over multiple cycles: reference values, derivative after reset, anti-windup at the output and integral limits, OFF/HALF/FULL hysteresis, bang-bang bands.

### Embedded Tests (`test_embedded/`)
These tests run on actual ESP32 hardware to verify hardware-specific functionality.

#### Relay Control Tests
- Tests relay switching with timing constraints
- Validates switch interval protection
- Tests emergency shutdown functionality
- Verifies rate limiting

## Singleton Reset for Testing

Some singletons provide `resetForTesting()` methods (only compiled with `UNIT_TEST` defined):

### ✅ Resetable Singletons

| Singleton | Reset Method | Notes |
|-----------|--------------|-------|
| **HealthMonitor** | `HealthMonitor::resetForTesting()` | Resets all metrics, counters, and state |
| **SchedulerContext** | `SchedulerContext::resetForTesting()` | Calls cleanup(), clears schedules. Requires reinit after reset |
| **MQTTDiagnostics** | `MQTTDiagnostics::resetForTesting()` | Deletes instance. Requires reinit after reset |

### ❌ Non-Resetable Singletons (Use Mocks Instead)

| Singleton | Why Not Resetable |
|-----------|-------------------|
| **SharedResourceManager** | Manages FreeRTOS primitives (event groups, mutexes, queues) used throughout system. Deletion would be dangerous. |
| **ModbusCoordinator** | Manages active FreeRTOS timer and task notifications. Cannot safely stop/recreate. |
| **QueueManager** | Manages FreeRTOS queues with active messages and blocked tasks. Cannot safely delete. |

**Recommendation**: For singletons managing FreeRTOS resources, use **mocks** or **test doubles** instead of resetting:
```cpp
// Instead of resetting SharedResourceManager:
class MockSharedResourceManager : public IResourceManager {
    // Mock implementation...
};

// In test:
MockSharedResourceManager mockSRM;
// Inject mock into component under test
```

### Using resetForTesting()

```cpp
#include <unity.h>
#include "monitoring/HealthMonitor.h"

void setUp() {
    // Reset singleton state before each test
    #ifdef UNIT_TEST
    HealthMonitor::resetForTesting();
    #endif
}

void tearDown() {
    // Optional: reset after test
}

void test_health_monitor_initial_state() {
    auto& hm = HealthMonitor::getInstance();
    TEST_ASSERT_EQUAL(HealthMonitor::HealthStatus::UNKNOWN, hm.getOverallHealth());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_health_monitor_initial_state);
    return UNITY_END();
}
```

### Why Some Singletons Can't Be Reset

**FreeRTOS Resource Management**: Singletons that manage FreeRTOS primitives cannot be safely reset because:
1. Other components may hold handles to resources
2. Tasks may be blocked on mutexes/event groups/queues
3. Race conditions during deletion can cause crashes
4. Time-sensitive state (circuit breakers, tick counts) would be lost

**Best Practice**: Design tests to work with the real singletons OR use dependency injection with mocks.

---

## Writing New Tests

### Native Test Template
```cpp
#include <unity.h>

void setUp(void) {
    // Setup before each test
}

void tearDown(void) {
    // Cleanup after each test
}

void test_example() {
    TEST_ASSERT_EQUAL(expected, actual);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_example);
    return UNITY_END();
}
```

### Embedded Test Template
```cpp
#include <unity.h>
#include <Arduino.h>

void setUp(void) {
    // Hardware setup
}

void tearDown(void) {
    // Hardware cleanup
}

void test_hardware_function() {
    // Test hardware functionality
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    
    UNITY_BEGIN();
    RUN_TEST(test_hardware_function);
    UNITY_END();
}

void loop() {
    // Empty
}
```

## Test Reports

Test results are saved to `test_report.json` in the project root. The report includes:
- Test execution timestamps
- Individual test results
- Pass/fail statistics
- Execution times

## Continuous Integration

These tests can be integrated into CI/CD pipelines:
- Native tests can run on any CI platform
- Embedded tests require hardware-in-the-loop setup

## Known Issues

1. **Hardware Interlock Stub**: The `checkHardwareInterlocks()` function in BurnerSafetyValidator always returns true (no interlock inputs are wired; see the comment in `src/modules/control/BurnerSafetyValidator.cpp`).

2. **Flow Sensor Simulation**: Flow sensor functionality is simulated using temperature differential as documented in the tests.

## Testing Extracted Helper Modules (Round 21)

The Round 21 refactoring extracted helper classes from large files to improve maintainability. Each extracted module should ideally have unit tests:

### Recommended Unit Tests

#### BurnerStateMachine Helper Classes
Note: the mode switch, shutdown and flame loss decisions moved from `BurnerSafetyChecks` to `BurnerTransitions.h` and are covered by `test_burner_transitions.cpp`; the list below predates that move.
```cpp
// test/test_native/test_burner_safety_checks.cpp
- test_isFlameDetected_relay_on()
- test_isFlameDetected_relay_off()
- test_checkSafetyConditions_all_pass()
- test_checkSafetyConditions_temp_limit_violated()
- test_canSeamlesslySwitch_from_running_low()

// test/test_native/test_burner_power_controller.cpp
- test_shouldIncreasePower_below_80c()
- test_shouldIncreasePower_blocks_at_80c()
- test_shouldDecreasePower_logic()

// test/test_native/test_burner_runtime_tracker.cpp
- test_recordStartTime()
- test_updateRuntimeCounters_calculates_correctly()
- test_millis_wraparound_handling()
```

#### RelayControlTask Helper Classes
```cpp
// test/test_native/test_relay_verification_manager.cpp
- test_checkPumpProtection_minimum_off_time()
- test_getPumpProtectionTimeRemaining()
- test_checkRelayHealthAndEscalate_consecutive_failures()

// test/test_native/test_relay_command_processor.cpp
- test_processRelayRequests_heating_pump_on()
- test_processRelayRequests_water_pump_on()
- test_processRelayRequests_burner_enable()
```

### Testing Strategy for Helper Classes

**Advantages of Testing Extracted Modules:**
1. Faster test execution (no full system setup required)
2. Better isolation (test one responsibility at a time)
3. Easier to mock dependencies
4. More comprehensive edge case coverage

**Example Test Structure:**
```cpp
#include <unity.h>
#include "modules/control/BurnerSafetyChecks.h"
#include "mocks/MockSystemResourceProvider.h"

void setUp() {
    MockSystemResourceProvider::reset();
}

void test_80c_safety_limit_blocks_high_power() {
    // Arrange: Set boiler temp to 81°C
    MockSystemResourceProvider::setSensorReading(
        SensorType::BOILER_OUTPUT,
        Temperature_t(810)  // 81.0°C
    );

    // Act
    bool canIncrease = BurnerPowerController::shouldIncreasePower(false);

    // Assert
    TEST_ASSERT_FALSE(canIncrease);
}
```

## Future Improvements

1. Add more embedded tests for:
   - Modbus communication (hardware-in-loop)
   - Temperature sensor reading (actual MB8ART)
   - Network connectivity
   - MQTT messaging with real broker

2. Add integration tests for:
   - ✅ Safety cascade (5-layer architecture) - DONE
   - ✅ Mode switching (water ↔ heating) - DONE
   - ✅ Progressive preheating (thermal shock) - DONE
   - ✅ Circuit breaker pattern - DONE
   - Full system startup sequence
   - Fault recovery scenarios

3. **Add unit tests for Round 21 extracted modules:**
   - ⏳ BurnerSafetyChecks (safety validation logic)
   - ⏳ BurnerPowerController (80°C safety limit)
   - ⏳ BurnerRuntimeTracker (FRAM counter management)
   - ⏳ RelayVerificationManager (pump protection)
   - ⏳ RelayCommandProcessor (event processing)
   - ⏳ SafeLog utility (float logging safety)

4. Add performance benchmarks for:
   - Task execution times
   - Memory usage patterns
   - Communication latencies
   - Control loop response time