/**
 * @file MockBurnerStateMachine.h
 * @brief Mock implementation of BurnerStateMachine for testing
 *
 * Updated to match real 9-state FSM:
 * IDLE → PRE_PURGE → IGNITION → RUNNING_LOW/HIGH → POST_PURGE → IDLE
 *                                    ↓
 *                              MODE_SWITCHING (seamless water ↔ heating)
 *                                    ↓
 *                               LOCKOUT (safety) / ERROR
 */

#ifndef MOCK_BURNER_STATE_MACHINE_H
#define MOCK_BURNER_STATE_MACHINE_H

#include <cstdint>
#include "IRelayController.h"

/**
 * @brief Burner state machine states - matches real BurnerSMState enum
 */
enum class BurnerSMState {
    IDLE,               // Burner off, waiting for demand
    PRE_PURGE,          // Pre-purge sequence before ignition
    IGNITION,           // Ignition sequence
    RUNNING_LOW,        // Running at low power (Stage 1: 23.3kW)
    RUNNING_HIGH,       // Running at high power (Stage 2: 42.2kW)
    MODE_SWITCHING,     // Seamless mode transition (water ↔ heating)
    POST_PURGE,         // Post-purge after shutdown
    LOCKOUT,            // Safety lockout state (too many ignition failures)
    ERROR               // Error state
};

class BurnerStateMachine {
public:
    // Alias for backward compatibility with tests using State
    using State = BurnerSMState;

    enum class PowerLevel {
        OFF,
        LOW,    // Stage 1 (23.3kW) - renamed from HALF
        HIGH    // Stage 2 (42.2kW) - renamed from FULL
    };

    enum class ErrorType {
        NONE,
        IGNITION_FAILURE,
        FLAME_LOSS,
        OVERHEAT,
        SENSOR_FAILURE,
        SAFETY_INTERLOCK
    };

    struct Config {
        uint8_t enableRelay;            // Relay 0: Burner enable
        uint8_t boostRelay;             // Relay 1: Stage 2 power boost
        uint8_t heatingPumpRelay;       // Relay 2: Heating pump
        uint8_t waterPumpRelay;         // Relay 3: Water pump
        uint32_t prePurgeTime;          // Pre-purge duration (ms)
        uint32_t postPurgeTime;         // Post-purge duration (ms)
        uint32_t ignitionTimeout;       // Ignition timeout (ms)
        uint32_t flameStabilizationTime; // Time for flame to stabilize (ms)
        uint32_t modeSwitchTime;        // Mode switch transition time (ms)
        uint8_t maxIgnitionRetries;     // Max retries before lockout
        uint32_t lockoutDuration;       // Lockout duration (ms)
    };

private:
    Config config_;
    BurnerSMState currentState_;
    PowerLevel powerLevel_;
    ErrorType lastError_;
    uint32_t stateStartTime_;
    IRelayController* relayController_;

    // Tracking for lockout
    uint8_t ignitionRetries_;
    bool heatDemand_;
    bool requestedHighPower_;

    // Mode switching
    bool modeSwitchPending_;
    PowerLevel targetPowerLevel_;

    // Flame detection (for testing)
    bool flameDetected_;

