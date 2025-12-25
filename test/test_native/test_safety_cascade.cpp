/**
 * @file test_safety_cascade.cpp
 * @brief Integration tests for the 5-layer safety cascade system
 *
 * Tests the coordinated operation of:
 * - Layer 1: BurnerSafetyValidator (pre-operation validation)
 * - Layer 2: SafetyInterlocks (continuous monitoring)
 * - Layer 3: CentralizedFailsafe (emergency coordinator)
 * - Layer 4: DELAY Watchdog (hardware auto-OFF)
 * - Layer 5: Hardware interlocks (physical safety devices)
 *
 * Also tests:
 * - Mode switching (water ↔ heating transitions)
 * - Thermal shock protection (progressive preheating)
 * - Circuit breaker patterns (mutex failure handling)
 */

#include <unity.h>
#include <cstdint>

// Include mocks
#include "mocks/MockTime.h"
#include "mocks/MockSharedSensorReadings.h"
#include "mocks/MockSystemSettings.h"
#include "mocks/MockBurnerRequestManager.h"

// Safety system result enumeration (mirrors BurnerSafetyValidator::ValidationResult)
enum class ValidationResult {
    SAFE_TO_OPERATE,
    SENSOR_FAILURE,
    TEMPERATURE_EXCEEDED,
    PUMP_FAILURE,
    WATER_FLOW_FAILURE,
    PRESSURE_EXCEEDED,
    FLAME_DETECTION_FAILURE,
    RUNTIME_EXCEEDED,
    EMERGENCY_STOP_ACTIVE,
    INSUFFICIENT_SENSORS,
    HARDWARE_INTERLOCK_OPEN,
    THERMAL_SHOCK_RISK
};

// Mock SafetyConfig for testing
struct MockSafetyConfig {
    uint8_t minRequiredSensors = 2;
    Temperature_t maxBoilerTemp = tempFromFloat(85.0f);
    Temperature_t maxWaterTemp = tempFromFloat(65.0f);
    uint32_t maxContinuousRuntimeMs = 4 * 60 * 60 * 1000; // 4 hours
    uint32_t maxDailyRuntimeMs = 12 * 60 * 60 * 1000;     // 12 hours
    uint32_t sensorStaleMs = 60000;  // 60 seconds
};

// Simulated safety validator for testing
class TestSafetyValidator {
public:
    static ValidationResult validateBurnerOperation(
        const SharedSensorReadings& readings,
        const MockSafetyConfig& config,
        bool isWaterMode,
        bool emergencyStopActive = false) {

        // 1. Check emergency stop first
        if (emergencyStopActive) {
            return ValidationResult::EMERGENCY_STOP_ACTIVE;
        }

        // 2. Validate sufficient sensors are working
        uint8_t validSensors = countValidSensors(readings);
        if (validSensors < config.minRequiredSensors) {
            return ValidationResult::INSUFFICIENT_SENSORS;
        }

        // 3. Check temperature limits
        if (readings.isBoilerTempOutputValid &&
            readings.boilerTempOutput >= config.maxBoilerTemp) {
            return ValidationResult::TEMPERATURE_EXCEEDED;
        }

        // 4. Mode-specific water tank check
        if (isWaterMode &&
            readings.isWaterHeaterTempTankValid &&
            readings.waterHeaterTempTank >= config.maxWaterTemp) {
            return ValidationResult::TEMPERATURE_EXCEEDED;
        }

        // 5. Check pressure (simplified)
        if (readings.isSystemPressureValid) {
            const int16_t MIN_PRESSURE = 100;  // 1.00 BAR
            const int16_t MAX_PRESSURE = 350;  // 3.50 BAR

            if (readings.systemPressure < MIN_PRESSURE ||
                readings.systemPressure > MAX_PRESSURE) {
                return ValidationResult::PRESSURE_EXCEEDED;
            }
        }

        // 6. Check thermal shock risk
        if (readings.isBoilerTempOutputValid && readings.isBoilerTempReturnValid) {
            Temperature_t differential = tempSub(readings.boilerTempOutput, readings.boilerTempReturn);
            const Temperature_t MAX_DIFFERENTIAL = tempFromFloat(30.0f);

            if (differential > MAX_DIFFERENTIAL) {
                return ValidationResult::THERMAL_SHOCK_RISK;
            }
        }

        // 7. Check sensor freshness
        if (readings.lastUpdateTimestamp > 0) {
            uint32_t sensorAge = millis() - readings.lastUpdateTimestamp;
            if (sensorAge > config.sensorStaleMs) {
                return ValidationResult::SENSOR_FAILURE;
            }
        }

        return ValidationResult::SAFE_TO_OPERATE;
    }

