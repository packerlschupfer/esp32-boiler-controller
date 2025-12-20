/**
 * @file MockRYN4.h
 * @brief Mock implementation of RYN4 relay controller for testing
 */

#ifndef MOCK_RYN4_H
#define MOCK_RYN4_H

#include <map>
#include <cstdint>
#include "IRelayController.h"
#include "MockTime.h"

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

#endif // MOCK_RYN4_H