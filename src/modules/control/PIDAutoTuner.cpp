// src/modules/control/PIDAutoTuner.cpp
#include "PIDAutoTuner.h"
#include "config/SystemConstants.h"
#include "SemaphoreGuard.h"
#include "LoggingMacros.h"
#include <vector>      // Only used locally for sorting in calculateAveragePeriod
#include <algorithm>
#include <numeric>


static const char* TAG = "PIDAutoTuner";

PIDAutoTuner::PIDAutoTuner()
    : setpoint(0)
    , outputStep(SystemConstants::PID::Autotune::DEFAULT_RELAY_AMPLITUDE)
    , hysteresis(SystemConstants::PID::Autotune::DEFAULT_RELAY_HYSTERESIS)
    , method(TuningMethod::ZIEGLER_NICHOLS_PI)
    , state(TuningState::IDLE)
    , relayState(false)
    , startTimeValid(false)
    , startTime(0)
    , lastSwitchTime(0) {
    
    mutex = xSemaphoreCreateMutex();
    if (mutex == nullptr) {
        LOG_ERROR(TAG, "Failed to create mutex");
    }
}

PIDAutoTuner::~PIDAutoTuner() {
    if (mutex != nullptr) {
        vSemaphoreDelete(mutex);
    }
}

bool PIDAutoTuner::startTuning(float targetSetpoint, float relayAmplitude, 
                                float relayHysteresis, TuningMethod tuningMethod) {
    SemaphoreGuard guard(mutex, pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "Failed to acquire mutex");
        return false;
    }
    
    if (state != TuningState::IDLE) {
        LOG_WARN(TAG, "Tuning already in progress");
        return false;
    }
    
    // Initialize parameters
    setpoint = targetSetpoint;
    outputStep = relayAmplitude;
    hysteresis = relayHysteresis;
    method = tuningMethod;
    
    // Clear previous data
    oscillationData.clear();
    peakTimes.clear();
    peakValues.clear();
    troughTimes.clear();
    troughValues.clear();
    
    // Reset state
    state = TuningState::RELAY_TEST;
    relayState = false;
    startTimeValid = false;
    startTime = 0;
    lastSwitchTime = 0;
    result = TuningResult();

    // Reset peak/trough tracking (set properly on first sample)
    extrema_.reset();
    
    LOG_INFO(TAG, "Starting PID auto-tuning: setpoint=%.1f, amplitude=%.1f, hysteresis=%.1f",
             setpoint, outputStep, hysteresis);
    
    return true;
}

float PIDAutoTuner::update(float currentTemp, float currentTime) {
    SemaphoreGuard guard(mutex, pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "Failed to acquire mutex");
        return 0.0f;
    }
    
    if (state != TuningState::RELAY_TEST) {
        return 0.0f;
    }
    
    // Initialize start time. An explicit flag, not "startTime == 0": currentTime is the
    // time since the start of the run, so the first sample really is 0 s.
    if (!startTimeValid) {
        startTime = currentTime;
        lastSwitchTime = currentTime;
        startTimeValid = true;
    }

    // The caller guarantees a monotonic time base (AutotuneClock). Time running backwards
    // means the time source wrapped or was re-based: every stored peak/trough time would
    // then be out of order (negative periods, wrong Tu) and the timeout below could never
    // fire again. Fail the run instead of analysing corrupt data.
    if (currentTime < startTime) {
        LOG_ERROR(TAG, "Auto-tuning time base went backwards (%.1f < %.1f) - aborting",
                  currentTime, startTime);
        state = TuningState::FAILED;
        return 0.0f;
    }

    // Check for timeout (elapsed time only, never an absolute comparison)
    if ((currentTime - startTime) > MAX_TUNING_TIME) {
        LOG_ERROR(TAG, "Auto-tuning timeout");
        state = TuningState::FAILED;
        return 0.0f;
    }

    // Perform relay control
    float output = relayControl(currentTemp, currentTime);
    
    // Store data point
    oscillationData.push_back({currentTime, currentTemp, output});
    
    // Detect peaks and troughs
    detectPeaksAndTroughs(currentTemp, currentTime);
    
    // Check if we have enough cycles
    if (hasEnoughCycles()) {
        LOG_INFO(TAG, "Sufficient oscillation cycles detected, analyzing...");
        state = TuningState::ANALYZING;
        
        if (analyzeOscillations()) {
            calculatePIDParameters();
            state = TuningState::COMPLETE;
            LOG_INFO(TAG, "Auto-tuning complete: Kp=%.3f, Ki=%.3f, Kd=%.3f",
                     result.Kp, result.Ki, result.Kd);
        } else {
            state = TuningState::FAILED;
            LOG_ERROR(TAG, "Failed to analyze oscillations");
        }
        
        return 0.0f;
    }
    
    return output;
}

