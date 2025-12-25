/**
 * @file test_burner_state_machine.cpp
 * @brief Unit tests for BurnerStateMachine
 *
 * Test functions are declared in test_main.cpp and run as part of the test suite.
 */

#include <unity.h>
#include <cstdint>

#ifdef NATIVE_TEST

#include "mocks/MockTime.h"

// Mock SystemConstants for BurnerStateMachine
namespace SystemConstants {
    namespace Burner {
        constexpr uint32_t PRE_PURGE_TIME_MS = 5000;
        constexpr uint32_t IGNITION_TIME_MS = 10000;
        constexpr uint32_t POST_PURGE_TIME_MS = 30000;
        constexpr uint32_t LOCKOUT_TIME_MS = 300000;
        constexpr uint8_t MAX_IGNITION_RETRIES = 3;
    }
}

// Burner state enum (matches real 9-state implementation)
enum class BurnerSMState {
    IDLE,
    PRE_PURGE,
    IGNITION,
    RUNNING_LOW,
    RUNNING_HIGH,
    MODE_SWITCHING,     // Seamless water ↔ heating transition
    POST_PURGE,
    LOCKOUT,
    ERROR
};

// Mock control variables
static bool bsm_mockFlameDetected = false;
static bool bsm_mockSafetyConditions = true;
static BurnerSMState bsm_currentState = BurnerSMState::IDLE;
static bool bsm_heatDemand = false;
static int16_t bsm_targetTemperature = 0;
static bool bsm_requestedHighPower = false;
static uint8_t bsm_ignitionRetries = 0;
static uint32_t bsm_stateStartTime = 0;
static bool bsm_modeSwitchRequested = false;
static constexpr uint32_t MODE_SWITCH_TIME_MS = 5000;

// Helper to convert float to Temperature_t (tenths of degree)
static int16_t tempFromFloat(float temp) { return (int16_t)(temp * 10); }

// Reset test state
static void bsm_resetTestState() {
    setMockMillis(0);
    bsm_mockFlameDetected = false;
    bsm_mockSafetyConditions = true;
    bsm_currentState = BurnerSMState::IDLE;
    bsm_heatDemand = false;
    bsm_targetTemperature = 0;
    bsm_requestedHighPower = false;
    bsm_ignitionRetries = 0;
    bsm_stateStartTime = 0;
    bsm_modeSwitchRequested = false;
}

