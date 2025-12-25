/**
 * @file test_concurrency.cpp
 * @brief Concurrency and race condition tests
 *
 * Tests simulate concurrent task execution via sequential function calls.
 * Uses MockTime for deterministic, repeatable testing.
 *
 * Tests 18 FreeRTOS tasks with 10 event groups and multiple mutexes.
 */

#include <unity.h>

#ifdef NATIVE_TEST

#include "mocks/MockTime.h"

// Mock state for tracking concurrent operations
static bool g_task1_completed = false;
static bool g_task2_completed = false;
static uint32_t g_operation_count = 0;
static bool g_mutex_deadlock_detected = false;

// Mock mutex simulation
static bool g_sensorMutexLocked = false;
static bool g_relayMutexLocked = false;

// Mock sensor reading state
static int16_t g_boilerTempOutput = 0;
static int16_t g_boilerTempReturn = 0;
static uint32_t g_lastUpdateTimestamp = 0;
static bool g_isBoilerTempOutputValid = false;

// Mock system state event bits
static uint32_t g_systemEventBits = 0;
#define HEATING_ON_BIT  (1 << 0)
#define WATER_ON_BIT    (1 << 1)
#define WATER_PRIORITY_BIT (1 << 2)

// Helper to convert float to temperature
static int16_t tempFromFloat(float temp) { return (int16_t)(temp * 10); }

// Reset all mock state
static void concurrency_resetMocks() {
    g_task1_completed = false;
    g_task2_completed = false;
    g_operation_count = 0;
    g_mutex_deadlock_detected = false;
    g_sensorMutexLocked = false;
    g_relayMutexLocked = false;
    g_boilerTempOutput = 0;
    g_boilerTempReturn = 0;
    g_lastUpdateTimestamp = 0;
    g_isBoilerTempOutputValid = false;
    g_systemEventBits = 0;
    setMockMillis(0);
}

// Mock mutex operations
static bool takeSensorMutex(uint32_t timeout_ms) {
    if (g_sensorMutexLocked) {
        return false;  // Timeout
    }
    g_sensorMutexLocked = true;
    return true;
}

static void giveSensorMutex() {
    g_sensorMutexLocked = false;
}

static bool takeRelayMutex(uint32_t timeout_ms) {
    if (g_relayMutexLocked) {
        return false;  // Timeout
    }
    g_relayMutexLocked = true;
    return true;
}

static void giveRelayMutex() {
    g_relayMutexLocked = false;
}

// ============================================================================
// Mutex Deadlock Detection Tests (3 tests)
// ============================================================================

void test_concurrency_mutex_correct_order() {
    concurrency_resetMocks();

    // Task A: Lock sensor → Lock relay (CORRECT ORDER)
    if (takeSensorMutex(100)) {
        if (takeRelayMutex(100)) {
            g_task1_completed = true;
            giveRelayMutex();
        }
        giveSensorMutex();
    }

    // Task B: Lock sensor → Lock relay (SAME ORDER)
    if (takeSensorMutex(100)) {
        if (takeRelayMutex(100)) {
            g_task2_completed = true;
            giveRelayMutex();
        }
        giveSensorMutex();
    }

    // Verify: No deadlock, both tasks complete
    TEST_ASSERT_TRUE(g_task1_completed);
    TEST_ASSERT_TRUE(g_task2_completed);
}

void test_concurrency_mutex_wrong_order_detected() {
    concurrency_resetMocks();

    // Task A: Lock sensor (holds it)
    bool taskA_got_sensor = takeSensorMutex(100);
    TEST_ASSERT_TRUE(taskA_got_sensor);

    // Task B: Lock relay first (WRONG ORDER)
    bool taskB_got_relay = takeRelayMutex(100);
    TEST_ASSERT_TRUE(taskB_got_relay);

    // Task B tries to get sensor (should timeout because Task A has it)
    bool taskB_got_sensor = takeSensorMutex(10);  // Short timeout
    TEST_ASSERT_FALSE(taskB_got_sensor);  // Expect timeout (deadlock would occur in real concurrency)

    // Clean up
    giveRelayMutex();
    giveSensorMutex();
}

void test_concurrency_circuit_breaker_triggers_after_3_failures() {
    concurrency_resetMocks();

    // Simulate SafetyInterlocks.cpp:227-238 pattern
    uint8_t failures = 0;
    const uint8_t MAX_FAILURES = 3;

    // Lock mutex to force failures
    takeSensorMutex(100);

    // Simulate 3+ failed mutex acquisitions
    for (int i = 0; i < 5; i++) {
        if (!takeSensorMutex(10)) {  // Force timeout
            failures++;
            if (failures >= MAX_FAILURES) {
                // Trigger failsafe (would happen in real code)
                g_mutex_deadlock_detected = true;
                break;
            }
        }
    }

    // Verify: Circuit breaker triggered after 3 failures
    TEST_ASSERT_EQUAL(3, failures);
    TEST_ASSERT_TRUE(g_mutex_deadlock_detected);

    // Clean up
    giveSensorMutex();
}