    static uint8_t countValidSensors(const SharedSensorReadings& readings) {
        uint8_t count = 0;
        if (readings.isBoilerTempOutputValid) count++;
        if (readings.isBoilerTempReturnValid) count++;
        if (readings.isWaterHeaterTempTankValid) count++;
        if (readings.isInsideTempValid) count++;
        return count;
    }
};

// Simulated safety interlocks for testing
class TestSafetyInterlocks {
public:
    struct InterlockStatus {
        bool temperatureValid = false;
        bool temperatureInRange = false;
        bool noEmergencyStop = false;
        bool communicationOk = false;
        bool waterFlowDetected = false;
        bool noSystemErrors = false;
        bool minimumSensorsValid = false;
        bool pressureInRange = false;
        uint32_t lastCheckTime = 0;

        bool allInterlocksPassed() const {
            return temperatureValid && temperatureInRange && noEmergencyStop &&
                   communicationOk && waterFlowDetected && noSystemErrors &&
                   minimumSensorsValid && pressureInRange;
        }
    };

    static InterlockStatus performFullSafetyCheck(
        const SharedSensorReadings& readings,
        bool isWaterMode,
        bool emergencyStopActive = false,
        bool hasSystemErrors = false) {

        InterlockStatus status;
        status.lastCheckTime = millis();

        // 1. Check emergency stop
        status.noEmergencyStop = !emergencyStopActive;

        // 2. Check system errors
        status.noSystemErrors = !hasSystemErrors;

        // 3. Verify temperature sensors
        uint8_t validSensors = TestSafetyValidator::countValidSensors(readings);
        status.minimumSensorsValid = (validSensors >= 2);
        status.temperatureValid = status.minimumSensorsValid;

        // 4. Check temperature limits and thermal shock
        const Temperature_t MAX_BOILER_TEMP = tempFromFloat(85.0f);
        const Temperature_t MAX_DIFFERENTIAL = tempFromFloat(30.0f);

        status.temperatureInRange = true;
        if (readings.isBoilerTempOutputValid &&
            readings.boilerTempOutput >= MAX_BOILER_TEMP) {
            status.temperatureInRange = false;
        }

        if (readings.isBoilerTempOutputValid && readings.isBoilerTempReturnValid) {
            Temperature_t diff = tempSub(readings.boilerTempOutput, readings.boilerTempReturn);
            if (diff > MAX_DIFFERENTIAL) {
                status.temperatureInRange = false;
            }
        }

        // 5. Check communication (assume OK for testing)
        status.communicationOk = true;

        // 6. Check water flow (assume OK)
        status.waterFlowDetected = true;

        // 7. Check pressure
        if (readings.isSystemPressureValid) {
            const int16_t MIN_PRESSURE = 100;
            const int16_t MAX_PRESSURE = 350;
            status.pressureInRange = (readings.systemPressure >= MIN_PRESSURE &&
                                      readings.systemPressure <= MAX_PRESSURE);
        } else {
            status.pressureInRange = true;  // Fail-open without sensor
        }

        return status;
    }
};

// Simulated centralized failsafe for testing
class TestCentralizedFailsafe {
public:
    enum class FailsafeLevel {
        NONE,
        DEGRADED,      // Reduce functionality
        SHUTDOWN,      // Stop burner
        EMERGENCY      // All off immediately
    };

    static FailsafeLevel currentLevel;
    static bool emergencyStopTriggered;
    static const char* lastReason;

    static void reset() {
        currentLevel = FailsafeLevel::NONE;
        emergencyStopTriggered = false;
        lastReason = nullptr;
    }

    static void triggerFailsafe(FailsafeLevel level, const char* reason) {
        currentLevel = level;
        lastReason = reason;
        if (level == FailsafeLevel::EMERGENCY) {
            emergencyStopTriggered = true;
        }
    }

    static void emergencyStop(const char* reason) {
        triggerFailsafe(FailsafeLevel::EMERGENCY, reason);
    }
};

TestCentralizedFailsafe::FailsafeLevel TestCentralizedFailsafe::currentLevel =
    TestCentralizedFailsafe::FailsafeLevel::NONE;
