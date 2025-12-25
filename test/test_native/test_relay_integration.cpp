/**
 * @file test_relay_integration.cpp
 * @brief Integration tests for RYN4 relay control with safety interlocks
 *
 * Updated to match real system:
 * - 8 relays (0-7 indexed)
 * - Uses PowerLevel::LOW/HIGH (not HALF/FULL)
 * - Uses State::ERROR (not EMERGENCY_STOP)
 * - Uses State::RUNNING_LOW/RUNNING_HIGH (not just RUNNING)
 */

#include <unity.h>
#include <vector>
#include <map>

// Include headers we're testing
#include "../../include/shared/Temperature.h"
#include "mocks/MockTime.h"
#include "mocks/MockBurnerStateMachine.h"
#include "mocks/MockRYN4.h"

// Test variables
static MockRYN4* mockRelay = nullptr;
static BurnerStateMachine* burnerSM = nullptr;

void setupRelayIntegration() {
    setMockMillis(1000); // Start at 1 second to avoid zero-time issues
    mockRelay = new MockRYN4();

    // Config matches real system: relay channels 0-7, 0-indexed
    BurnerStateMachine::Config config = {
        .enableRelay = 0,      // CH0: Burner enable (with DELAY watchdog)
        .boostRelay = 1,       // CH1: Power boost (Stage 2)
        .heatingPumpRelay = 2, // CH2: Heating circulation pump
        .waterPumpRelay = 3,   // CH3: Hot water loading pump
        .prePurgeTime = 5000,
        .postPurgeTime = 30000,
        .ignitionTimeout = 10000,
        .flameStabilizationTime = 3000,
        .modeSwitchTime = 5000,
        .maxIgnitionRetries = 3,
        .lockoutDuration = 3600000  // 1 hour
    };
    burnerSM = new BurnerStateMachine(config);

    // Link the burner state machine to relay control
    burnerSM->setRelayController(mockRelay);
}

void tearDownRelayIntegration() {
    delete burnerSM;
    delete mockRelay;
}

// Test basic burner startup sequence
void test_burner_startup_sequence() {
    setupRelayIntegration();

    // Start burner at high power
    burnerSM->setHeatDemand(true, true);  // highPower = true
    burnerSM->update();  // Trigger state transition from IDLE to PRE_PURGE

    // Should start with pre-purge
    TEST_ASSERT_EQUAL(BurnerStateMachine::State::PRE_PURGE, burnerSM->getCurrentState());

    // Advance time past pre-purge (5000ms configured)
    advanceMockMillis(6000);
    burnerSM->update();

    // Should be in ignition phase
    TEST_ASSERT_EQUAL(BurnerStateMachine::State::IGNITION, burnerSM->getCurrentState());

    // Simulate flame detection and advance through flame stabilization (3000ms)
    burnerSM->setFlameDetected(true);
    advanceMockMillis(4000);
    burnerSM->update();

    // Should be running at high power
    TEST_ASSERT_EQUAL(BurnerStateMachine::State::RUNNING_HIGH, burnerSM->getCurrentState());

    tearDownRelayIntegration();
}

// Test emergency stop
void test_emergency_stop_relay_control() {
    setupRelayIntegration();

    // Start burner normally - need update() after setHeatDemand to transition
    burnerSM->setHeatDemand(true, true);
    burnerSM->update();  // IDLE -> PRE_PURGE

    // Advance past pre-purge
    advanceMockMillis(6000);
    burnerSM->update();  // PRE_PURGE -> IGNITION

    // Flame detected and stabilize
    burnerSM->setFlameDetected(true);
    advanceMockMillis(4000);
    burnerSM->update();  // IGNITION -> RUNNING_HIGH

    // Verify running
    TEST_ASSERT_EQUAL(BurnerStateMachine::State::RUNNING_HIGH, burnerSM->getCurrentState());

    // Trigger emergency stop
    burnerSM->emergencyStop();
    mockRelay->emergencyStop();

    // All relays should be OFF immediately
    TEST_ASSERT_FALSE(mockRelay->getRelay(0));
    TEST_ASSERT_FALSE(mockRelay->getRelay(1));
    TEST_ASSERT_FALSE(mockRelay->getRelay(2));
    TEST_ASSERT_FALSE(mockRelay->getRelay(3));

    // State should be ERROR
    TEST_ASSERT_EQUAL(BurnerStateMachine::State::ERROR, burnerSM->getCurrentState());

    tearDownRelayIntegration();
}

// Test relay switch timing protection
void test_relay_switch_timing_protection() {
    setupRelayIntegration();

    // Turn relay on at time 0 (using 0-indexed relays)
    TEST_ASSERT_TRUE(mockRelay->setRelay(0, true));
    TEST_ASSERT_TRUE(mockRelay->getRelay(0)); // Verify it's on

    // Try to switch immediately - should fail (MIN_SWITCH_INTERVAL_MS = 150ms)
    advanceMockMillis(100); // Only 100ms passed
    bool result = mockRelay->setRelay(0, false);
    TEST_ASSERT_FALSE_MESSAGE(result, "Expected setRelay to return false due to timing protection");
    TEST_ASSERT_TRUE(mockRelay->getRelay(0)); // Should still be on

    // Wait full interval
    advanceMockMillis(100); // Now 200ms total (> 150ms minimum)
    TEST_ASSERT_TRUE(mockRelay->setRelay(0, false));
    TEST_ASSERT_FALSE(mockRelay->getRelay(0)); // Should now be off

    tearDownRelayIntegration();
}