void PIDAutoTuner::stopTuning() {
    SemaphoreGuard guard(mutex, pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        return;
    }
    
    if (state == TuningState::RELAY_TEST) {
        LOG_INFO(TAG, "Auto-tuning stopped by user");
        state = TuningState::IDLE;
    }
}

uint8_t PIDAutoTuner::getProgress() const {
    if (state == TuningState::IDLE) return 0;
    if (state == TuningState::COMPLETE) return 100;
    if (state == TuningState::FAILED) return 0;

    // Calculate progress based on number of cycles
    size_t cycles = std::min(peakTimes.size(), troughTimes.size());
    return static_cast<uint8_t>((cycles * 100) / MIN_CYCLES);
}

uint8_t PIDAutoTuner::getCycleCount() const {
    return static_cast<uint8_t>(std::min(peakTimes.size(), troughTimes.size()));
}

float PIDAutoTuner::getElapsedTime() const {
    if (state == TuningState::IDLE || !startTimeValid) return 0.0f;
    if (!oscillationData.empty()) {
        return oscillationData.back().time - startTime;
    }
    return 0.0f;
}

const char* PIDAutoTuner::getStatusMessage() const {
    switch (state) {
        case TuningState::IDLE:
            return "Ready to start auto-tuning";
        case TuningState::RELAY_TEST:
            return "Performing relay feedback test...";
        case TuningState::ANALYZING:
            return "Analyzing oscillations...";
        case TuningState::COMPLETE:
            return "Auto-tuning complete";
        case TuningState::FAILED:
            return "Auto-tuning failed";
        default:
            return "Unknown state";
    }
}

float PIDAutoTuner::relayControl(float currentTemp, float currentTime) {
    float error = setpoint - currentTemp;
    bool previousState = relayState;

    // Track the extremes of the current relay phase (see RelayExtremaTracker:
    // the boiler lags the relay, so the true peak lies in the OFF phase and the
    // true trough in the ON phase)
    extrema_.sample(currentTemp, currentTime);

    // Relay with hysteresis
    if (relayState) {
        // Currently high, switch to low if error < -hysteresis
        // (temp is above setpoint + hysteresis)
        if (error < -hysteresis) {
            relayState = false;
        }
    } else {
        // Currently low, switch to high if error > hysteresis
        // (temp is below setpoint - hysteresis)
        if (error > hysteresis) {
            relayState = true;
        }
    }

    if (relayState != previousState) {
        // ON phase ended -> trough (min of that ON phase)
        // OFF phase ended -> peak (max of that OFF phase)
        RelayExtrema::Event event = extrema_.onSwitch(relayState, currentTemp, currentTime);
        if (event.valid) {
            if (event.isPeak) {
                peakTimes.push_back(event.time);
                peakValues.push_back(event.value);
            } else {
                troughTimes.push_back(event.time);
                troughValues.push_back(event.value);
            }
            LOG_INFO(TAG, "%s recorded: %.1f°C at t=%.0fs (cycles: %u)",
                     event.isPeak ? "Peak" : "Trough",
                     event.value, event.time - startTime,
                     static_cast<unsigned>(std::min(peakTimes.size(), troughTimes.size())));
        }
    }

    // Log relay switch
    if (relayState != previousState) {
        if (!oscillationData.empty()) {
            lastSwitchTime = currentTime;
        }
        LOG_INFO(TAG, "Relay switch: %s -> %s (temp=%.1f, error=%.1f, hyst=%.1f)",
                 previousState ? "ON" : "OFF",
                 relayState ? "ON" : "OFF",
                 currentTemp, error, hysteresis);
    }

    return relayState ? outputStep : -outputStep;
}

void PIDAutoTuner::detectPeaksAndTroughs(float currentTemp, float currentTime) {
    // Peak/trough detection is now handled in relayControl() based on relay switches
    // This function is kept for compatibility but does nothing
    (void)currentTemp;
    (void)currentTime;
}