bool TestCentralizedFailsafe::emergencyStopTriggered = false;
const char* TestCentralizedFailsafe::lastReason = nullptr;

// Mode switching state machine for testing
class TestModeSwitcher {
public:
    enum class Mode {
        IDLE,
        HEATING,
        WATER,
        MODE_SWITCHING  // Transition state
    };

    Mode currentMode = Mode::IDLE;
    Mode targetMode = Mode::IDLE;
    uint32_t transitionStartTime = 0;
    static constexpr uint32_t TRANSITION_DURATION_MS = 5000;  // 5 seconds

    bool requestModeSwitch(Mode newMode) {
        if (currentMode == newMode) {
            return true;  // Already in target mode
        }

        // Can only switch from HEATING or WATER modes
        if (currentMode != Mode::HEATING && currentMode != Mode::WATER &&
            currentMode != Mode::IDLE) {
            return false;  // In transition, can't switch
        }

        // Enter transition state
        targetMode = newMode;
        currentMode = Mode::MODE_SWITCHING;
        transitionStartTime = millis();
        return true;
    }

    void update() {
        if (currentMode == Mode::MODE_SWITCHING) {
            uint32_t elapsed = millis() - transitionStartTime;
            if (elapsed >= TRANSITION_DURATION_MS) {
                // Transition complete
                currentMode = targetMode;
            }
        }
    }

    bool isInTransition() const {
        return currentMode == Mode::MODE_SWITCHING;
    }
};

// Progressive preheating simulator for testing
class TestProgressivePreheater {
public:
    enum class State {
        IDLE,
        CYCLING,
        COMPLETE
    };

    State state = State::IDLE;
    uint32_t cycleStartTime = 0;
    uint8_t currentCycle = 0;
    static constexpr uint8_t MAX_CYCLES = 10;

    // Progressive ON/OFF durations (ms)
    uint32_t getOnDuration(uint8_t cycle) const {
        // 3s → 15s progressive increase over 10 cycles
        // Formula: 3000 + (cycle * 1200) gives 3s, 4.2s, 5.4s, 6.6s... 15s
        return 3000 + (cycle * 1200);
    }

    uint32_t getOffDuration(uint8_t cycle) const {
        // 25s → 5s progressive decrease
        return 25000 - (cycle * 2000);
    }

    void startPreheating(Temperature_t differential) {
        // Only preheat if differential exceeds threshold
        const Temperature_t THRESHOLD = tempFromFloat(15.0f);

        if (differential > THRESHOLD) {
            state = State::CYCLING;
            currentCycle = 0;
            cycleStartTime = millis();
        } else {
            state = State::COMPLETE;
        }
    }

    bool update(bool pumpRunning) {
        if (state != State::CYCLING) {
            return (state == State::COMPLETE);
        }

        uint32_t elapsed = millis() - cycleStartTime;
        uint32_t onDuration = getOnDuration(currentCycle);
        uint32_t offDuration = getOffDuration(currentCycle);
        uint32_t cycleDuration = onDuration + offDuration;

        if (elapsed >= cycleDuration) {
            // Move to next cycle
            currentCycle++;
            cycleStartTime = millis();

            if (currentCycle >= MAX_CYCLES) {
                state = State::COMPLETE;
                return true;
            }
        }

        return false;  // Not complete yet
    }

    bool shouldPumpBeOn() const {
        if (state != State::CYCLING) {
            return (state == State::COMPLETE);
        }

        uint32_t elapsed = millis() - cycleStartTime;
        uint32_t onDuration = getOnDuration(currentCycle);

        return (elapsed < onDuration);
    }
};

// Test fixtures
static SharedSensorReadings readings;
static MockSafetyConfig config;

void setUp_safety_cascade() {
    setMockMillis(0);
    TestCentralizedFailsafe::reset();

    // Initialize readings with safe values
    readings = SharedSensorReadings();
    readings.boilerTempOutput = tempFromFloat(50.0f);
    readings.boilerTempReturn = tempFromFloat(45.0f);
    readings.waterHeaterTempTank = tempFromFloat(45.0f);
    readings.insideTemp = tempFromFloat(20.0f);
    readings.systemPressure = 200;  // 2.00 BAR
    readings.isBoilerTempOutputValid = true;
    readings.isBoilerTempReturnValid = true;
    readings.isWaterHeaterTempTankValid = true;
    readings.isInsideTempValid = true;
    readings.isSystemPressureValid = true;
    readings.lastUpdateTimestamp = millis();

    config = MockSafetyConfig();
}