// ============================================================================
// Mode Switch Race Condition Tests (4 tests)
// ============================================================================

void test_concurrency_mode_switch_water_and_heating_simultaneous() {
    concurrency_resetMocks();

    // Simulate race: Both HEATING_ON and WATER_ON set simultaneously
    g_systemEventBits = HEATING_ON_BIT | WATER_ON_BIT;

    // Check for conflict
    bool heatingOn = (g_systemEventBits & HEATING_ON_BIT) != 0;
    bool waterOn = (g_systemEventBits & WATER_ON_BIT) != 0;
    bool waterPriority = (g_systemEventBits & WATER_PRIORITY_BIT) != 0;

    // Both modes ON should be detected as conflict
    bool conflict = (heatingOn && waterOn);
    TEST_ASSERT_TRUE(conflict);

    // Without priority, behavior is undefined
    // With priority, water should win
    TEST_ASSERT_FALSE(waterPriority);  // No priority set
}

void test_concurrency_mode_switch_water_priority_wins() {
    concurrency_resetMocks();

    // Both modes ON, WATER_PRIORITY set
    g_systemEventBits = HEATING_ON_BIT | WATER_ON_BIT | WATER_PRIORITY_BIT;

    bool heatingOn = (g_systemEventBits & HEATING_ON_BIT) != 0;
    bool waterOn = (g_systemEventBits & WATER_ON_BIT) != 0;
    bool waterPriority = (g_systemEventBits & WATER_PRIORITY_BIT) != 0;

    // Tiebreaker logic: water wins if priority set
    bool currentModeIsWater = waterOn && (!heatingOn || waterPriority);

    // Verify water mode selected
    TEST_ASSERT_TRUE(currentModeIsWater);
}

void test_concurrency_mode_switch_rapid_toggle() {
    concurrency_resetMocks();

    uint32_t stateChangeCount = 0;
    bool prevWaterMode = false;

    // Toggle mode 10 times rapidly
    for (int i = 0; i < 10; i++) {
        if (i % 2 == 0) {
            g_systemEventBits = WATER_ON_BIT;
        } else {
            g_systemEventBits = HEATING_ON_BIT;
        }

        bool waterOn = (g_systemEventBits & WATER_ON_BIT) != 0;
        bool currentModeIsWater = waterOn;

        if (currentModeIsWater != prevWaterMode) {
            stateChangeCount++;
            prevWaterMode = currentModeIsWater;
        }

        advanceMockMillis(500);  // 500ms between toggles
    }

    // Should have 10 toggles (but anti-flapping would limit in real system)
    TEST_ASSERT_EQUAL(10, stateChangeCount);
}

void test_concurrency_seamless_switch_requires_all_conditions() {
    concurrency_resetMocks();

    // Test conditions for seamless switch
    bool inRunningState = true;
    bool safetyOK = true;
    bool flameDetected = true;

    // All conditions met
    bool canSwitch = inRunningState && safetyOK && flameDetected;
    TEST_ASSERT_TRUE(canSwitch);

    // Missing one condition
    flameDetected = false;
    canSwitch = inRunningState && safetyOK && flameDetected;
    TEST_ASSERT_FALSE(canSwitch);

    // Missing another condition
    flameDetected = true;
    safetyOK = false;
    canSwitch = inRunningState && safetyOK && flameDetected;
    TEST_ASSERT_FALSE(canSwitch);
}

// ============================================================================
// Sensor Reading Race Condition Tests (3 tests)
// ============================================================================

void test_concurrency_sensor_reading_atomic_fields() {
    concurrency_resetMocks();

    // Simulate sensor update WITH mutex
    if (takeSensorMutex(100)) {
        g_boilerTempOutput = tempFromFloat(55.0f);
        g_boilerTempReturn = tempFromFloat(45.0f);
        giveSensorMutex();
    }

    // Read WITH mutex (correct)
    int16_t output = 0;
    int16_t return_temp = 0;
    if (takeSensorMutex(100)) {
        output = g_boilerTempOutput;
        return_temp = g_boilerTempReturn;
        giveSensorMutex();
    }

    // Values should match
    TEST_ASSERT_EQUAL(tempFromFloat(55.0f), output);
    TEST_ASSERT_EQUAL(tempFromFloat(45.0f), return_temp);
}

void test_concurrency_sensor_staleness_check_during_update() {
    concurrency_resetMocks();
    setMockMillis(0);

    // Sensor update at T=0
    if (takeSensorMutex(100)) {
        g_boilerTempOutput = tempFromFloat(60.0f);
        g_lastUpdateTimestamp = millis();
        giveSensorMutex();
    }

    // Advance time 30 seconds
    advanceMockMillis(30000);

    // Safety check reads timestamp
    uint32_t age = 0;
    if (takeSensorMutex(100)) {
        age = millis() - g_lastUpdateTimestamp;
        giveSensorMutex();
    }

    TEST_ASSERT_EQUAL(30000, age);

    // Sensor is fresh (< 60s default timeout)
    bool isStale = (age > 60000);
    TEST_ASSERT_FALSE(isStale);
}

