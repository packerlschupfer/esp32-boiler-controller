// include/modules/control/BoilerPowerLevel.h
#ifndef BOILER_POWER_LEVEL_H
#define BOILER_POWER_LEVEL_H

#include <cstdint>
#include "modules/control/FixedPointPIDStep.h"
#include "modules/control/PIDGainFixedPoint.h"

/**
 * @brief BoilerTempController's power level decision (header-only, native-testable).
 *
 * The mapping from PID output / temperature error to OFF, HALF or FULL, with the
 * hysteresis of both burner types. BoilerTempController uses it for its control
 * cycle and for predictHeatDemand() (BurnerControlTask's arming check), and the
 * native tests use it for multi-cycle scenarios. Anti-flapping stays in the
 * controller (firmware only).
 *
 * Temperatures in tenths of °C, PID output in percent.
 */
namespace BoilerPowerLevel {

    enum class Level {
        OFF = 0,
        HALF = 1,
        FULL = 2
    };

    // PID output thresholds (BoilerTempController::Config defaults). Pure PID output
    // is centred at 50 % at target; wide bands minimise burner cycling.
    constexpr uint8_t DEFAULT_OFF_THRESHOLD = 35;         // Below this -> OFF (well above target)
    constexpr uint8_t DEFAULT_HALF_THRESHOLD = 45;        // Above this (+ hysteresis) -> at least HALF
    constexpr uint8_t DEFAULT_FULL_THRESHOLD = 75;        // Above this -> FULL (significantly below target)
    constexpr uint8_t DEFAULT_THRESHOLD_HYSTERESIS = 10;  // Wide hysteresis prevents oscillation

    // Bang-bang bands (BoilerTempController::Config defaults)
    constexpr int16_t DEFAULT_OFF_HYSTERESIS = 50;         // +5.0 °C above target -> OFF
    constexpr int16_t DEFAULT_ON_HYSTERESIS = 30;          // 3.0 °C below target -> HALF
    constexpr int16_t DEFAULT_FULL_POWER_THRESHOLD = 100;  // 10.0 °C below target -> FULL

    // PID cycle timing (BoilerTempController::calculateModulating). The PID only runs
    // while a request is active and not during autotune; a longer gap resets it.
    constexpr uint32_t PID_MAX_DT_MS = 10000;     // control cycle is 2.5 s
    constexpr uint32_t PID_NOMINAL_DT_MS = 2500;

    struct Thresholds {
        uint8_t offThreshold;
        uint8_t halfThreshold;
        uint8_t fullThreshold;
        uint8_t thresholdHysteresis;
    };

    struct BangBangBands {
        int16_t offHysteresis;
        int16_t onHysteresis;
        int16_t fullPowerThreshold;
    };

    inline Thresholds defaultThresholds() {
        Thresholds t = {DEFAULT_OFF_THRESHOLD, DEFAULT_HALF_THRESHOLD,
                        DEFAULT_FULL_THRESHOLD, DEFAULT_THRESHOLD_HYSTERESIS};
        return t;
    }

    inline BangBangBands defaultBangBangBands() {
        BangBangBands b = {DEFAULT_OFF_HYSTERESIS, DEFAULT_ON_HYSTERESIS, DEFAULT_FULL_POWER_THRESHOLD};
        return b;
    }

    /**
     * @brief Modulating (PID) burner: power level from the PID output with hysteresis
     *
     * OFF turns on above halfThreshold + hysteresis (FULL directly above fullThreshold);
     * HALF turns off below offThreshold and goes FULL above fullThreshold; FULL drops
     * to HALF below fullThreshold - hysteresis and to OFF below offThreshold.
     */
    inline Level fromPidOutput(Level last, uint8_t pidOutput, const Thresholds& t) {
        const uint8_t offLow = t.offThreshold;
        const uint8_t halfHigh = static_cast<uint8_t>(t.halfThreshold + t.thresholdHysteresis);
        const uint8_t fullLow = static_cast<uint8_t>(t.fullThreshold - t.thresholdHysteresis);
        const uint8_t fullHigh = t.fullThreshold;

        Level desired = last;
        switch (last) {
            case Level::OFF:
                if (pidOutput > halfHigh) {
                    desired = (pidOutput > fullHigh) ? Level::FULL : Level::HALF;
                }
                break;

            case Level::HALF:
                if (pidOutput < offLow) {
                    desired = Level::OFF;
                } else if (pidOutput > fullHigh) {
                    desired = Level::FULL;
                }
                break;

            case Level::FULL:
                if (pidOutput < offLow) {
                    desired = Level::OFF;
                } else if (pidOutput < fullLow) {
                    desired = Level::HALF;
                }
                break;
        }
        return desired;
    }