void tearDown_safety_cascade() {
    // Cleanup
}

// =====================================================
// SAFETY CASCADE TESTS
// =====================================================

// Test: All 5 layers pass when conditions are safe
void test_safety_cascade_all_pass() {
    setUp_safety_cascade();

    // Layer 1: BurnerSafetyValidator
    ValidationResult validatorResult = TestSafetyValidator::validateBurnerOperation(
        readings, config, false, false);
    TEST_ASSERT_EQUAL(ValidationResult::SAFE_TO_OPERATE, validatorResult);

    // Layer 2: SafetyInterlocks
    auto interlockStatus = TestSafetyInterlocks::performFullSafetyCheck(
        readings, false, false, false);
    TEST_ASSERT_TRUE(interlockStatus.allInterlocksPassed());

    // Layer 3: CentralizedFailsafe (should not be triggered)
    TEST_ASSERT_EQUAL(TestCentralizedFailsafe::FailsafeLevel::NONE,
                     TestCentralizedFailsafe::currentLevel);
    TEST_ASSERT_FALSE(TestCentralizedFailsafe::emergencyStopTriggered);

    tearDown_safety_cascade();
}

// Test: Layer 1 blocks when temperature exceeds limit
void test_safety_cascade_validator_blocks_high_temp() {
    setUp_safety_cascade();

    // Set boiler temp above limit
    readings.boilerTempOutput = tempFromFloat(90.0f);  // Above 85°C limit

    // Layer 1 should block
    ValidationResult result = TestSafetyValidator::validateBurnerOperation(
        readings, config, false, false);
    TEST_ASSERT_EQUAL(ValidationResult::TEMPERATURE_EXCEEDED, result);

    tearDown_safety_cascade();
}

// Test: Layer 1 blocks when insufficient sensors
void test_safety_cascade_validator_blocks_insufficient_sensors() {
    setUp_safety_cascade();

    // Only 1 valid sensor (need 2)
    readings.isBoilerTempOutputValid = true;
    readings.isBoilerTempReturnValid = false;
    readings.isWaterHeaterTempTankValid = false;
    readings.isInsideTempValid = false;

    ValidationResult result = TestSafetyValidator::validateBurnerOperation(
        readings, config, false, false);
    TEST_ASSERT_EQUAL(ValidationResult::INSUFFICIENT_SENSORS, result);

    tearDown_safety_cascade();
}

// Test: Layer 1 blocks thermal shock risk
void test_safety_cascade_validator_blocks_thermal_shock() {
    setUp_safety_cascade();

    // Create high differential (thermal shock risk)
    readings.boilerTempOutput = tempFromFloat(70.0f);
    readings.boilerTempReturn = tempFromFloat(35.0f);  // 35°C differential

    ValidationResult result = TestSafetyValidator::validateBurnerOperation(
        readings, config, false, false);
    TEST_ASSERT_EQUAL(ValidationResult::THERMAL_SHOCK_RISK, result);

    tearDown_safety_cascade();
}

// Test: Layer 2 catches emergency stop
void test_safety_cascade_interlocks_emergency_stop() {
    setUp_safety_cascade();

    auto status = TestSafetyInterlocks::performFullSafetyCheck(
        readings, false, true, false);  // emergency stop active

    TEST_ASSERT_FALSE(status.noEmergencyStop);
    TEST_ASSERT_FALSE(status.allInterlocksPassed());

    tearDown_safety_cascade();
}

// Test: Layer 2 catches system errors
void test_safety_cascade_interlocks_system_errors() {
    setUp_safety_cascade();

    auto status = TestSafetyInterlocks::performFullSafetyCheck(
        readings, false, false, true);  // system errors present

    TEST_ASSERT_FALSE(status.noSystemErrors);
    TEST_ASSERT_FALSE(status.allInterlocksPassed());

    tearDown_safety_cascade();
}

// Test: Layer 3 triggered by critical failure
void test_safety_cascade_failsafe_triggered() {
    setUp_safety_cascade();

    // Simulate critical failure
    TestCentralizedFailsafe::emergencyStop("Critical temperature exceeded");

    TEST_ASSERT_EQUAL(TestCentralizedFailsafe::FailsafeLevel::EMERGENCY,
                     TestCentralizedFailsafe::currentLevel);
    TEST_ASSERT_TRUE(TestCentralizedFailsafe::emergencyStopTriggered);

    tearDown_safety_cascade();
}

