/**
 * @file MockMB8ART.h
 * @brief Mock implementation of MB8ART sensor for testing
 */

#ifndef MOCK_MB8ART_H
#define MOCK_MB8ART_H

#include "../../include/shared/Temperature.h"
#include <map>

class MockMB8ART {
private:
    Temperature_t channels[8];
    bool simulateTimeout;
    bool connected;
    std::map<int, bool> channelErrors;
    
public:
    MockMB8ART() : simulateTimeout(false), connected(true) {
        // Initialize with reasonable temperatures
        for (int i = 0; i < 8; i++) {
            channels[i] = tempFromFloat(20.0f + i * 5.0f);
            channelErrors[i] = false;
        }
    }
    
    void setChannelTemp(int channel, float temp) {
        if (channel >= 0 && channel < 8) {
            channels[channel] = tempFromFloat(temp);
        }
    }
    
    void setChannelValue(int channel, Temperature_t temp) {
        if (channel >= 0 && channel < 8) {
            channels[channel] = temp;
        }
    }
    
    void setSimulateTimeout(bool timeout) {
        simulateTimeout = timeout;
    }
    
    void setConnected(bool conn) {
        connected = conn;
    }
    
    void setChannelError(int channel, bool hasError) {
        if (channel >= 0 && channel < 8) {
            channelErrors[channel] = hasError;
        }
    }
    
    Temperature_t readChannel(int channel) {
        if (!connected || simulateTimeout || channel < 0 || channel >= 8) {
            return TEMP_INVALID;
        }
        
        // Check for channel-specific errors
        if (channelErrors[channel]) {
            return TEMP_INVALID;
        }
        
        return channels[channel];
    }
    
    bool isConnected() const {
        return connected && !simulateTimeout;
    }
    
    // Get all channel values for debugging
    void getAllChannels(Temperature_t* buffer) {
        for (int i = 0; i < 8; i++) {
            buffer[i] = readChannel(i);
        }
    }
};

#endif // MOCK_MB8ART_H