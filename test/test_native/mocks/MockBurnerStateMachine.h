/**
 * @file MockBurnerStateMachine.h
 * @brief Mock implementation of BurnerStateMachine for testing
 */

#ifndef MOCK_BURNER_STATE_MACHINE_H
#define MOCK_BURNER_STATE_MACHINE_H

#include <cstdint>
#include "IRelayController.h"

class BurnerStateMachine {
public:
    enum class State {
        IDLE,
        PRE_IGNITION,
        IGNITION,
        RUNNING,
        POST_PURGE,
        ERROR,
        EMERGENCY_STOP
    };
    
    enum class PowerLevel {
        OFF,
        HALF,
        FULL
    };
    
    enum class ErrorType {
        NONE,
        IGNITION_FAILURE,
        FLAME_LOSS,
        OVERHEAT,
        SENSOR_FAILURE
    };
    
    struct Config {
        uint8_t ignitionRelay;
        uint8_t gasValveRelay;
        uint8_t fanRelay;
        uint8_t pumpRelay;
        uint32_t preIgnitionTime;
        uint32_t postPurgeTime;
        uint32_t ignitionTimeout;
        uint32_t flameStabilizationTime;
    };
    
private:
    Config config_;
    State currentState_;
    PowerLevel powerLevel_;
    ErrorType lastError_;
    uint32_t stateStartTime_;
    IRelayController* relayController_;
    
    void updateRelays() {
        if (!relayController_) return;
        
        switch (currentState_) {
            case State::IDLE:
                relayController_->setRelay(config_.ignitionRelay, false);
                relayController_->setRelay(config_.gasValveRelay, false);
                relayController_->setRelay(config_.fanRelay, false);
                relayController_->setRelay(config_.pumpRelay, false);
                break;
                
            case State::PRE_IGNITION:
                relayController_->setRelay(config_.ignitionRelay, false);
                relayController_->setRelay(config_.gasValveRelay, false);
                relayController_->setRelay(config_.fanRelay, true);
                relayController_->setRelay(config_.pumpRelay, powerLevel_ != PowerLevel::OFF);
                break;
                
            case State::IGNITION:
                relayController_->setRelay(config_.ignitionRelay, true);
                relayController_->setRelay(config_.gasValveRelay, true);
                relayController_->setRelay(config_.fanRelay, true);
                relayController_->setRelay(config_.pumpRelay, true);
                break;
                
            case State::RUNNING:
                relayController_->setRelay(config_.ignitionRelay, false);
                relayController_->setRelay(config_.gasValveRelay, true);
                relayController_->setRelay(config_.fanRelay, true);
                relayController_->setRelay(config_.pumpRelay, true);
                break;
                
            case State::POST_PURGE:
                relayController_->setRelay(config_.ignitionRelay, false);
                relayController_->setRelay(config_.gasValveRelay, false);
                relayController_->setRelay(config_.fanRelay, true);
                relayController_->setRelay(config_.pumpRelay, false);
                break;
                
            case State::ERROR:
            case State::EMERGENCY_STOP:
                relayController_->setRelay(config_.ignitionRelay, false);
                relayController_->setRelay(config_.gasValveRelay, false);
                relayController_->setRelay(config_.fanRelay, false);
                relayController_->setRelay(config_.pumpRelay, false);
                break;
        }
    }
    
public:
    BurnerStateMachine(const Config& config) 
        : config_(config), 
          currentState_(State::IDLE),
          powerLevel_(PowerLevel::OFF),
          lastError_(ErrorType::NONE),
          stateStartTime_(0),
          relayController_(nullptr) {}
    
    void setRelayController(IRelayController* controller) {
        relayController_ = controller;
    }
    
    void requestStart(PowerLevel level) {
        if (currentState_ == State::IDLE) {
            powerLevel_ = level;
            currentState_ = State::PRE_IGNITION;
            stateStartTime_ = millis();
            updateRelays();
        }
    }
    
    void requestStop() {
        if (currentState_ == State::RUNNING) {
            currentState_ = State::POST_PURGE;
            stateStartTime_ = millis();
            updateRelays();
        }
    }
    
    void emergencyStop() {
        currentState_ = State::EMERGENCY_STOP;
        powerLevel_ = PowerLevel::OFF;
        updateRelays();
    }
    
    void update() {
        uint32_t elapsed = millis() - stateStartTime_;
        State oldState = currentState_;
        
        switch (currentState_) {
            case State::PRE_IGNITION:
                if (elapsed > config_.preIgnitionTime) {
                    currentState_ = State::IGNITION;
                    stateStartTime_ = millis();
                }
                break;
                
            case State::IGNITION:
                if (elapsed > config_.ignitionTimeout) {
                    currentState_ = State::RUNNING;
                    stateStartTime_ = millis();
                }
                break;
                
            case State::POST_PURGE:
                if (elapsed > config_.postPurgeTime) {
                    currentState_ = State::IDLE;
                    powerLevel_ = PowerLevel::OFF;
                }
                break;
                
            default:
                break;
        }
        
        // Update relays if state changed
        if (oldState != currentState_) {
            updateRelays();
        }
    }
    
    State getCurrentState() const { return currentState_; }
    PowerLevel getPowerLevel() const { return powerLevel_; }
    
    void setPowerLevel(PowerLevel level) {
        if (currentState_ == State::RUNNING) {
            powerLevel_ = level;
        }
    }
    
    void reportError(ErrorType error) {
        lastError_ = error;
        currentState_ = State::ERROR;
        powerLevel_ = PowerLevel::OFF;
        updateRelays();
    }
};

// Global millis function declaration
extern uint32_t millis();

#endif // MOCK_BURNER_STATE_MACHINE_H