// include/modules/control/AutotuneClock.h
#ifndef AUTOTUNE_CLOCK_H
#define AUTOTUNE_CLOCK_H

#include <cstdint>

/**
 * @brief Run time of an autotune run in seconds (header-only, native-testable).
 *
 * PIDAutoTuner works in float seconds: relay switch times, peak/trough times, the
 * oscillation period and the timeout. Feeding it millis()/1000 as an absolute value
 * breaks twice (review 2026-09-14 pid-7):
 *
 * - millis() wraps after ~49.7 days. The time base then jumps back to 0, so elapsed
 *   times go negative (timeout never fires, the run is extended forever) and the
 *   peak/trough timestamps of one run are no longer ordered (negative periods, wrong Tu).
 * - float has a 24-bit mantissa. At an uptime of 4.29e9 ms the step of
 *   static_cast<float>(millis())/1000.0f is 0.5 s, and above ~2.3e6 s it is larger than
 *   the 100 ms the relay test resolves; peak times would quantise to the same value.
 *
 * Both disappear when the tuner is fed the time *since the start of the run*: the
 * subtraction is done on uint32_t, where the wrap cancels out, and the result stays
 * below MAX_TUNING_TIME_SECONDS (5400 s, resolution ~0.5 ms as a float).
 */
namespace AutotuneClock {

    class Clock {
    public:
        Clock() { reset(); }

        void reset() {
            startMs_ = 0;
            started_ = false;
        }

        /**
         * @brief Start the run (BoilerTempController::startAutoTuning)
         */
        void start(uint32_t nowMs) {
            startMs_ = nowMs;
            started_ = true;
        }

        bool started() const { return started_; }

        /**
         * @brief Milliseconds since start(), wrap-safe (unsigned subtraction)
         */
        uint32_t elapsedMs(uint32_t nowMs) const {
            return started_ ? (nowMs - startMs_) : 0;
        }

        /**
         * @brief Seconds since start() for PIDAutoTuner::update()
         *
         * Starts the clock if start() was missed, so the first sample is always 0 s
         * and never an absolute millis() value.
         */
        float elapsedSeconds(uint32_t nowMs) {
            if (!started_) {
                start(nowMs);
            }
            return static_cast<float>(nowMs - startMs_) / 1000.0f;
        }

    private:
        uint32_t startMs_;
        bool started_;
    };

} // namespace AutotuneClock

#endif // AUTOTUNE_CLOCK_H