    void updateRelays() {
        if (!relayController_) return;

        switch (currentState_) {
            case BurnerSMState::IDLE:
                relayController_->setRelay(config_.enableRelay, false);
                relayController_->setRelay(config_.boostRelay, false);
                relayController_->setRelay(config_.heatingPumpRelay, false);
                relayController_->setRelay(config_.waterPumpRelay, false);
                break;

            case BurnerSMState::PRE_PURGE:
                // Fan on, pump on, burner off
                relayController_->setRelay(config_.enableRelay, false);
                relayController_->setRelay(config_.boostRelay, false);
                relayController_->setRelay(config_.heatingPumpRelay, true);
                relayController_->setRelay(config_.waterPumpRelay, false);
                break;

            case BurnerSMState::IGNITION:
                // Burner enable + ignition
                relayController_->setRelay(config_.enableRelay, true);
                relayController_->setRelay(config_.boostRelay, false);  // Start low
                relayController_->setRelay(config_.heatingPumpRelay, true);
                relayController_->setRelay(config_.waterPumpRelay, false);
                break;

            case BurnerSMState::RUNNING_LOW:
                relayController_->setRelay(config_.enableRelay, true);
                relayController_->setRelay(config_.boostRelay, false);  // Low power
                relayController_->setRelay(config_.heatingPumpRelay, true);
                relayController_->setRelay(config_.waterPumpRelay, false);
                break;

            case BurnerSMState::RUNNING_HIGH:
                relayController_->setRelay(config_.enableRelay, true);
                relayController_->setRelay(config_.boostRelay, true);   // High power
                relayController_->setRelay(config_.heatingPumpRelay, true);
                relayController_->setRelay(config_.waterPumpRelay, false);
                break;

            case BurnerSMState::MODE_SWITCHING:
                // Keep burner running during seamless transition
                relayController_->setRelay(config_.enableRelay, true);
                relayController_->setRelay(config_.boostRelay,
                    powerLevel_ == PowerLevel::HIGH);
                // Pumps controlled by mode switching logic
                break;

            case BurnerSMState::POST_PURGE:
                // Burner off, pump still running for heat dissipation
                relayController_->setRelay(config_.enableRelay, false);
                relayController_->setRelay(config_.boostRelay, false);
                relayController_->setRelay(config_.heatingPumpRelay, true);
                relayController_->setRelay(config_.waterPumpRelay, false);
                break;

            case BurnerSMState::LOCKOUT:
            case BurnerSMState::ERROR:
                // All off - safety shutdown
                relayController_->setRelay(config_.enableRelay, false);
                relayController_->setRelay(config_.boostRelay, false);
                relayController_->setRelay(config_.heatingPumpRelay, false);
                relayController_->setRelay(config_.waterPumpRelay, false);
                break;
        }
    }

public:
    BurnerStateMachine()
        : currentState_(BurnerSMState::IDLE),
          powerLevel_(PowerLevel::OFF),
          lastError_(ErrorType::NONE),
          stateStartTime_(0),
          relayController_(nullptr),
          ignitionRetries_(0),
          heatDemand_(false),
          requestedHighPower_(false),
          modeSwitchPending_(false),
          targetPowerLevel_(PowerLevel::OFF),
          flameDetected_(false) {

        // Default config
        config_.enableRelay = 0;
        config_.boostRelay = 1;
        config_.heatingPumpRelay = 2;
        config_.waterPumpRelay = 3;
        config_.prePurgeTime = 2000;
        config_.postPurgeTime = 60000;
        config_.ignitionTimeout = 5000;
        config_.flameStabilizationTime = 3000;
        config_.modeSwitchTime = 5000;
        config_.maxIgnitionRetries = 3;
        config_.lockoutDuration = 300000;  // 5 minutes
    }

    BurnerStateMachine(const Config& config)
        : config_(config),
          currentState_(BurnerSMState::IDLE),
          powerLevel_(PowerLevel::OFF),
          lastError_(ErrorType::NONE),
          stateStartTime_(0),
          relayController_(nullptr),
          ignitionRetries_(0),
          heatDemand_(false),
          requestedHighPower_(false),
          modeSwitchPending_(false),
          targetPowerLevel_(PowerLevel::OFF),
          flameDetected_(false) {}

    void setRelayController(IRelayController* controller) {
        relayController_ = controller;
    }

    /**
     * @brief Set heat demand with PID-driven power level
     */
    void setHeatDemand(bool demand, bool highPower = false) {
        heatDemand_ = demand;
        requestedHighPower_ = highPower;
    }

    /**
     * @brief Set flame detected state (for testing)
     */
    void setFlameDetected(bool detected) {
        flameDetected_ = detected;
        // If flame is lost while running, go to ERROR
        if (!detected && (currentState_ == BurnerSMState::RUNNING_LOW ||
                          currentState_ == BurnerSMState::RUNNING_HIGH)) {
            currentState_ = BurnerSMState::ERROR;
            lastError_ = ErrorType::FLAME_LOSS;
            updateRelays();
        }
    }

    bool isFlameDetected() const {
        return flameDetected_;
    }

