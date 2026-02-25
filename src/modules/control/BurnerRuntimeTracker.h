// src/modules/control/BurnerRuntimeTracker.h
#ifndef BURNER_RUNTIME_TRACKER_H
#define BURNER_RUNTIME_TRACKER_H

#include <cstdint>
#include <atomic>

/**
 * @brief Burner runtime tracking with FRAM persistence
 *
 * Extracted from BurnerStateMachine.cpp (Round 21 Refactoring).
 * Manages burner runtime counters with persistence to RuntimeStorage and CriticalDataStorage.
 *
 * Thread Safety:
 * - Uses atomic operations for start time tracking
 * - RuntimeStorage and CriticalDataStorage provide their own thread-safety
 * - Safe to call from BurnerStateMachine state handlers
 */
class BurnerRuntimeTracker {
public:
    /**
     * @brief Record burner start time
     *
     * Called from onEnterRunningLow/High state entry actions.
     * Records current timestamp if not already running.
     */
    static void recordStartTime();

    /**
     * @brief Update runtime counters and persist to storage
     *
     * Called from onExitRunning state exit action.
     * Calculates elapsed time since recordStartTime() and updates:
     * - Total system runtime
     * - Burner-specific runtime
     * - Heating or water runtime (based on current mode)
     * - CriticalDataStorage counters (FRAM persistence)
     *
     * Handles millis() wraparound safely using Utils::elapsedMs().
     */
    static void updateRuntimeCounters();

    /**
     * @brief Get current burner start time (for testing/diagnostics)
     * @return Start timestamp in milliseconds, or 0 if not running
     */
    static uint32_t getStartTime();

private:
    // Track when burner started running (atomic for thread-safety)
    // M4 fix: restored std::atomic<uint32_t> (was regressed to plain uint32_t in Round 21 extraction)
    static std::atomic<uint32_t> burnerStartTime;
};

#endif // BURNER_RUNTIME_TRACKER_H