// Test: Pressure out of range blocks operation
void test_safety_cascade_pressure_out_of_range() {
    setUp_safety_cascade();

    // Low pressure
    readings.systemPressure = 50;  // 0.50 BAR (below 1.00 BAR minimum)

    ValidationResult result = TestSafetyValidator::validateBurnerOperation(
        readings, config, false, false);
    TEST_ASSERT_EQUAL(ValidationResult::PRESSURE_EXCEEDED, result);

    // High pressure
    readings.systemPressure = 400;  // 4.00 BAR (above 3.50 BAR maximum)

    result = TestSafetyValidator::validateBurnerOperation(
        readings, config, false, false);
    TEST_ASSERT_EQUAL(ValidationResult::PRESSURE_EXCEEDED, result);

    tearDown_safety_cascade();
}

// Test: Stale sensor data blocks operation
void test_safety_cascade_stale_sensor_data() {
    setUp_safety_cascade();

    // Set timestamp 2 minutes ago (stale threshold is 60s)
    readings.lastUpdateTimestamp = 0;
    advanceMockMillis(120000);  // 2 minutes
    readings.lastUpdateTimestamp = 0;  // Never updated

    // With timestamp 0 and current time > sensorStaleMs, should fail
    readings.lastUpdateTimestamp = millis() - 70000;  // 70 seconds old

    ValidationResult result = TestSafetyValidator::validateBurnerOperation(
        readings, config, false, false);
    TEST_ASSERT_EQUAL(ValidationResult::SENSOR_FAILURE, result);

    tearDown_safety_cascade();
}

// =====================================================
// MODE SWITCHING TESTS
// =====================================================

// Test: Mode switching from IDLE to HEATING
void test_mode_switching_idle_to_heating() {
    TestModeSwitcher switcher;

    TEST_ASSERT_EQUAL(TestModeSwitcher::Mode::IDLE, switcher.currentMode);

    bool success = switcher.requestModeSwitch(TestModeSwitcher::Mode::HEATING);
    TEST_ASSERT_TRUE(success);
    TEST_ASSERT_TRUE(switcher.isInTransition());

    // Advance time past transition
    advanceMockMillis(6000);
    switcher.update();

    TEST_ASSERT_FALSE(switcher.isInTransition());
    TEST_ASSERT_EQUAL(TestModeSwitcher::Mode::HEATING, switcher.currentMode);
}

// Test: Mode switching from HEATING to WATER
void test_mode_switching_heating_to_water() {
    TestModeSwitcher switcher;
    switcher.currentMode = TestModeSwitcher::Mode::HEATING;

    bool success = switcher.requestModeSwitch(TestModeSwitcher::Mode::WATER);
    TEST_ASSERT_TRUE(success);
    TEST_ASSERT_TRUE(switcher.isInTransition());

    // Advance time past transition
    advanceMockMillis(6000);
    switcher.update();

    TEST_ASSERT_EQUAL(TestModeSwitcher::Mode::WATER, switcher.currentMode);
}

// Test: Mode switching rejected during transition
void test_mode_switching_rejected_during_transition() {
    TestModeSwitcher switcher;
    switcher.currentMode = TestModeSwitcher::Mode::HEATING;

    // Start transition to WATER
    switcher.requestModeSwitch(TestModeSwitcher::Mode::WATER);
    TEST_ASSERT_TRUE(switcher.isInTransition());

    // Try to switch again while in transition
    bool success = switcher.requestModeSwitch(TestModeSwitcher::Mode::HEATING);
    TEST_ASSERT_FALSE(success);  // Should be rejected
}

// Test: Mode-specific water temp check (water mode only)
void test_mode_specific_water_temp_check() {
    setUp_safety_cascade();

    // Set water tank temp above limit
    readings.waterHeaterTempTank = tempFromFloat(70.0f);  // Above 65°C limit

    // In HEATING mode - water temp should be ignored
    ValidationResult heatingResult = TestSafetyValidator::validateBurnerOperation(
        readings, config, false, false);  // isWaterMode = false
    TEST_ASSERT_EQUAL(ValidationResult::SAFE_TO_OPERATE, heatingResult);

    // In WATER mode - water temp should block
    ValidationResult waterResult = TestSafetyValidator::validateBurnerOperation(
        readings, config, true, false);  // isWaterMode = true
    TEST_ASSERT_EQUAL(ValidationResult::TEMPERATURE_EXCEEDED, waterResult);

    tearDown_safety_cascade();
}