    /**
     * @brief Request burner start (legacy API)
     */
    void requestStart(PowerLevel level) {
        if (currentState_ == BurnerSMState::IDLE) {
            powerLevel_ = level;
            targetPowerLevel_ = level;
            heatDemand_ = true;
            requestedHighPower_ = (level == PowerLevel::HIGH);
            currentState_ = BurnerSMState::PRE_PURGE;
            stateStartTime_ = millis();
            ignitionRetries_ = 0;
            updateRelays();
        }
    }

    /**
     * @brief Request burner stop (legacy API)
     */
    void requestStop() {
        if (currentState_ == BurnerSMState::RUNNING_LOW ||
            currentState_ == BurnerSMState::RUNNING_HIGH) {
            heatDemand_ = false;
            currentState_ = BurnerSMState::POST_PURGE;
            stateStartTime_ = millis();
            updateRelays();
        }
    }

    /**
     * @brief Request seamless mode switch (water ↔ heating)
     */
    void requestModeSwitch() {
        if (currentState_ == BurnerSMState::RUNNING_LOW ||
            currentState_ == BurnerSMState::RUNNING_HIGH) {
            modeSwitchPending_ = true;
            currentState_ = BurnerSMState::MODE_SWITCHING;
            stateStartTime_ = millis();
            updateRelays();
        }
    }

    /**
     * @brief Emergency stop - immediate shutdown
     */
    void emergencyStop() {
        currentState_ = BurnerSMState::ERROR;
        powerLevel_ = PowerLevel::OFF;
        lastError_ = ErrorType::SAFETY_INTERLOCK;
        updateRelays();
    }

    /**
     * @brief Update state machine (call from task loop)
     */
    void update() {
        uint32_t elapsed = millis() - stateStartTime_;
        BurnerSMState oldState = currentState_;

        switch (currentState_) {
            case BurnerSMState::IDLE:
                // Check for heat demand
                if (heatDemand_) {
                    currentState_ = BurnerSMState::PRE_PURGE;
                    stateStartTime_ = millis();
                    ignitionRetries_ = 0;
                }
                break;

            case BurnerSMState::PRE_PURGE:
                if (!heatDemand_) {
                    // Demand removed during pre-purge
                    currentState_ = BurnerSMState::IDLE;
                    powerLevel_ = PowerLevel::OFF;
                } else if (elapsed > config_.prePurgeTime) {
                    currentState_ = BurnerSMState::IGNITION;
                    stateStartTime_ = millis();
                }
                break;

            case BurnerSMState::IGNITION:
                if (!heatDemand_) {
                    currentState_ = BurnerSMState::POST_PURGE;
                    stateStartTime_ = millis();
                } else if (flameDetected_ && elapsed > config_.flameStabilizationTime) {
                    // Flame detected and stabilized - ignition success
                    if (requestedHighPower_) {
                        currentState_ = BurnerSMState::RUNNING_HIGH;
                        powerLevel_ = PowerLevel::HIGH;
                    } else {
                        currentState_ = BurnerSMState::RUNNING_LOW;
                        powerLevel_ = PowerLevel::LOW;
                    }
                    stateStartTime_ = millis();
                } else if (elapsed > config_.ignitionTimeout && !flameDetected_) {
                    // Ignition timeout without flame - retry or lockout
                    ignitionRetries_++;
                    if (ignitionRetries_ >= config_.maxIgnitionRetries) {
                        currentState_ = BurnerSMState::LOCKOUT;
                        lastError_ = ErrorType::IGNITION_FAILURE;
                    } else {
                        // Retry - go back to pre-purge
                        currentState_ = BurnerSMState::PRE_PURGE;
                    }
                    stateStartTime_ = millis();
                }
                break;

            case BurnerSMState::RUNNING_LOW:
                if (!heatDemand_) {
                    currentState_ = BurnerSMState::POST_PURGE;
                    stateStartTime_ = millis();
                } else if (requestedHighPower_) {
                    // Power increase requested
                    currentState_ = BurnerSMState::RUNNING_HIGH;
                    powerLevel_ = PowerLevel::HIGH;
                    stateStartTime_ = millis();
                }
                break;

            case BurnerSMState::RUNNING_HIGH:
                if (!heatDemand_) {
                    currentState_ = BurnerSMState::POST_PURGE;
                    stateStartTime_ = millis();
                } else if (!requestedHighPower_) {
                    // Power decrease requested
                    currentState_ = BurnerSMState::RUNNING_LOW;
                    powerLevel_ = PowerLevel::LOW;
                    stateStartTime_ = millis();
                }
                break;

            case BurnerSMState::MODE_SWITCHING:
                if (elapsed > config_.modeSwitchTime) {
                    // Mode switch complete - return to running
                    modeSwitchPending_ = false;
                    if (heatDemand_) {
                        if (requestedHighPower_) {
                            currentState_ = BurnerSMState::RUNNING_HIGH;
                            powerLevel_ = PowerLevel::HIGH;
                        } else {
                            currentState_ = BurnerSMState::RUNNING_LOW;
                            powerLevel_ = PowerLevel::LOW;
                        }
                    } else {
                        currentState_ = BurnerSMState::POST_PURGE;
                    }
                    stateStartTime_ = millis();
                }
                break;

            case BurnerSMState::POST_PURGE:
                if (elapsed > config_.postPurgeTime) {
                    currentState_ = BurnerSMState::IDLE;
                    powerLevel_ = PowerLevel::OFF;
                }
                break;

            case BurnerSMState::LOCKOUT:
                // Auto-reset after lockout time
                if (elapsed > config_.lockoutDuration) {
                    currentState_ = BurnerSMState::IDLE;
                    ignitionRetries_ = 0;
                    lastError_ = ErrorType::NONE;
                }
                break;

            case BurnerSMState::ERROR:
                // Stays in error until manual reset
                break;
        }

        // Update relays if state changed
        if (oldState != currentState_) {
            updateRelays();
        }
    }

