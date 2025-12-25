// src/utils/StateMachine.h
#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

#include <functional>
#include <unordered_map>
#include "LoggingMacros.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config/SystemConstants.h"

/**
 * @brief Generic state machine template
 * 
 * @tparam StateEnum The enum type representing states
 * 
 * This template provides a reusable state machine implementation
 * for various system components. It supports:
 * - State transition callbacks
 * - Entry/exit actions for each state
 * - Timeout handling
 * - State history tracking
 */
template<typename StateEnum>
class StateMachine {
public:
    using StateHandler = std::function<StateEnum()>;
    using TransitionCallback = std::function<void(StateEnum from, StateEnum to)>;
    using ActionCallback = std::function<void()>;
    
    struct StateConfig {
        StateHandler handler;           // Main state logic
        ActionCallback onEntry;         // Called when entering state
        ActionCallback onExit;          // Called when exiting state
        uint32_t timeoutMs;            // State timeout (0 = no timeout)
        StateEnum timeoutNextState;     // State to transition on timeout
    };
    
private:
    StateEnum currentState;
    StateEnum previousState;
    std::unordered_map<StateEnum, StateConfig> stateConfigs;
    TransitionCallback transitionCallback;
    uint32_t stateEntryTime;
    const char* name;
    bool initialized;
    
public:
    StateMachine(const char* machineName, StateEnum initialState) 
        : currentState(initialState)
        , previousState(initialState)
        , transitionCallback(nullptr)
        , stateEntryTime(0)
        , name(machineName)
        , initialized(false) {
    }
    
    /**
     * @brief Register a state with its configuration
     */
    void registerState(StateEnum state, const StateConfig& config) {
        stateConfigs[state] = config;
    }
    
    /**
     * @brief Register a simple state with just a handler
     */
    void registerState(StateEnum state, StateHandler handler) {
        StateConfig config = {
            .handler = handler,
            .onEntry = nullptr,
            .onExit = nullptr,
            .timeoutMs = 0,
            .timeoutNextState = state
        };
        stateConfigs[state] = config;
    }
    
    /**
     * @brief Set callback for state transitions
     */
    void setTransitionCallback(TransitionCallback callback) {
        transitionCallback = callback;
    }
    
    /**
     * @brief Initialize the state machine
     */
    void initialize() {
        if (!initialized) {
            stateEntryTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
            
            // Call entry action for initial state
            auto it = stateConfigs.find(currentState);
            if (it != stateConfigs.end() && it->second.onEntry) {
                it->second.onEntry();
            }
            
            initialized = true;
            LOG_INFO(name, "State machine initialized in state %d", (int)currentState);
        }
    }
    
    /**
     * @brief Update the state machine (call periodically)
     */
    void update() {
        if (!initialized) {
            LOG_WARN(name, "State machine not initialized");
            return;
        }
        
        auto it = stateConfigs.find(currentState);
        if (it == stateConfigs.end()) {
            LOG_ERROR(name, "No handler for state %d", (int)currentState);
            return;
        }
        
        const StateConfig& config = it->second;
        
        // Check for timeout
        if (config.timeoutMs > 0) {
            uint32_t currentTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if ((currentTime - stateEntryTime) > config.timeoutMs) {
                LOG_INFO(name, "State %d completed after %lu ms, transitioning to next state", 
                         (int)currentState, currentTime - stateEntryTime);
                transitionTo(config.timeoutNextState);
                return;
            }
        }
        
        // Execute state handler
        if (config.handler) {
            StateEnum nextState = config.handler();
            if (nextState != currentState) {
                transitionTo(nextState);
            }
        }
    }
    
    /**
     * @brief Force transition to a new state
     */
    void transitionTo(StateEnum newState) {
        if (newState == currentState) {
            return;
        }
        
        LOG_INFO(name, "State transition: %d -> %d", (int)currentState, (int)newState);
        
        // Call exit action for current state
        auto currentIt = stateConfigs.find(currentState);
        if (currentIt != stateConfigs.end() && currentIt->second.onExit) {
            currentIt->second.onExit();
        }
        
        // Update states
        previousState = currentState;
        currentState = newState;
        stateEntryTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
        
        // Call transition callback
        if (transitionCallback) {
            transitionCallback(previousState, currentState);
        }
        
        // Call entry action for new state
        auto newIt = stateConfigs.find(currentState);
        if (newIt != stateConfigs.end() && newIt->second.onEntry) {
            newIt->second.onEntry();
        }
    }
    
    /**
     * @brief Get current state
     */
    StateEnum getCurrentState() const {
        return currentState;
    }
    
    /**
     * @brief Get previous state
     */
    StateEnum getPreviousState() const {
        return previousState;
    }
    
    /**
     * @brief Get time in current state (ms)
     */
    uint32_t getTimeInState() const {
        return (xTaskGetTickCount() * portTICK_PERIOD_MS) - stateEntryTime;
    }
    
    /**
     * @brief Check if in a specific state
     */
    bool isInState(StateEnum state) const {
        return currentState == state;
    }
    
    /**
     * @brief Reset to initial state
     */
    void reset(StateEnum initialState) {
        LOG_INFO(name, "Resetting state machine to state %d", (int)initialState);
        transitionTo(initialState);
    }
};

#endif // STATE_MACHINE_H