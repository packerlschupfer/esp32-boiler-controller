/**
 * @file MockBurnerRequestManager.h
 * @brief Mock implementation of BurnerRequestManager for testing
 */

#ifndef MOCK_BURNER_REQUEST_MANAGER_H
#define MOCK_BURNER_REQUEST_MANAGER_H

#include "../../include/shared/Temperature.h"

class BurnerRequestManager {
public:
    enum class RequestSource {
        NONE,
        HEATING,
        WATER,
        MANUAL,
        EMERGENCY
    };
    
    struct BurnerRequest {
        RequestSource source;
        Temperature_t targetTemperature;
        uint8_t powerPercent;
    };
    
private:
    BurnerRequest heatingRequest_;
    BurnerRequest waterRequest_;
    bool waterPriority_;
    bool emergencyStop_;
    
public:
    BurnerRequestManager() 
        : heatingRequest_({RequestSource::NONE, 0, 0}),
          waterRequest_({RequestSource::NONE, 0, 0}),
          waterPriority_(true),  // Water priority enabled by default
          emergencyStop_(false) {}
    
    void requestHeating(Temperature_t targetTemp, uint8_t powerPercent) {
        heatingRequest_ = {RequestSource::HEATING, targetTemp, powerPercent};
    }
    
    void requestWater(Temperature_t targetTemp, uint8_t powerPercent) {
        waterRequest_ = {RequestSource::WATER, targetTemp, powerPercent};
    }
    
    void clearHeatingRequest() {
        heatingRequest_ = {RequestSource::NONE, 0, 0};
    }
    
    void clearWaterRequest() {
        waterRequest_ = {RequestSource::NONE, 0, 0};
    }
    
    void emergencyStop() {
        heatingRequest_ = {RequestSource::NONE, 0, 0};
        waterRequest_ = {RequestSource::NONE, 0, 0};
        emergencyStop_ = true;
    }
    
    BurnerRequest getCurrentRequest() const {
        // Emergency stop overrides everything
        if (emergencyStop_) {
            return {RequestSource::EMERGENCY, 0, 0};
        }
        
        // Check for water request first if water has priority
        if (waterPriority_ && waterRequest_.source == RequestSource::WATER) {
            return waterRequest_;
        }
        
        // Check for any water request
        if (waterRequest_.source == RequestSource::WATER) {
            return waterRequest_;
        }
        
        // Check for heating request
        if (heatingRequest_.source == RequestSource::HEATING) {
            return heatingRequest_;
        }
        
        // No active requests
        return {RequestSource::NONE, 0, 0};
    }
    
    void setWaterPriority(bool priority) {
        waterPriority_ = priority;
    }
    
    void clearEmergencyStop() {
        emergencyStop_ = false;
    }
};

#endif // MOCK_BURNER_REQUEST_MANAGER_H