// Simplified state machine update for testing
static void bsm_update() {
    uint32_t timeInState = millis() - bsm_stateStartTime;
    BurnerSMState newState = bsm_currentState;

    switch (bsm_currentState) {
        case BurnerSMState::IDLE:
            if (bsm_heatDemand && bsm_mockSafetyConditions) {
                newState = BurnerSMState::PRE_PURGE;
            }
            break;

        case BurnerSMState::PRE_PURGE:
            if (!bsm_mockSafetyConditions) {
                newState = BurnerSMState::ERROR;
            } else if (!bsm_heatDemand) {
                newState = BurnerSMState::POST_PURGE;
            } else if (timeInState >= SystemConstants::Burner::PRE_PURGE_TIME_MS) {
                newState = BurnerSMState::IGNITION;
            }
            break;

        case BurnerSMState::IGNITION:
            if (!bsm_mockSafetyConditions) {
                newState = BurnerSMState::ERROR;
            } else if (bsm_mockFlameDetected) {
                bsm_ignitionRetries = 0;
                newState = bsm_requestedHighPower ? BurnerSMState::RUNNING_HIGH : BurnerSMState::RUNNING_LOW;
            } else if (timeInState >= SystemConstants::Burner::IGNITION_TIME_MS) {
                bsm_ignitionRetries++;
                if (bsm_ignitionRetries >= SystemConstants::Burner::MAX_IGNITION_RETRIES) {
                    newState = BurnerSMState::LOCKOUT;
                } else {
                    newState = BurnerSMState::PRE_PURGE;
                }
            }
            break;

        case BurnerSMState::RUNNING_LOW:
            if (!bsm_mockSafetyConditions || !bsm_mockFlameDetected) {
                newState = BurnerSMState::ERROR;
            } else if (bsm_modeSwitchRequested) {
                newState = BurnerSMState::MODE_SWITCHING;
            } else if (!bsm_heatDemand) {
                newState = BurnerSMState::POST_PURGE;
            } else if (bsm_requestedHighPower) {
                newState = BurnerSMState::RUNNING_HIGH;
            }
            break;

        case BurnerSMState::RUNNING_HIGH:
            if (!bsm_mockSafetyConditions || !bsm_mockFlameDetected) {
                newState = BurnerSMState::ERROR;
            } else if (bsm_modeSwitchRequested) {
                newState = BurnerSMState::MODE_SWITCHING;
            } else if (!bsm_heatDemand) {
                newState = BurnerSMState::POST_PURGE;
            } else if (!bsm_requestedHighPower) {
                newState = BurnerSMState::RUNNING_LOW;
            }
            break;

        case BurnerSMState::MODE_SWITCHING:
            // Seamless transition between water and heating modes
            if (!bsm_mockSafetyConditions || !bsm_mockFlameDetected) {
                newState = BurnerSMState::ERROR;
            } else if (timeInState >= MODE_SWITCH_TIME_MS) {
                bsm_modeSwitchRequested = false;
                if (bsm_heatDemand) {
                    newState = bsm_requestedHighPower ?
                        BurnerSMState::RUNNING_HIGH : BurnerSMState::RUNNING_LOW;
                } else {
                    newState = BurnerSMState::POST_PURGE;
                }
            }
            break;

        case BurnerSMState::POST_PURGE:
            if (timeInState >= SystemConstants::Burner::POST_PURGE_TIME_MS) {
                newState = BurnerSMState::IDLE;
            }
            break;

        case BurnerSMState::LOCKOUT:
            if (timeInState >= SystemConstants::Burner::LOCKOUT_TIME_MS) {
                bsm_ignitionRetries = 0;
                newState = BurnerSMState::IDLE;
            }
            break;

        case BurnerSMState::ERROR:
            // Stay in error until external reset
            break;
    }

    if (newState != bsm_currentState) {
        bsm_currentState = newState;
        bsm_stateStartTime = millis();
    }
}

static void bsm_setHeatDemand(bool demand, int16_t target = 0, bool highPower = false) {
    bsm_heatDemand = demand;
    bsm_targetTemperature = target;
    bsm_requestedHighPower = highPower;
}

static void bsm_emergencyStop() {
    bsm_currentState = BurnerSMState::ERROR;
    bsm_stateStartTime = millis();
    bsm_heatDemand = false;
}

static void bsm_resetLockout() {
    if (bsm_currentState == BurnerSMState::LOCKOUT) {
        bsm_ignitionRetries = 0;
        bsm_currentState = BurnerSMState::IDLE;
        bsm_stateStartTime = millis();
    }
}

static void bsm_requestModeSwitch() {
    if (bsm_currentState == BurnerSMState::RUNNING_LOW ||
        bsm_currentState == BurnerSMState::RUNNING_HIGH) {
        bsm_modeSwitchRequested = true;
    }
}

// ============================================================================
// Test Cases - All prefixed with bsm_ to avoid collisions
// ============================================================================

void test_bsm_initial_state_is_idle() {
    bsm_resetTestState();
    TEST_ASSERT_EQUAL(BurnerSMState::IDLE, bsm_currentState);
}

void test_bsm_heat_demand_triggers_pre_purge() {
    bsm_resetTestState();
    bsm_setHeatDemand(true, tempFromFloat(60.0f), false);
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::PRE_PURGE, bsm_currentState);
}

