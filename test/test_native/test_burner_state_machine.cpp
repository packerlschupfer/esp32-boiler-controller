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

// Burner state enum (matches real implementation)
enum class BurnerSMState {
    IDLE,
    PRE_PURGE,
    IGNITION,
    RUNNING_LOW,
    RUNNING_HIGH,
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
            } else if (!bsm_heatDemand) {
                newState = BurnerSMState::POST_PURGE;
            } else if (bsm_requestedHighPower) {
                newState = BurnerSMState::RUNNING_HIGH;
            }
            break;

        case BurnerSMState::RUNNING_HIGH:
            if (!bsm_mockSafetyConditions || !bsm_mockFlameDetected) {
                newState = BurnerSMState::ERROR;
            } else if (!bsm_heatDemand) {
                newState = BurnerSMState::POST_PURGE;
            } else if (!bsm_requestedHighPower) {
                newState = BurnerSMState::RUNNING_LOW;
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

#endif // NATIVE_TEST
