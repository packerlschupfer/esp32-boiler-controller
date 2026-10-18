// include/modules/control/ReturnPreheater.h
#ifndef RETURN_PREHEATER_H
#define RETURN_PREHEATER_H

#include <cstdint>
#include "shared/Temperature.h"

/**
 * @brief Return Preheater - Thermal shock mitigation via pump cycling
 *
 * When transitioning from water heating to space heating, the heating return
 * line may be cold while the boiler is hot. Starting the burner with a large
 * temperature differential (>30°C) risks thermal shock damage.
 *
 * This module cycles the heating pump to gradually warm the return line:
 * - Progressive ON durations: 3s → 5s → 8s → 12s → 15s (increasing circulation)
 * - Progressive OFF durations: 25s → 20s → 15s → 10s → 5s (decreasing wait)
 * - Exits when differential < safe threshold (25°C) or timeout
 *
 * Usage:
 *   1. BurnerSafetyValidator detects THERMAL_SHOCK_RISK
 *   2. BurnerControlTask calls ReturnPreheater::start()
 *   3. Call ReturnPreheater::update() periodically (e.g., every 100ms)
 *   4. When isComplete() returns true, burner can start
 */
class ReturnPreheater {
public:
    enum class State {
        IDLE,           // Not active
        PREHEATING,     // Cycling pump
        COMPLETE,       // Differential OK, safe to start burner
        TIMEOUT         // Max cycles/time reached without success
    };

    /**
     * @brief Start preheating sequence
     * @return true if started successfully, false if already running
     */
    static bool start();

    /**
     * @brief Update preheating state machine
     * Call periodically (every 100-500ms recommended)
     * @return true when preheating is complete (either success or timeout)
     */
    static bool update();

    /**
     * @brief Check if preheating is complete (safe to start burner)
     * @return true if complete or timeout
     */
    static bool isComplete();

    /**
     * @brief Check if preheating completed successfully (differential OK)
     * @return true only if differential is below safe threshold
     */
    static bool isSuccess();

    /**
     * @brief Get current state
     * @return Current preheating state
     */
    static State getState();

    /**
     * @brief Force stop preheating
     */
    static void stop();

    /**
     * @brief Reset to IDLE state (for new cycle)
     */
    static void reset();

    /**
     * @brief Get current cycle number (1-based)
     * @return Current cycle (0 if not active)
     */
    static uint8_t getCurrentCycle();

    /**
     * @brief Get progress percentage (0-100)
     * @return Estimated progress based on cycles completed
     */
    static uint8_t getProgress();

    /**
     * @brief Check if pump should be ON in current state
     * @return true if pump should be running
     */
    static bool shouldPumpBeOn();

    /**
     * @brief Convert state to string for logging
     * @param state State to convert
     * @return String representation
     */
    static const char* stateToString(State state);

private:
    // State tracking
    static State state_;
    static uint8_t currentCycle_;
    static uint32_t cycleStartTime_;
    static uint32_t preheatStartTime_;
    static bool pumpOn_;
    static uint32_t lastPumpChangeTime_;

    // Get ON duration for current cycle (progressive)
    static uint16_t getOnDurationMs(uint8_t cycle);

    // Get OFF duration (ON × multiplier)
    static uint16_t getOffDurationMs(uint8_t cycle);

    // Check if thermal differential is safe
    static bool isDifferentialSafe();

    // Get current thermal differential
    static Temperature_t getCurrentDifferential();

    // Set pump state via relay event
    static void setPumpState(bool on);

    static const char* TAG;
};

#endif // RETURN_PREHEATER_H