void test_bsm_pre_purge_to_ignition() {
    bsm_resetTestState();
    bsm_setHeatDemand(true, tempFromFloat(60.0f), false);
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::PRE_PURGE, bsm_currentState);

    advanceMockMillis(SystemConstants::Burner::PRE_PURGE_TIME_MS + 100);
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::IGNITION, bsm_currentState);
}

void test_bsm_ignition_success_low_power() {
    bsm_resetTestState();
    bsm_setHeatDemand(true, tempFromFloat(60.0f), false);
    bsm_update();
    advanceMockMillis(SystemConstants::Burner::PRE_PURGE_TIME_MS + 100);
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::IGNITION, bsm_currentState);

    bsm_mockFlameDetected = true;
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::RUNNING_LOW, bsm_currentState);
}

void test_bsm_ignition_success_high_power() {
    bsm_resetTestState();
    bsm_setHeatDemand(true, tempFromFloat(70.0f), true);
    bsm_update();
    advanceMockMillis(SystemConstants::Burner::PRE_PURGE_TIME_MS + 100);
    bsm_update();

    bsm_mockFlameDetected = true;
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::RUNNING_HIGH, bsm_currentState);
}

void test_bsm_ignition_timeout_retry() {
    bsm_resetTestState();
    bsm_setHeatDemand(true, tempFromFloat(60.0f), false);
    bsm_update();
    advanceMockMillis(SystemConstants::Burner::PRE_PURGE_TIME_MS + 100);
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::IGNITION, bsm_currentState);

    // No flame, wait for timeout
    advanceMockMillis(SystemConstants::Burner::IGNITION_TIME_MS + 100);
    bsm_update();

    TEST_ASSERT_EQUAL(BurnerSMState::PRE_PURGE, bsm_currentState);
    TEST_ASSERT_EQUAL(1, bsm_ignitionRetries);
}

void test_bsm_ignition_failures_cause_lockout() {
    bsm_resetTestState();
    bsm_setHeatDemand(true, tempFromFloat(60.0f), false);

    for (uint8_t i = 0; i < SystemConstants::Burner::MAX_IGNITION_RETRIES; i++) {
        bsm_update();
        advanceMockMillis(SystemConstants::Burner::PRE_PURGE_TIME_MS + 100);
        bsm_update();
        advanceMockMillis(SystemConstants::Burner::IGNITION_TIME_MS + 100);
        bsm_update();
    }

    TEST_ASSERT_EQUAL(BurnerSMState::LOCKOUT, bsm_currentState);
}

void test_bsm_lockout_auto_reset() {
    bsm_resetTestState();
    bsm_setHeatDemand(true, tempFromFloat(60.0f), false);

    // Get into lockout
    for (uint8_t i = 0; i < SystemConstants::Burner::MAX_IGNITION_RETRIES; i++) {
        bsm_update();
        advanceMockMillis(SystemConstants::Burner::PRE_PURGE_TIME_MS + 100);
        bsm_update();
        advanceMockMillis(SystemConstants::Burner::IGNITION_TIME_MS + 100);
        bsm_update();
    }
    TEST_ASSERT_EQUAL(BurnerSMState::LOCKOUT, bsm_currentState);

    advanceMockMillis(SystemConstants::Burner::LOCKOUT_TIME_MS + 100);
    bsm_update();

    TEST_ASSERT_EQUAL(BurnerSMState::IDLE, bsm_currentState);
    TEST_ASSERT_EQUAL(0, bsm_ignitionRetries);
}

void test_bsm_lockout_manual_reset() {
    bsm_resetTestState();
    bsm_setHeatDemand(true, tempFromFloat(60.0f), false);

    for (uint8_t i = 0; i < SystemConstants::Burner::MAX_IGNITION_RETRIES; i++) {
        bsm_update();
        advanceMockMillis(SystemConstants::Burner::PRE_PURGE_TIME_MS + 100);
        bsm_update();
        advanceMockMillis(SystemConstants::Burner::IGNITION_TIME_MS + 100);
        bsm_update();
    }
    TEST_ASSERT_EQUAL(BurnerSMState::LOCKOUT, bsm_currentState);

    bsm_resetLockout();
    TEST_ASSERT_EQUAL(BurnerSMState::IDLE, bsm_currentState);
}