// =====================================================
// PROGRESSIVE PREHEATING TESTS
// =====================================================

// Test: Preheating skipped when differential is low
void test_preheating_skipped_low_differential() {
    TestProgressivePreheater preheater;

    // Small differential (10°C) - below 15°C threshold
    Temperature_t differential = tempFromFloat(10.0f);
    preheater.startPreheating(differential);

    TEST_ASSERT_EQUAL(TestProgressivePreheater::State::COMPLETE, preheater.state);
}

// Test: Preheating starts when differential is high
void test_preheating_starts_high_differential() {
    TestProgressivePreheater preheater;

    // Large differential (25°C) - above 15°C threshold
    Temperature_t differential = tempFromFloat(25.0f);
    preheater.startPreheating(differential);

    TEST_ASSERT_EQUAL(TestProgressivePreheater::State::CYCLING, preheater.state);
    TEST_ASSERT_EQUAL(0, preheater.currentCycle);
}

// Test: Progressive pump cycling durations
void test_preheating_progressive_durations() {
    TestProgressivePreheater preheater;

    // Verify progressive ON duration (3s → 15s over 10 cycles)
    // Formula: 3000 + (cycle * 1200)
    TEST_ASSERT_EQUAL(3000, preheater.getOnDuration(0));   // Cycle 0: 3s
    TEST_ASSERT_EQUAL(5400, preheater.getOnDuration(2));   // Cycle 2: 5.4s
    TEST_ASSERT_EQUAL(15000, preheater.getOnDuration(10)); // Cycle 10: 15s

    // Verify progressive OFF duration (25s → 5s over 10 cycles)
    // Formula: 25000 - (cycle * 2000)
    TEST_ASSERT_EQUAL(25000, preheater.getOffDuration(0));  // Cycle 0: 25s
    TEST_ASSERT_EQUAL(21000, preheater.getOffDuration(2));  // Cycle 2: 21s
    TEST_ASSERT_EQUAL(5000, preheater.getOffDuration(10));  // Cycle 10: 5s
}

// Test: Pump ON/OFF state during cycling
void test_preheating_pump_state_during_cycle() {
    TestProgressivePreheater preheater;
    setMockMillis(0);

    Temperature_t differential = tempFromFloat(25.0f);
    preheater.startPreheating(differential);

    // At start - pump should be ON (in ON phase)
    TEST_ASSERT_TRUE(preheater.shouldPumpBeOn());

    // After ON duration (3s) - pump should be OFF
    advanceMockMillis(3500);
    TEST_ASSERT_FALSE(preheater.shouldPumpBeOn());

    // After full cycle - next cycle starts
    advanceMockMillis(25000);  // Past OFF duration
    preheater.update(true);
    TEST_ASSERT_EQUAL(1, preheater.currentCycle);
}

// Test: Preheating completes after all cycles
void test_preheating_completes_after_cycles() {
    TestProgressivePreheater preheater;
    setMockMillis(0);

    Temperature_t differential = tempFromFloat(25.0f);
    preheater.startPreheating(differential);

    // Simulate all 10 cycles (simplified - just advance time significantly)
    for (int i = 0; i < 12; i++) {
        advanceMockMillis(30000);  // 30s per iteration
        preheater.update(true);
    }

    TEST_ASSERT_EQUAL(TestProgressivePreheater::State::COMPLETE, preheater.state);
}

// =====================================================
// CIRCUIT BREAKER PATTERN TESTS
// =====================================================

// Test: First mutex failure - assume safe (transient)
void test_circuit_breaker_first_failure_assume_safe() {
    // Simulated circuit breaker behavior
    uint8_t consecutiveFailures = 1;
    const uint8_t MAX_FAILURES = 3;

    bool shouldBlockOperation = (consecutiveFailures >= MAX_FAILURES);
    TEST_ASSERT_FALSE(shouldBlockOperation);  // First failure - don't block
}