    /**
     * @brief Two-stage burner: three-point bang-bang with hysteresis
     * @param error target - current (positive = need heat)
     */
    inline Level fromBangBangError(Level last, int16_t error, const BangBangBands& b) {
        if (error < -b.offHysteresis) {
            return Level::OFF;        // Too hot
        }
        if (error > b.fullPowerThreshold) {
            return Level::FULL;       // Very cold
        }
        if (error > b.onHysteresis) {
            return Level::HALF;       // Cold enough
        }
        return last;                  // In the hysteresis band: keep the current level
    }

    /**
     * @brief One modulating control cycle: PID step, power mapping, hysteresis
     *
     * Advances pidState. Without anti-flapping, which the controller applies on top.
     */
    inline Level nextModulatingLevel(Level last, FixedPointPIDStep::State& pidState,
                                     const FixedPointPIDStep::Limits& limits,
                                     int16_t target, int16_t current,
                                     int32_t Kp, int32_t Ki, int32_t Kd, uint32_t dtMs,
                                     const Thresholds& t) {
        const int16_t adjustment = FixedPointPIDStep::step(pidState, limits, target, current,
                                                           Kp, Ki, Kd, dtMs);
        return fromPidOutput(last, PIDGainFixedPoint::powerPercentFromAdjustment(adjustment), t);
    }

    /**
     * @brief Has the PID been paused? (no control cycle for more than PID_MAX_DT_MS)
     * @param lastPidMs millis() of the last PID cycle; read BEFORE nowMs is taken, so a
     *        cycle stored concurrently can never make the difference wrap (review R1-2)
     */
    inline bool pidPaused(uint32_t nowMs, uint32_t lastPidMs) {
        return (nowMs - lastPidMs) > PID_MAX_DT_MS;
    }

    // How the next modulating control cycle starts
    struct CycleStart {
        bool resetPid;   // run the step from FixedPointPIDStep::resetState()
        Level level;     // level the power mapping's hysteresis starts from
    };

    /**
     * @brief Start of the next modulating cycle (calculateModulating, predictHeatDemand)
     *
     * - pause (no cycle for PID_MAX_DT_MS): the PID restarts and the level starts from
     *   OFF. The burner was not driven by this controller during the pause (no request,
     *   autotune, no usable boiler temperature), so the last level is stale: HALF held
     *   down to 35 % and let a new request arm up to ~14 °C above target with the water
     *   gains (review R1-1).
     * - mode or gain change without a pause: PID reset, level kept, so a seamless
     *   water <-> heating handover keeps its hysteresis.
     */
    inline CycleStart modulatingCycleStart(Level lastLevel, bool paused, bool modeOrGainsChanged) {
        CycleStart start;
        start.resetPid = paused || modeOrGainsChanged;
        start.level = paused ? Level::OFF : lastLevel;
        return start;
    }

    /**
     * @brief Prediction of the next modulating cycle (BoilerTempController::predictHeatDemand)
     *
     * Applies modulatingCycleStart() to a copy of the PID state and runs one cycle with the
     * nominal dt. Anti-flapping is not included.
     */
    inline Level predictModulatingLevel(Level lastLevel, FixedPointPIDStep::State pidState,
                                        const FixedPointPIDStep::Limits& limits,
                                        bool paused, bool modeOrGainsChanged,
                                        int16_t target, int16_t current,
                                        int32_t Kp, int32_t Ki, int32_t Kd,
                                        const Thresholds& t) {
        const CycleStart start = modulatingCycleStart(lastLevel, paused, modeOrGainsChanged);
        if (start.resetPid) {
            pidState = FixedPointPIDStep::resetState();
        }
        return nextModulatingLevel(start.level, pidState, limits, target, current,
                                   Kp, Ki, Kd, PID_NOMINAL_DT_MS, t);
    }

} // namespace BoilerPowerLevel

#endif // BOILER_POWER_LEVEL_H