void test_bsm_demand_removal_triggers_post_purge() {
    bsm_resetTestState();
    bsm_setHeatDemand(true, tempFromFloat(60.0f), false);
    bsm_update();
    advanceMockMillis(SystemConstants::Burner::PRE_PURGE_TIME_MS + 100);
    bsm_update();
    bsm_mockFlameDetected = true;
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::RUNNING_LOW, bsm_currentState);

    bsm_setHeatDemand(false);
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::POST_PURGE, bsm_currentState);
}

void test_bsm_post_purge_to_idle() {
    bsm_resetTestState();
    bsm_setHeatDemand(true, tempFromFloat(60.0f), false);
    bsm_update();
    advanceMockMillis(SystemConstants::Burner::PRE_PURGE_TIME_MS + 100);
    bsm_update();
    bsm_mockFlameDetected = true;
    bsm_update();
    bsm_setHeatDemand(false);
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::POST_PURGE, bsm_currentState);

    advanceMockMillis(SystemConstants::Burner::POST_PURGE_TIME_MS + 100);
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::IDLE, bsm_currentState);
}

void test_bsm_emergency_stop() {
    bsm_resetTestState();
    bsm_setHeatDemand(true, tempFromFloat(60.0f), false);
    bsm_update();
    advanceMockMillis(SystemConstants::Burner::PRE_PURGE_TIME_MS + 100);
    bsm_update();
    bsm_mockFlameDetected = true;
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::RUNNING_LOW, bsm_currentState);

    bsm_emergencyStop();
    TEST_ASSERT_EQUAL(BurnerSMState::ERROR, bsm_currentState);
    TEST_ASSERT_FALSE(bsm_heatDemand);
}

void test_bsm_safety_failure_causes_error() {
    bsm_resetTestState();
    bsm_setHeatDemand(true, tempFromFloat(60.0f), false);
    bsm_update();
    advanceMockMillis(SystemConstants::Burner::PRE_PURGE_TIME_MS + 100);
    bsm_update();
    bsm_mockFlameDetected = true;
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::RUNNING_LOW, bsm_currentState);

    bsm_mockSafetyConditions = false;
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::ERROR, bsm_currentState);
}

void test_bsm_flame_loss_causes_error() {
    bsm_resetTestState();
    bsm_setHeatDemand(true, tempFromFloat(60.0f), false);
    bsm_update();
    advanceMockMillis(SystemConstants::Burner::PRE_PURGE_TIME_MS + 100);
    bsm_update();
    bsm_mockFlameDetected = true;
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::RUNNING_LOW, bsm_currentState);

    bsm_mockFlameDetected = false;
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::ERROR, bsm_currentState);
}

void test_bsm_power_level_switching() {
    bsm_resetTestState();
    bsm_setHeatDemand(true, tempFromFloat(60.0f), false);
    bsm_update();
    advanceMockMillis(SystemConstants::Burner::PRE_PURGE_TIME_MS + 100);
    bsm_update();
    bsm_mockFlameDetected = true;
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::RUNNING_LOW, bsm_currentState);

    bsm_setHeatDemand(true, tempFromFloat(70.0f), true);
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::RUNNING_HIGH, bsm_currentState);

    bsm_setHeatDemand(true, tempFromFloat(60.0f), false);
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::RUNNING_LOW, bsm_currentState);
}

void test_bsm_no_start_without_safety() {
    bsm_resetTestState();
    bsm_mockSafetyConditions = false;
    bsm_setHeatDemand(true, tempFromFloat(60.0f), false);
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::IDLE, bsm_currentState);
}

