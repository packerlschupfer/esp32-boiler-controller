# ESPlan Boiler Controller Test Suite

This directory contains unit and integration tests for the ESPlan Boiler Controller project.

## Test Structure

```
test/
├── test_native/              # Tests that run on development machine
│   ├── test_temperature_conversion.cpp
│   ├── test_burner_safety.cpp
│   ├── test_memory_pool.cpp
│   ├── test_burner_state_machine.cpp
│   ├── test_pid_autotuner.cpp
│   ├── test_error_recovery_manager.cpp
│   ├── test_control_loop_integration.cpp
│   ├── test_mqtt_integration.cpp
│   ├── test_relay_integration.cpp
│   ├── test_sensor_integration.cpp
│   ├── test_persistent_storage_integration.cpp
│   ├── test_system_e2e.cpp
│   ├── test_safety_cascade.cpp      # 5-layer safety integration
│   ├── test_main.cpp
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

#### Temperature Conversion Tests
- Tests the Temperature_t fixed-point conversion functions
- Validates arithmetic operations on temperatures
- Tests formatting and validation functions

#### Burner Safety Tests
- Tests safety validation logic
- Validates temperature limit checks
- Tests thermal shock protection
- Documents the hardware interlock stub issue

#### Memory Pool Tests
- Tests memory pool allocation/deallocation
- Validates pool statistics tracking
- Tests RAII wrapper functionality
- Stress tests allocation patterns

#### Safety Cascade Integration Tests (`test_safety_cascade.cpp`)
Tests the 5-layer safety architecture working together:
- **Layer cascade**: All 5 layers (Validator → Interlocks → Failsafe → DELAY → Hardware)
- **Temperature limits**: Boiler and water tank overtemperature blocking
- **Sensor validation**: Minimum sensor requirements, stale data detection
- **Thermal shock protection**: 30°C differential limit enforcement
- **Pressure monitoring**: Operating pressure range enforcement
- **Emergency stop cascade**: Multi-layer emergency response
- **Mode switching**: Water ↔ Heating transitions via MODE_SWITCHING state
- **Progressive preheating**: Thermal shock mitigation with pump cycling
- **Circuit breaker pattern**: Mutex failure handling (3 consecutive = failsafe)

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

1. **Hardware Interlock Stub**: The `checkHardwareInterlocks()` function in BurnerSafetyValidator always returns true. This is documented in the burner safety tests.

2. **Flow Sensor Simulation**: Flow sensor functionality is simulated using temperature differential as documented in the tests.

## Testing Extracted Helper Modules (Round 21)

The Round 21 refactoring extracted helper classes from large files to improve maintainability. Each extracted module should ideally have unit tests:

### Recommended Unit Tests

#### BurnerStateMachine Helper Classes
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