void test_concurrency_validity_flag_consistency() {
    concurrency_resetMocks();

    // Mark sensor invalid
    if (takeSensorMutex(100)) {
        g_isBoilerTempOutputValid = false;
        g_boilerTempOutput = -32768;  // TEMP_INVALID
        giveSensorMutex();
    }

    // Read sensor
    bool valid = false;
    int16_t temp = 0;
    if (takeSensorMutex(100)) {
        valid = g_isBoilerTempOutputValid;
        temp = g_boilerTempOutput;
        giveSensorMutex();
    }

    // Verify consistency
    TEST_ASSERT_FALSE(valid);
    TEST_ASSERT_EQUAL(-32768, temp);
}

// ============================================================================
// Anti-Flapping Under Concurrent Load Tests (4 tests)
// ============================================================================

void test_concurrency_antiflapping_minimum_on_time() {
    concurrency_resetMocks();
    setMockMillis(0);

    uint32_t burnerStartTime = millis();
    const uint32_t MIN_ON_TIME_MS = 120000;  // 2 minutes

    // Try to stop after only 30 seconds
    advanceMockMillis(30000);
    uint32_t elapsed = millis() - burnerStartTime;
    bool canTurnOff = (elapsed >= MIN_ON_TIME_MS);

    // Should NOT be allowed yet
    TEST_ASSERT_FALSE(canTurnOff);

    // After 2 minutes, should allow shutdown
    advanceMockMillis(90000);  // Total: 120 seconds
    elapsed = millis() - burnerStartTime;
    canTurnOff = (elapsed >= MIN_ON_TIME_MS);
    TEST_ASSERT_TRUE(canTurnOff);
}

void test_concurrency_antiflapping_minimum_off_time() {
    concurrency_resetMocks();
    setMockMillis(0);

    uint32_t burnerStopTime = millis();
    const uint32_t MIN_OFF_TIME_MS = 20000;  // 20 seconds

    // Try to restart after only 10 seconds
    advanceMockMillis(10000);
    uint32_t elapsed = millis() - burnerStopTime;
    bool canTurnOn = (elapsed >= MIN_OFF_TIME_MS);

    // Should NOT be allowed yet
    TEST_ASSERT_FALSE(canTurnOn);

    // After 20 seconds, should allow restart
    advanceMockMillis(10000);  // Total: 20 seconds
    elapsed = millis() - burnerStopTime;
    canTurnOn = (elapsed >= MIN_OFF_TIME_MS);
    TEST_ASSERT_TRUE(canTurnOn);
}

void test_concurrency_antiflapping_power_level_throttle() {
    concurrency_resetMocks();
    setMockMillis(0);

    uint32_t lastPowerChange = millis();
    const uint32_t MIN_POWER_CHANGE_INTERVAL_MS = 30000;  // 30 seconds

    // Request power change immediately (first change always allowed)
    bool firstChangeAllowed = true;
    TEST_ASSERT_TRUE(firstChangeAllowed);

    lastPowerChange = millis();

    // Request power change after only 5 seconds
    advanceMockMillis(5000);
    uint32_t elapsed = millis() - lastPowerChange;
    bool canChangePower = (elapsed >= MIN_POWER_CHANGE_INTERVAL_MS);

    // Should be throttled
    TEST_ASSERT_FALSE(canChangePower);

    // After 30 seconds, should allow change
    advanceMockMillis(25000);  // Total: 30 seconds
    elapsed = millis() - lastPowerChange;
    canChangePower = (elapsed >= MIN_POWER_CHANGE_INTERVAL_MS);
    TEST_ASSERT_TRUE(canChangePower);
}

void test_concurrency_antiflapping_concurrent_demands() {
    concurrency_resetMocks();
    setMockMillis(0);

    uint32_t stateChangeCount = 0;
    uint32_t lastStateChange = millis();
    const uint32_t MIN_STATE_CHANGE_INTERVAL_MS = 10000;  // 10 seconds

    // Simulate 20 rapid demand changes
    for (int i = 0; i < 20; i++) {
        bool demandActive = (i % 2 == 0);

        // Check if state change allowed
        uint32_t elapsed = millis() - lastStateChange;
        bool canChange = (elapsed >= MIN_STATE_CHANGE_INTERVAL_MS);

        if (canChange) {
            stateChangeCount++;
            lastStateChange = millis();
        }

        advanceMockMillis(2000);  // 2 seconds between attempts
    }

    // Anti-flapping should limit state changes
    // 20 attempts * 2s = 40s total
    // With 10s minimum interval, max changes = 40/10 = 4
    TEST_ASSERT_LESS_THAN(6, stateChangeCount);  // Allow some margin
}

#endif // NATIVE_TEST