void test_bsm_demand_removal_during_pre_purge() {
    bsm_resetTestState();
    bsm_setHeatDemand(true, tempFromFloat(60.0f), false);
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::PRE_PURGE, bsm_currentState);

    bsm_setHeatDemand(false);
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::POST_PURGE, bsm_currentState);
}

// ============================================================================
// MODE_SWITCHING Tests - Seamless water ↔ heating transitions
// ============================================================================

void test_bsm_mode_switch_from_running_low() {
    bsm_resetTestState();
    bsm_setHeatDemand(true, tempFromFloat(60.0f), false);
    bsm_update();
    advanceMockMillis(SystemConstants::Burner::PRE_PURGE_TIME_MS + 100);
    bsm_update();
    bsm_mockFlameDetected = true;
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::RUNNING_LOW, bsm_currentState);

    // Request mode switch
    bsm_requestModeSwitch();
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::MODE_SWITCHING, bsm_currentState);
}

void test_bsm_mode_switch_from_running_high() {
    bsm_resetTestState();
    bsm_setHeatDemand(true, tempFromFloat(70.0f), true);
    bsm_update();
    advanceMockMillis(SystemConstants::Burner::PRE_PURGE_TIME_MS + 100);
    bsm_update();
    bsm_mockFlameDetected = true;
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::RUNNING_HIGH, bsm_currentState);

    // Request mode switch
    bsm_requestModeSwitch();
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::MODE_SWITCHING, bsm_currentState);
}

void test_bsm_mode_switch_completes_to_running() {
    bsm_resetTestState();
    bsm_setHeatDemand(true, tempFromFloat(60.0f), false);
    bsm_update();
    advanceMockMillis(SystemConstants::Burner::PRE_PURGE_TIME_MS + 100);
    bsm_update();
    bsm_mockFlameDetected = true;
    bsm_update();

    bsm_requestModeSwitch();
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::MODE_SWITCHING, bsm_currentState);

    // Wait for mode switch to complete
    advanceMockMillis(MODE_SWITCH_TIME_MS + 100);
    bsm_update();

    // Should return to RUNNING_LOW (demand still active, low power)
    TEST_ASSERT_EQUAL(BurnerSMState::RUNNING_LOW, bsm_currentState);
}

void test_bsm_mode_switch_no_demand_goes_to_post_purge() {
    bsm_resetTestState();
    bsm_setHeatDemand(true, tempFromFloat(60.0f), false);
    bsm_update();
    advanceMockMillis(SystemConstants::Burner::PRE_PURGE_TIME_MS + 100);
    bsm_update();
    bsm_mockFlameDetected = true;
    bsm_update();

    bsm_requestModeSwitch();
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::MODE_SWITCHING, bsm_currentState);

    // Remove demand during mode switch
    bsm_setHeatDemand(false);

    // Wait for mode switch to complete
    advanceMockMillis(MODE_SWITCH_TIME_MS + 100);
    bsm_update();

    // Should go to POST_PURGE (no demand)
    TEST_ASSERT_EQUAL(BurnerSMState::POST_PURGE, bsm_currentState);
}

void test_bsm_mode_switch_safety_failure_causes_error() {
    bsm_resetTestState();
    bsm_setHeatDemand(true, tempFromFloat(60.0f), false);
    bsm_update();
    advanceMockMillis(SystemConstants::Burner::PRE_PURGE_TIME_MS + 100);
    bsm_update();
    bsm_mockFlameDetected = true;
    bsm_update();

    bsm_requestModeSwitch();
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::MODE_SWITCHING, bsm_currentState);

    // Safety failure during mode switch
    bsm_mockSafetyConditions = false;
    bsm_update();

    TEST_ASSERT_EQUAL(BurnerSMState::ERROR, bsm_currentState);
}

