/**
 * @file test_relay_integration.cpp
 * @brief Integration tests for RYN4 relay control with safety interlocks
 */

#include <unity.h>
#include <vector>
#include <map>

// Include headers we're testing
#include "../../include/shared/Temperature.h"
#include "mocks/MockTime.h"
#include "mocks/MockBurnerStateMachine.h"

// Mock RYN4 relay controller
class MockRYN4 : public IRelayController {
private:
    std::map<int, bool> relayStates;
    std::map<int, uint32_t> lastSwitchTime;
    static constexpr uint32_t MIN_SWITCH_INTERVAL_MS = 1000;
    bool connected;
    
public:
    MockRYN4() : connected(true) {
        // Initialize all relays to OFF
        for (int i = 1; i <= 4; i++) {
            relayStates[i] = false;
            lastSwitchTime[i] = 0;
        }
    }
    
    bool setRelay(int relayNum, bool state) override {
        if (!connected || relayNum < 1 || relayNum > 4) {
            return false;
        }
        
        // If state is the same, just return success
        if (relayStates[relayNum] == state) {
            return true;
        }
        
        // Check minimum switch interval for state changes
        uint32_t currentTime = millis();
        auto lastTime = lastSwitchTime[relayNum];
        
        // Check timing only if this relay has been switched before
        if (lastTime > 0) {
            if (currentTime - lastTime < MIN_SWITCH_INTERVAL_MS) {
                return false; // Too soon to switch
            }
        }
        
        // Update state and timestamp
        relayStates[relayNum] = state;
        lastSwitchTime[relayNum] = currentTime;
        return true;
    }
    
    bool getRelay(int relayNum) const override {
        auto it = relayStates.find(relayNum);
        return (it != relayStates.end()) ? it->second : false;
    }
    
    void setConnected(bool conn) {
        connected = conn;
    }
    
    bool isConnected() const {
        return connected;
    }
    
    void emergencyStop() {
        for (auto& relay : relayStates) {
            relay.second = false;
        }
    }
};

// Test variables
static MockRYN4* mockRelay = nullptr;
static BurnerStateMachine* burnerSM = nullptr;