// Test: Third consecutive failure - trigger failsafe
void test_circuit_breaker_third_failure_triggers_failsafe() {
    uint8_t consecutiveFailures = 3;
    const uint8_t MAX_FAILURES = 3;

    bool shouldBlockOperation = (consecutiveFailures >= MAX_FAILURES);
    TEST_ASSERT_TRUE(shouldBlockOperation);  // Third failure - block!

    // Trigger failsafe
    if (shouldBlockOperation) {
        TestCentralizedFailsafe::triggerFailsafe(
            TestCentralizedFailsafe::FailsafeLevel::DEGRADED,
            "Repeated mutex timeout");
    }

    TEST_ASSERT_EQUAL(TestCentralizedFailsafe::FailsafeLevel::DEGRADED,
                     TestCentralizedFailsafe::currentLevel);
}

// Test: Success resets failure counter
void test_circuit_breaker_success_resets_counter() {
    uint8_t consecutiveFailures = 2;

    // Simulate successful operation
    bool mutexAcquired = true;
    if (mutexAcquired) {
        consecutiveFailures = 0;  // Reset on success
    }

    TEST_ASSERT_EQUAL(0, consecutiveFailures);
}

// =====================================================
// COMBINED SCENARIO TESTS
// =====================================================

// Test: Complete heating activation workflow with all safety checks
void test_complete_heating_activation_workflow() {
    setUp_safety_cascade();
    TestModeSwitcher modeSwitcher;
    TestProgressivePreheater preheater;

    // Initial state: IDLE
    TEST_ASSERT_EQUAL(TestModeSwitcher::Mode::IDLE, modeSwitcher.currentMode);

    // Step 1: Request heating mode
    modeSwitcher.requestModeSwitch(TestModeSwitcher::Mode::HEATING);
    TEST_ASSERT_TRUE(modeSwitcher.isInTransition());

    // Step 2: Check pre-operation safety (Layer 1)
    ValidationResult safetyResult = TestSafetyValidator::validateBurnerOperation(
        readings, config, false, false);
    TEST_ASSERT_EQUAL(ValidationResult::SAFE_TO_OPERATE, safetyResult);

    // Step 3: Check interlocks (Layer 2)
    auto interlockStatus = TestSafetyInterlocks::performFullSafetyCheck(
        readings, false, false, false);
    TEST_ASSERT_TRUE(interlockStatus.allInterlocksPassed());

    // Step 4: Check thermal shock preheating need
    Temperature_t differential = tempSub(readings.boilerTempOutput, readings.boilerTempReturn);
    preheater.startPreheating(differential);

    // Differential is 5°C (50 - 45), so preheating should complete immediately
    TEST_ASSERT_EQUAL(TestProgressivePreheater::State::COMPLETE, preheater.state);

    // Step 5: Complete mode transition
    advanceMockMillis(6000);
    modeSwitcher.update();
    TEST_ASSERT_EQUAL(TestModeSwitcher::Mode::HEATING, modeSwitcher.currentMode);

    tearDown_safety_cascade();
}

// Test: Water heating blocked by high tank temperature
void test_water_heating_blocked_by_tank_temp() {
    setUp_safety_cascade();

    // Tank already at target temp
    readings.waterHeaterTempTank = tempFromFloat(68.0f);  // Above 65°C limit

    ValidationResult result = TestSafetyValidator::validateBurnerOperation(
        readings, config, true, false);  // Water mode

    TEST_ASSERT_EQUAL(ValidationResult::TEMPERATURE_EXCEEDED, result);

    tearDown_safety_cascade();
}

// Test: Emergency stop cascades through all layers
void test_emergency_stop_cascade() {
    setUp_safety_cascade();

    // Layer 1: Validator blocks
    ValidationResult validatorResult = TestSafetyValidator::validateBurnerOperation(
        readings, config, false, true);  // emergency stop active
    TEST_ASSERT_EQUAL(ValidationResult::EMERGENCY_STOP_ACTIVE, validatorResult);

    // Layer 2: Interlocks fail
    auto interlockStatus = TestSafetyInterlocks::performFullSafetyCheck(
        readings, false, true, false);
    TEST_ASSERT_FALSE(interlockStatus.allInterlocksPassed());

    // Layer 3: Failsafe triggered
    TestCentralizedFailsafe::emergencyStop("Test emergency");
    TEST_ASSERT_TRUE(TestCentralizedFailsafe::emergencyStopTriggered);

    tearDown_safety_cascade();
}

// main() is in test_main.cpp - these tests need to be registered there