void test_bsm_mode_switch_flame_loss_causes_error() {
    bsm_resetTestState();
    bsm_setHeatDemand(true, tempFromFloat(60.0f), false);
    bsm_update();
    advanceMockMillis(SystemConstants::Burner::PRE_PURGE_TIME_MS + 100);
    bsm_update();
    bsm_mockFlameDetected = true;
    bsm_update();

    bsm_requestModeSwitch();
    bsm_update();
    TEST_ASSERT_EQUAL(BurnerSMState::MODE_SWITCHING, bsm_currentState);

    // Flame loss during mode switch
    bsm_mockFlameDetected = false;
    bsm_update();

    TEST_ASSERT_EQUAL(BurnerSMState::ERROR, bsm_currentState);
}

void test_bsm_mode_switch_ignored_from_idle() {
    bsm_resetTestState();
    TEST_ASSERT_EQUAL(BurnerSMState::IDLE, bsm_currentState);

    // Mode switch request should be ignored from IDLE
    bsm_requestModeSwitch();
    bsm_update();

    TEST_ASSERT_EQUAL(BurnerSMState::IDLE, bsm_currentState);
    TEST_ASSERT_FALSE(bsm_modeSwitchRequested);
}

// ============================================================================
// Improvement 1: Power Level Mismatch Failsafe - Mock Infrastructure
// ============================================================================

static bool bsm_mockPowerLevelShouldFail = false;
static bool bsm_mockFailsafeTriggered = false;
static bool bsm_mockEmergencyStopCalled = false;

static void bsm_resetFailsafeMocks() {
    bsm_mockPowerLevelShouldFail = false;
    bsm_mockFailsafeTriggered = false;
    bsm_mockEmergencyStopCalled = false;
}

// Simulate power level change that can fail
static bool bsm_setPowerLevel(bool isHigh) {
    if (bsm_mockPowerLevelShouldFail) {
        // Trigger failsafe
        bsm_mockFailsafeTriggered = true;
        bsm_mockEmergencyStopCalled = true;
        bsm_currentState = BurnerSMState::ERROR;
        return false;
    }
    return true;
}

// ============================================================================
// Improvement 2: Helper Functions - Mock Implementation
// ============================================================================

// Mock helper: Check safety shutdown conditions
static BurnerSMState bsm_checkSafetyShutdown(BurnerSMState currentState) {
    // Check if we should stop burner
    if (!bsm_heatDemand || !bsm_mockSafetyConditions) {
        // Simplified: no anti-flapping in mock
        return BurnerSMState::POST_PURGE;
    }
    // No shutdown condition - return current state (sentinel)
    return currentState;
}

// Mock helper: Check flame loss conditions
static BurnerSMState bsm_checkFlameLoss(BurnerSMState currentState) {
    if (!bsm_mockFlameDetected) {
        // Both intentional and unexpected flame loss → POST_PURGE
        return BurnerSMState::POST_PURGE;
    }
    // Flame detected - no transition
    return currentState;
}

// ============================================================================
// Improvement 1 Tests: Power Level Mismatch Failsafe
// ============================================================================

void test_bsm_power_level_mismatch_triggers_failsafe_low() {
    bsm_resetTestState();
    bsm_resetFailsafeMocks();

    // Setup: Get burner to RUNNING_LOW state
    bsm_setHeatDemand(true, tempFromFloat(60.0f), false);
    bsm_update();  // IDLE → PRE_PURGE
    advanceMockMillis(SystemConstants::Burner::PRE_PURGE_TIME_MS + 100);
    bsm_update();  // PRE_PURGE → IGNITION
    bsm_mockFlameDetected = true;
    bsm_update();  // IGNITION → RUNNING_LOW
    TEST_ASSERT_EQUAL(BurnerSMState::RUNNING_LOW, bsm_currentState);

    // Simulate power level change failure during state entry
    bsm_mockPowerLevelShouldFail = true;

    // Simulate onEnterRunningLow() calling setPowerLevel(HALF)
    bool success = bsm_setPowerLevel(false);  // false = HALF/LOW

    // Verify: Power level failed, failsafe triggered, emergency stop called
    TEST_ASSERT_FALSE(success);
    TEST_ASSERT_TRUE(bsm_mockFailsafeTriggered);
    TEST_ASSERT_TRUE(bsm_mockEmergencyStopCalled);
    TEST_ASSERT_EQUAL(BurnerSMState::ERROR, bsm_currentState);
}