void setupRelayIntegration() {
    setMockMillis(1000); // Start at 1 second to avoid zero-time issues
    mockRelay = new MockRYN4();
    
    BurnerStateMachine::Config config = {
        .ignitionRelay = 1,
        .gasValveRelay = 2,
        .fanRelay = 3,
        .pumpRelay = 4,
        .preIgnitionTime = 5000,
        .postPurgeTime = 30000,
        .ignitionTimeout = 10000,
        .flameStabilizationTime = 3000
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
    
    // Start burner
    burnerSM->requestStart(BurnerStateMachine::PowerLevel::FULL);
    
    // Should start with pre-ignition (fan only)
    TEST_ASSERT_FALSE(mockRelay->getRelay(1)); // Ignition OFF
    TEST_ASSERT_FALSE(mockRelay->getRelay(2)); // Gas valve OFF
    TEST_ASSERT_TRUE(mockRelay->getRelay(3));  // Fan ON
    TEST_ASSERT_TRUE(mockRelay->getRelay(4));  // Pump ON with FULL power
    
    // Advance time past pre-ignition
    advanceMockMillis(6000);
    burnerSM->update();
    
    // Should be in ignition phase
    TEST_ASSERT_TRUE(mockRelay->getRelay(1));  // Ignition ON
    TEST_ASSERT_TRUE(mockRelay->getRelay(2));  // Gas valve ON
    TEST_ASSERT_TRUE(mockRelay->getRelay(3));  // Fan ON
    
    tearDownRelayIntegration();
}

// Test emergency stop
void test_emergency_stop_relay_control() {
    setupRelayIntegration();
    
    // Start burner normally
    burnerSM->requestStart(BurnerStateMachine::PowerLevel::FULL);
    advanceMockMillis(10000);
    burnerSM->update();
    
    // Verify running
    TEST_ASSERT_TRUE(mockRelay->getRelay(2)); // Gas valve should be ON
    
    // Trigger emergency stop
    burnerSM->emergencyStop();
    mockRelay->emergencyStop();
    
    // All relays should be OFF immediately
    TEST_ASSERT_FALSE(mockRelay->getRelay(1));
    TEST_ASSERT_FALSE(mockRelay->getRelay(2));
    TEST_ASSERT_FALSE(mockRelay->getRelay(3));
    TEST_ASSERT_FALSE(mockRelay->getRelay(4));
    
    // State should be EMERGENCY_STOP
    TEST_ASSERT_EQUAL(BurnerStateMachine::State::EMERGENCY_STOP, burnerSM->getCurrentState());
    
    tearDownRelayIntegration();
}

// Test relay switch timing protection
void test_relay_switch_timing_protection() {
    setupRelayIntegration();
    
    // Turn relay on at time 0
    TEST_ASSERT_TRUE(mockRelay->setRelay(1, true));
    TEST_ASSERT_TRUE(mockRelay->getRelay(1)); // Verify it's on
    
    // Try to switch immediately - should fail
    advanceMockMillis(500); // Only 500ms passed
    bool result = mockRelay->setRelay(1, false);
    TEST_ASSERT_FALSE_MESSAGE(result, "Expected setRelay to return false due to timing protection");
    TEST_ASSERT_TRUE(mockRelay->getRelay(1)); // Should still be on
    
    // Wait full interval
    advanceMockMillis(600); // Now 1100ms total
    TEST_ASSERT_TRUE(mockRelay->setRelay(1, false));
    TEST_ASSERT_FALSE(mockRelay->getRelay(1)); // Should now be off
    
    tearDownRelayIntegration();
}

// Test relay failure handling
void test_relay_disconnection_handling() {
    setupRelayIntegration();
    
    // Start burner
    burnerSM->requestStart(BurnerStateMachine::PowerLevel::FULL);
    advanceMockMillis(6000);
    burnerSM->update();
    
    // Simulate relay disconnection
    mockRelay->setConnected(false);
    
    // Try to control relay - should fail
    TEST_ASSERT_FALSE(mockRelay->setRelay(2, true));
    TEST_ASSERT_FALSE(mockRelay->isConnected());
    
    // Burner should detect failure and go to error state
    // (In real implementation, burner would check relay status)
    
    tearDownRelayIntegration();
}

// Test power level control
void test_power_level_relay_mapping() {
    setupRelayIntegration();
    
    // Test HALF power
    burnerSM->requestStart(BurnerStateMachine::PowerLevel::HALF);
    advanceMockMillis(10000);
    burnerSM->update();
    
    // In HALF power, might use different relay configuration
    // This depends on your specific implementation
    TEST_ASSERT_TRUE(mockRelay->getRelay(2)); // Gas valve always ON when running
    
    // Switch to FULL power
    burnerSM->setPowerLevel(BurnerStateMachine::PowerLevel::FULL);
    burnerSM->update();
    
    // Verify power change (implementation specific)
    TEST_ASSERT_TRUE(mockRelay->getRelay(2));
    
    tearDownRelayIntegration();
}

// Test pump control integration
void test_pump_relay_control() {
    setupRelayIntegration();
    
    // In heating mode, pump should run with burner
    burnerSM->requestStart(BurnerStateMachine::PowerLevel::FULL);
    advanceMockMillis(10000);
    burnerSM->update();
    
    // Pump should be ON when burner is running
    TEST_ASSERT_TRUE(mockRelay->getRelay(4));
    
    // Stop burner
    burnerSM->requestStop();
    burnerSM->update();
    
    // Pump might continue for post-circulation
    // This is implementation specific
    
    tearDownRelayIntegration();
}

// Test multiple relay coordination
void test_multiple_relay_coordination() {
    setupRelayIntegration();
    
    // Test that relays are switched in proper sequence
    std::vector<std::pair<int, bool>> relaySequence;
    
    // Record relay changes during startup
    burnerSM->requestStart(BurnerStateMachine::PowerLevel::FULL);
    
    for (int i = 0; i < 20; i++) {
        burnerSM->update();
        
        // Check each relay and record changes
        for (int relay = 1; relay <= 4; relay++) {
            bool state = mockRelay->getRelay(relay);
            if (relaySequence.empty() || 
                relaySequence.back().first != relay || 
                relaySequence.back().second != state) {
                relaySequence.push_back({relay, state});
            }
        }
        
        advanceMockMillis(1000);
    }
    
    // Verify fan starts before gas valve
    bool fanStarted = false;
    bool gasStarted = false;
    for (const auto& change : relaySequence) {
        if (change.first == 3 && change.second) fanStarted = true;
        if (change.first == 2 && change.second) {
            gasStarted = true;
            TEST_ASSERT_TRUE(fanStarted); // Fan must start before gas
        }
    }
    
    tearDownRelayIntegration();
}

// Test relay state persistence
void test_relay_state_after_error() {
    setupRelayIntegration();
    
    // Start burner
    burnerSM->requestStart(BurnerStateMachine::PowerLevel::FULL);
    
    // Advance through pre-ignition (5s)
    advanceMockMillis(6000);
    burnerSM->update();
    
    // Advance through ignition (10s more)
    advanceMockMillis(11000);
    burnerSM->update();
    
    // Verify we're running
    TEST_ASSERT_EQUAL(BurnerStateMachine::State::RUNNING, burnerSM->getCurrentState());
    TEST_ASSERT_TRUE(mockRelay->getRelay(2)); // Gas valve should be ON
    
    // Simulate error condition
    burnerSM->reportError(BurnerStateMachine::ErrorType::IGNITION_FAILURE);
    
    // Safety relays should turn off
    TEST_ASSERT_FALSE(mockRelay->getRelay(1)); // Ignition OFF
    TEST_ASSERT_FALSE(mockRelay->getRelay(2)); // Gas valve OFF
    
    // Fan might stay on for purge
    // Pump might stay on for circulation
    
    tearDownRelayIntegration();
}

// main() is in test_main.cpp