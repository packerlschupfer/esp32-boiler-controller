/**
 * @file MockRYN4.h
 * @brief Mock implementation of RYN4 8-channel relay controller for testing
 *
 * Updated to match real RYN4 module:
 * - 8 relay channels (0-7)
 * - DELAY command support for hardware watchdog
 * - Relay numbering starts at 0 (not 1)
 */

#ifndef MOCK_RYN4_H
#define MOCK_RYN4_H

#include <map>
#include <cstdint>
#include "IRelayController.h"
#include "MockTime.h"

class MockRYN4 : public IRelayController {
private:
    static constexpr int NUM_RELAYS = 8;
    std::map<int, bool> relayStates;
    std::map<int, uint32_t> lastSwitchTime;
    std::map<int, uint32_t> delayExpiry;  // DELAY command expiry timestamps
    static constexpr uint32_t MIN_SWITCH_INTERVAL_MS = 150;  // Matches real config
    bool connected;

public:
    MockRYN4() : connected(true) {
        // Initialize all 8 relays to OFF (0-indexed)
        for (int i = 0; i < NUM_RELAYS; i++) {
            relayStates[i] = false;
            lastSwitchTime[i] = 0;
            delayExpiry[i] = 0;
        }
    }

    bool setRelay(int relayNum, bool state) override {
        if (!connected || relayNum < 0 || relayNum >= NUM_RELAYS) {
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

#endif // MOCK_RYN4_H