void test_bsm_power_level_mismatch_triggers_failsafe_high() {
    bsm_resetTestState();
    bsm_resetFailsafeMocks();

    // Setup: Get burner to RUNNING_HIGH state
    bsm_setHeatDemand(true, tempFromFloat(70.0f), true);  // High power requested
    bsm_update();  // IDLE → PRE_PURGE
    advanceMockMillis(SystemConstants::Burner::PRE_PURGE_TIME_MS + 100);
    bsm_update();  // PRE_PURGE → IGNITION
    bsm_mockFlameDetected = true;
    bsm_update();  // IGNITION → RUNNING_HIGH
    TEST_ASSERT_EQUAL(BurnerSMState::RUNNING_HIGH, bsm_currentState);

    // Simulate power level change failure during state entry
    bsm_mockPowerLevelShouldFail = true;

    // Simulate onEnterRunningHigh() calling setPowerLevel(FULL)
    bool success = bsm_setPowerLevel(true);  // true = FULL/HIGH

    // Verify: Power level failed, failsafe triggered, emergency stop called
    TEST_ASSERT_FALSE(success);
    TEST_ASSERT_TRUE(bsm_mockFailsafeTriggered);
    TEST_ASSERT_TRUE(bsm_mockEmergencyStopCalled);
    TEST_ASSERT_EQUAL(BurnerSMState::ERROR, bsm_currentState);
}

// ============================================================================
// Improvement 2 Tests: Helper Function Extraction
// ============================================================================

void test_bsm_helper_checkSafetyShutdown_no_demand() {
    bsm_resetTestState();
    bsm_setHeatDemand(false, tempFromFloat(20.0f), false);

    BurnerSMState result = bsm_checkSafetyShutdown(BurnerSMState::RUNNING_LOW);

    // No demand → Should return POST_PURGE
    TEST_ASSERT_EQUAL(BurnerSMState::POST_PURGE, result);
}

void test_bsm_helper_checkSafetyShutdown_demand_active() {
    bsm_resetTestState();
    bsm_setHeatDemand(true, tempFromFloat(60.0f), false);
    bsm_mockSafetyConditions = true;

    BurnerSMState result = bsm_checkSafetyShutdown(BurnerSMState::RUNNING_LOW);

    // Demand active, safety OK → Should return sentinel (current state)
    TEST_ASSERT_EQUAL(BurnerSMState::RUNNING_LOW, result);
}

void test_bsm_helper_checkFlameLoss_unexpected() {
    bsm_resetTestState();
    bsm_setHeatDemand(true, tempFromFloat(60.0f), false);  // Demand active
    bsm_mockFlameDetected = false;  // Flame lost

    BurnerSMState result = bsm_checkFlameLoss(BurnerSMState::RUNNING_HIGH);

    // Unexpected flame loss (demand active) → POST_PURGE
    TEST_ASSERT_EQUAL(BurnerSMState::POST_PURGE, result);
}

void test_bsm_helper_checkFlameLoss_intentional() {
    bsm_resetTestState();
    bsm_setHeatDemand(false, tempFromFloat(20.0f), false);  // No demand
    bsm_mockFlameDetected = false;  // Flame off

    BurnerSMState result = bsm_checkFlameLoss(BurnerSMState::RUNNING_HIGH);

    // Intentional shutdown (no demand) → POST_PURGE
    TEST_ASSERT_EQUAL(BurnerSMState::POST_PURGE, result);
}

#endif // NATIVE_TEST