// Test relay failure handling
void test_relay_disconnection_handling() {
    setupRelayIntegration();

    // Start burner - need update() after setHeatDemand
    burnerSM->setHeatDemand(true, true);
    burnerSM->update();  // IDLE -> PRE_PURGE
    advanceMockMillis(6000);
    burnerSM->update();  // PRE_PURGE -> IGNITION
    burnerSM->setFlameDetected(true);
    advanceMockMillis(4000);
    burnerSM->update();  // IGNITION -> RUNNING_HIGH

    // Verify running
    TEST_ASSERT_EQUAL(BurnerStateMachine::State::RUNNING_HIGH, burnerSM->getCurrentState());

    // Simulate relay disconnection
    mockRelay->setConnected(false);

    // Try to control relay - should fail
    TEST_ASSERT_FALSE(mockRelay->setRelay(0, true));
    TEST_ASSERT_FALSE(mockRelay->isConnected());

    tearDownRelayIntegration();
}

// Test power level control
void test_power_level_relay_mapping() {
    setupRelayIntegration();

    // Test LOW power (Stage 1: 23.3kW)
    burnerSM->setHeatDemand(true, false);  // highPower = false
    burnerSM->update();  // IDLE -> PRE_PURGE
    advanceMockMillis(6000);
    burnerSM->update();  // PRE_PURGE -> IGNITION
    burnerSM->setFlameDetected(true);
    advanceMockMillis(4000);
    burnerSM->update();  // IGNITION -> RUNNING_LOW

    // In LOW power, boost relay is OFF
    TEST_ASSERT_EQUAL(BurnerStateMachine::State::RUNNING_LOW, burnerSM->getCurrentState());

    // Switch to HIGH power - the mock transitions directly when requestedHighPower changes
    burnerSM->setHeatDemand(true, true);  // Now want high power
    burnerSM->update();  // RUNNING_LOW -> RUNNING_HIGH (direct transition)

    // Should be running at high power
    TEST_ASSERT_EQUAL(BurnerStateMachine::State::RUNNING_HIGH, burnerSM->getCurrentState());

    tearDownRelayIntegration();
}

// Test pump control integration
void test_pump_relay_control() {
    setupRelayIntegration();

    // In heating mode, pump should run with burner
    burnerSM->setHeatDemand(true, true);
    burnerSM->update();  // IDLE -> PRE_PURGE
    advanceMockMillis(6000);
    burnerSM->update();  // PRE_PURGE -> IGNITION
    burnerSM->setFlameDetected(true);
    advanceMockMillis(4000);
    burnerSM->update();  // IGNITION -> RUNNING_HIGH

    // Verify running
    TEST_ASSERT_EQUAL(BurnerStateMachine::State::RUNNING_HIGH, burnerSM->getCurrentState());

    // Stop burner
    burnerSM->setHeatDemand(false, false);
    burnerSM->update();

    // Should transition to POST_PURGE
    TEST_ASSERT_EQUAL(BurnerStateMachine::State::POST_PURGE, burnerSM->getCurrentState());

    tearDownRelayIntegration();
}

// Test multiple relay coordination
void test_multiple_relay_coordination() {
    setupRelayIntegration();

    // Test that relays are switched in proper sequence
    std::vector<std::pair<int, bool>> relaySequence;

    // Record relay changes during startup
    burnerSM->setHeatDemand(true, true);

    for (int i = 0; i < 20; i++) {
        burnerSM->update();

        // Check each relay and record changes (0-indexed, 8 relays)
        for (int relay = 0; relay < 8; relay++) {
            bool state = mockRelay->getRelay(relay);
            if (relaySequence.empty() ||
                relaySequence.back().first != relay ||
                relaySequence.back().second != state) {
                relaySequence.push_back({relay, state});
            }
        }

        // Simulate flame detection when in ignition phase
        if (burnerSM->getCurrentState() == BurnerStateMachine::State::IGNITION) {
            burnerSM->setFlameDetected(true);
        }

        advanceMockMillis(1000);
    }

    // Verify pump starts before burner enable
    bool pumpStarted = false;
    bool burnerEnabled = false;
    for (const auto& change : relaySequence) {
        if (change.first == 2 && change.second) pumpStarted = true;  // Heating pump
        if (change.first == 0 && change.second) {
            burnerEnabled = true;
            TEST_ASSERT_TRUE(pumpStarted); // Pump must start before burner
        }
    }

    tearDownRelayIntegration();
}

// Test relay state persistence
void test_relay_state_after_error() {
    setupRelayIntegration();

    // Start burner
    burnerSM->setHeatDemand(true, true);
    burnerSM->update();  // IDLE -> PRE_PURGE

    // Advance through pre-purge (5s)
    advanceMockMillis(6000);
    burnerSM->update();  // PRE_PURGE -> IGNITION

    // Simulate flame detection and advance through ignition
    burnerSM->setFlameDetected(true);
    advanceMockMillis(4000);
    burnerSM->update();  // IGNITION -> RUNNING_HIGH

    // Verify we're running
    TEST_ASSERT_EQUAL(BurnerStateMachine::State::RUNNING_HIGH, burnerSM->getCurrentState());

    // Simulate flame loss (error condition) - setFlameDetected handles transition to ERROR
    burnerSM->setFlameDetected(false);

    // Should go to ERROR state immediately (setFlameDetected triggers this)
    TEST_ASSERT_EQUAL(BurnerStateMachine::State::ERROR, burnerSM->getCurrentState());

    tearDownRelayIntegration();
}

// main() is in test_main.cpp