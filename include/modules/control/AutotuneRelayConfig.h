// include/modules/control/AutotuneRelayConfig.h
#ifndef AUTOTUNE_RELAY_CONFIG_H
#define AUTOTUNE_RELAY_CONFIG_H

#include <cmath>

/**
 * @brief Relay-feedback autotune parameters from settings (header-only, native-testable).
 *
 * - amplitude (pid/autotune/amplitude): relay output half-swing d in percent, used in
 *   Ku = 4 d / (pi a)
 * - hysteresis (pid/autotune/hysteresis): switching band around the setpoint in °C
 *
 * BoilerTempController drives the relay test OFF <-> FULL (0 <-> 100 %), a half-swing of
 * 50 % on a two-stage burner. Another amplitude is used as-is in the formula, so the
 * resulting gains scale by amplitude / 50 - only meaningful for a burner whose relay
 * output really swings by that amount (e.g. a modulating output).
 */
namespace AutotuneRelayConfig {

    constexpr float AMPLITUDE_MIN = 10.0f;
    constexpr float AMPLITUDE_MAX = 100.0f;
    constexpr float HYSTERESIS_MIN = 0.5f;
    constexpr float HYSTERESIS_MAX = 10.0f;

    // Half-swing of the OFF <-> FULL relay test on a two-stage burner
    constexpr float TWO_STAGE_SWING = 50.0f;

    inline float amplitudeOrDefault(float amplitude, float fallback) {
        return (std::isfinite(amplitude) && amplitude >= AMPLITUDE_MIN && amplitude <= AMPLITUDE_MAX)
                   ? amplitude : fallback;
    }

    inline float hysteresisOrDefault(float hysteresis, float fallback) {
        return (std::isfinite(hysteresis) && hysteresis >= HYSTERESIS_MIN && hysteresis <= HYSTERESIS_MAX)
                   ? hysteresis : fallback;
    }

    inline bool matchesOutputSwing(float amplitude, float outputSwing) {
        return std::fabs(amplitude - outputSwing) < 0.05f;
    }

    // Factor by which the tuned gains differ from those for the real output swing
    inline float gainScale(float amplitude, float outputSwing) {
        return amplitude / outputSwing;
    }

} // namespace AutotuneRelayConfig

#endif // AUTOTUNE_RELAY_CONFIG_H