bool PIDAutoTuner::analyzeOscillations() {
    if (peakTimes.size() < 2 || troughTimes.size() < 2) {
        LOG_ERROR(TAG, "Insufficient oscillation data");
        return false;
    }
    
    // Calculate average period
    float avgPeriod = calculateAveragePeriod();
    if (avgPeriod <= 0) {
        LOG_ERROR(TAG, "Invalid oscillation period");
        return false;
    }
    
    // Calculate amplitude
    float amplitude = calculateAmplitude();
    if (amplitude <= 0) {
        LOG_ERROR(TAG, "Invalid oscillation amplitude");
        return false;
    }
    
    // Calculate ultimate gain using relay feedback formula
    // Ku = (4 * d) / (π * a)
    // where d is relay amplitude and a is oscillation amplitude
    result.ultimateGain = (4.0f * outputStep) / (static_cast<float>(M_PI) * amplitude);
    result.ultimatePeriod = avgPeriod;
    
    LOG_INFO(TAG, "Ultimate gain Ku=%.3f, Ultimate period Tu=%.1f seconds",
             result.ultimateGain, result.ultimatePeriod);
    
    return true;
}

void PIDAutoTuner::calculatePIDParameters() {
    applyTuningMethod(result.ultimateGain, result.ultimatePeriod);
    result.valid = true;
}

void PIDAutoTuner::applyTuningMethod(float Ku, float Tu) {
    switch (method) {
        case TuningMethod::ZIEGLER_NICHOLS_PI:
            // Conservative PI tuning
            result.Kp = 0.45f * Ku;
            result.Ki = result.Kp / (0.83f * Tu);
            result.Kd = 0.0f;
            break;
            
        case TuningMethod::ZIEGLER_NICHOLS_PID:
            // Classic PID tuning
            result.Kp = 0.6f * Ku;
            result.Ki = result.Kp / (0.5f * Tu);
            result.Kd = result.Kp * 0.125f * Tu;
            break;
            
        case TuningMethod::TYREUS_LUYBEN:
            // More conservative, less overshoot
            result.Kp = 0.3125f * Ku;
            result.Ki = result.Kp / (2.2f * Tu);
            result.Kd = result.Kp * 0.37f * Tu;
            break;
            
        case TuningMethod::COHEN_COON:
            // Modified for relay feedback (approximation)
            result.Kp = 0.35f * Ku;
            result.Ki = result.Kp / (1.2f * Tu);
            result.Kd = result.Kp * 0.25f * Tu;
            break;
            
        case TuningMethod::LAMBDA_TUNING:
            // Smooth control, minimal overshoot
            // Lambda is set to Tu for conservative tuning
            float lambda = Tu;
            result.Kp = 0.2f * Ku;
            result.Ki = result.Kp / lambda;
            result.Kd = 0.0f; // PI control for lambda tuning
            break;
    }
    
    // Apply safety limits
    result.Kp = std::max(0.1f, std::min(100.0f, result.Kp));
    result.Ki = std::max(0.0f, std::min(10.0f, result.Ki));
    result.Kd = std::max(0.0f, std::min(10.0f, result.Kd));
}

bool PIDAutoTuner::hasEnoughCycles() const {
    // Need at least MIN_CYCLES complete oscillations
    size_t peakCount = peakTimes.size();
    size_t troughCount = troughTimes.size();
    size_t cycles = std::min(peakCount, troughCount);
    
    return cycles >= MIN_CYCLES;
}

float PIDAutoTuner::calculateAveragePeriod() const {
    std::vector<float> periods;
    
    // Calculate periods from peak to peak
    for (size_t i = 1; i < peakTimes.size(); i++) {
        periods.push_back(peakTimes[i] - peakTimes[i-1]);
    }
    
    // Calculate periods from trough to trough
    for (size_t i = 1; i < troughTimes.size(); i++) {
        periods.push_back(troughTimes[i] - troughTimes[i-1]);
    }
    
    if (periods.empty()) return 0;
    
    // Calculate average, excluding outliers
    std::sort(periods.begin(), periods.end());
    
    // Remove top and bottom 20% if we have enough data
    size_t trimCount = periods.size() / 5;
    if (periods.size() > 5) {
        periods.erase(periods.begin(), periods.begin() + trimCount);
        periods.erase(periods.end() - trimCount, periods.end());
    }
    
    float sum = std::accumulate(periods.begin(), periods.end(), 0.0f);
    return sum / static_cast<float>(periods.size());
}

float PIDAutoTuner::calculateAmplitude() const {
    if (peakValues.empty() || troughValues.empty()) return 0;
    
    // Calculate average peak and trough values
    float avgPeak = std::accumulate(peakValues.begin(), peakValues.end(), 0.0f) / static_cast<float>(peakValues.size());
    float avgTrough = std::accumulate(troughValues.begin(), troughValues.end(), 0.0f) / static_cast<float>(troughValues.size());
    
    // Amplitude is half the peak-to-peak value
    return (avgPeak - avgTrough) / 2.0f;
}