    /**
     * @brief Simulate ignition failure (for testing)
     */
    void simulateIgnitionFailure() {
        if (currentState_ == BurnerSMState::IGNITION) {
            ignitionRetries_++;
            if (ignitionRetries_ >= config_.maxIgnitionRetries) {
                currentState_ = BurnerSMState::LOCKOUT;
                lastError_ = ErrorType::IGNITION_FAILURE;
            } else {
                // Retry - go back to pre-purge
                currentState_ = BurnerSMState::PRE_PURGE;
            }
            stateStartTime_ = millis();
            updateRelays();
        }
    }

    /**
     * @brief Report error condition
     */
    void reportError(ErrorType error) {
        lastError_ = error;
        currentState_ = BurnerSMState::ERROR;
        powerLevel_ = PowerLevel::OFF;
        updateRelays();
    }

    /**
     * @brief Reset from lockout state
     */
    void resetLockout() {
        if (currentState_ == BurnerSMState::LOCKOUT) {
            currentState_ = BurnerSMState::IDLE;
            ignitionRetries_ = 0;
            lastError_ = ErrorType::NONE;
            updateRelays();
        }
    }

    // Getters
    BurnerSMState getCurrentState() const { return currentState_; }
    PowerLevel getPowerLevel() const { return powerLevel_; }
    ErrorType getLastError() const { return lastError_; }
    uint8_t getIgnitionRetries() const { return ignitionRetries_; }
    bool getHeatDemand() const { return heatDemand_; }
    bool isInLockout() const { return currentState_ == BurnerSMState::LOCKOUT; }
    bool isRunning() const {
        return currentState_ == BurnerSMState::RUNNING_LOW ||
               currentState_ == BurnerSMState::RUNNING_HIGH;
    }
    bool isInModeSwitching() const {
        return currentState_ == BurnerSMState::MODE_SWITCHING;
    }

    // For test compatibility
    void setPowerLevel(PowerLevel level) {
        if (isRunning()) {
            powerLevel_ = level;
            requestedHighPower_ = (level == PowerLevel::HIGH);
        }
    }

    // State setter for direct test manipulation
    void setStateForTesting(BurnerSMState state) {
        currentState_ = state;
        stateStartTime_ = millis();
    }
};

// Global millis function declaration
extern uint32_t millis();

#endif // MOCK_BURNER_STATE_MACHINE_H
