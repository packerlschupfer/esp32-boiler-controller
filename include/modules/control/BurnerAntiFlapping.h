// include/modules/control/BurnerAntiFlapping.h
#pragma once

#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "modules/control/BurnerStateMachine.h"

/**
 * @brief Anti-flapping mechanism for burner control
 * 
 * Prevents rapid cycling of burner on/off states and limits power level changes
 * to protect hardware and improve efficiency.
 */
class BurnerAntiFlapping {
public:
    /**
     * @brief Power levels for tracking changes
     */
    enum class PowerLevel {
        OFF = 0,
        POWER_LOW = 1,    // Renamed to avoid conflict with Arduino LOW macro
        POWER_HIGH = 2    // Renamed to avoid conflict with Arduino HIGH macro
    };
    
    /**
     * @brief Initialize anti-flapping system
     */
    static void initialize();
    
    /**
     * @brief Check if burner can turn on
     * @return true if minimum off time has elapsed
     */
    static bool canTurnOn();
    
    /**
     * @brief Check if burner can turn off
     * @return true if minimum on time has elapsed
     */
    static bool canTurnOff();
    
    /**
     * @brief Check if power level can be changed
     * @param newLevel Desired power level
     * @return true if change is allowed
     */
    static bool canChangePowerLevel(PowerLevel newLevel);

    /**
     * @brief Atomic check-and-reserve for power level change (Round 14 Issue #16)
     *
     * Prevents TOCTOU race by atomically checking permission and reserving the transition.
     * The caller MUST call commitPowerLevelChange() on successful transition,
     * or rollbackPowerLevelChange() if the transition fails.
     *
     * @param newLevel Desired power level
     * @return true if reservation was successful and transition may proceed
     */
    static bool reservePowerLevelChange(PowerLevel newLevel);

    /**
     * @brief Commit a previously reserved power level change
     *
     * Call this after successful state transition to finalize the change.
     */
    static void commitPowerLevelChange();

    /**
     * @brief Rollback a previously reserved power level change
     *
     * Call this if the state transition fails to release the reservation.
     */
    static void rollbackPowerLevelChange();
    
    /**
     * @brief Record burner turned on
     */
    static void recordBurnerOn();
    
    /**
     * @brief Record burner turned off
     */
    static void recordBurnerOff();
    
    /**
     * @brief Record power level change
     * @param level New power level
     */
    static void recordPowerLevelChange(PowerLevel level);
    
    /**
     * @brief Get time until burner can turn on
     * @return Milliseconds until allowed, 0 if allowed now
     */
    static uint32_t getTimeUntilCanTurnOn();
    
    /**
     * @brief Get time until burner can turn off
     * @return Milliseconds until allowed, 0 if allowed now
     */
    static uint32_t getTimeUntilCanTurnOff();
    
    /**
     * @brief Get time until power level can change
     * @return Milliseconds until allowed, 0 if allowed now
     */
    static uint32_t getTimeUntilCanChangePower();
    
    /**
     * @brief Check if PID output change is significant enough to act on
     * @param currentOutput Current PID output value
     * @param newOutput New PID output value
     * @return true if change exceeds deadband threshold
     */
    static bool isSignificantPIDChange(float currentOutput, float newOutput);
    
    /**
     * @brief Convert burner state to power level
     * @param state Burner state machine state
     * @return Corresponding power level
     */
    static PowerLevel stateToPowerLevel(BurnerSMState state);
    
    /**
     * @brief Get current power level
     * @return Current power level
     */
    static PowerLevel getCurrentPowerLevel() { return currentPowerLevel_; }
    
    /**
     * @brief Reset anti-flapping state (for emergency or testing)
     */
    static void reset();
    
private:
    // Mutex for thread-safe access to anti-flapping state
    static SemaphoreHandle_t mutex_;
    static constexpr TickType_t MUTEX_TIMEOUT = pdMS_TO_TICKS(10);

    // State tracking
    static bool isBurnerOn_;
    static PowerLevel currentPowerLevel_;

    // Reservation tracking (Round 14 Issue #16 - atomic check-and-reserve)
    static bool reservationPending_;
    static PowerLevel reservedLevel_;

    // Timing tracking
    static uint32_t lastBurnerOnTime_;
    static uint32_t lastBurnerOffTime_;
    static uint32_t lastPowerChangeTime_;

    // PID output tracking for deadband
    static float lastPIDOutput_;

    static const char* TAG;
};