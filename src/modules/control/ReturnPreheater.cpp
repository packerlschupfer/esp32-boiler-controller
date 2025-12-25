// src/modules/control/ReturnPreheater.cpp
#include "modules/control/ReturnPreheater.h"
#include "core/SystemResourceProvider.h"
#include "config/SystemConstants.h"
#include "config/SystemSettingsStruct.h"
#include "events/SystemEventsGenerated.h"
#include "shared/SharedSensorReadings.h"
#include "utils/MutexRetryHelper.h"
#include "LoggingMacros.h"
#include <Arduino.h>

const char* ReturnPreheater::TAG = "ReturnPreheater";

// Static member definitions
ReturnPreheater::State ReturnPreheater::state_ = State::IDLE;
uint8_t ReturnPreheater::currentCycle_ = 0;
uint32_t ReturnPreheater::cycleStartTime_ = 0;
uint32_t ReturnPreheater::preheatStartTime_ = 0;
bool ReturnPreheater::pumpOn_ = false;
uint32_t ReturnPreheater::lastPumpChangeTime_ = 0;

bool ReturnPreheater::start() {
    if (state_ == State::PREHEATING) {
        LOG_WARN(TAG, "Already preheating");
        return false;
    }

    // Get settings
    SystemSettings& settings = SRP::getSystemSettings();
    if (!settings.preheatEnabled) {
        LOG_INFO(TAG, "Preheating disabled - skipping");
        state_ = State::COMPLETE;
        return true;
    }

    // Check if preheating is even needed
    if (isDifferentialSafe()) {
        LOG_INFO(TAG, "Differential already safe - no preheating needed");
        state_ = State::COMPLETE;
        return true;
    }

    // Initialize state
    state_ = State::PREHEATING;
    currentCycle_ = 1;
    preheatStartTime_ = millis();
    cycleStartTime_ = millis();
    pumpOn_ = true;
    lastPumpChangeTime_ = millis();

    // Start first pump ON phase
    setPumpState(true);

    Temperature_t diff = getCurrentDifferential();
    char diffBuf[16];
    formatTemp(diffBuf, sizeof(diffBuf), diff);
    LOG_INFO(TAG, "Starting return preheating - differential=%s, cycle 1", diffBuf);

    return true;
}

bool ReturnPreheater::update() {
    if (state_ != State::PREHEATING) {
        return state_ == State::COMPLETE || state_ == State::TIMEOUT;
    }

    uint32_t now = millis();
    SystemSettings& settings = SRP::getSystemSettings();

    // Check global timeout
    uint32_t elapsed = now - preheatStartTime_;
    if (elapsed > settings.preheatTimeoutMs) {
        LOG_WARN(TAG, "Preheating timeout after %lu ms", elapsed);
        setPumpState(false);
        state_ = State::TIMEOUT;
        return true;
    }

    // Check max cycles
    if (currentCycle_ > settings.preheatMaxCycles) {
        LOG_WARN(TAG, "Max preheating cycles (%u) reached", settings.preheatMaxCycles);
        setPumpState(false);
        state_ = State::TIMEOUT;
        return true;
    }

    // Check if differential is now safe
    if (isDifferentialSafe()) {
        Temperature_t diff = getCurrentDifferential();
        char diffBuf[16];
        formatTemp(diffBuf, sizeof(diffBuf), diff);
        LOG_INFO(TAG, "Preheating complete - differential=%s after %u cycles", diffBuf, currentCycle_);
        setPumpState(false);
        state_ = State::COMPLETE;
        return true;
    }

    // Handle pump cycling
    uint32_t cycleElapsed = now - cycleStartTime_;
    uint16_t onDuration = getOnDurationMs(currentCycle_);
    uint16_t offDuration = getOffDurationMs(currentCycle_);

    if (pumpOn_) {
        // In ON phase - check if time to turn OFF
        if (cycleElapsed >= onDuration) {
            // Check minimum pump change time
            uint32_t sinceLastChange = now - lastPumpChangeTime_;
            if (sinceLastChange >= settings.preheatPumpMinMs) {
                setPumpState(false);
                pumpOn_ = false;
                cycleStartTime_ = now;
                lastPumpChangeTime_ = now;
                LOG_DEBUG(TAG, "Cycle %u: pump OFF for %u ms", currentCycle_, offDuration);
            }
        }
    } else {
        // In OFF phase - check if time to turn ON (next cycle)
        if (cycleElapsed >= offDuration) {
            // Check minimum pump change time
            uint32_t sinceLastChange = now - lastPumpChangeTime_;
            if (sinceLastChange >= settings.preheatPumpMinMs) {
                currentCycle_++;
                if (currentCycle_ <= settings.preheatMaxCycles) {
                    setPumpState(true);
                    pumpOn_ = true;
                    cycleStartTime_ = now;
                    lastPumpChangeTime_ = now;

                    Temperature_t diff = getCurrentDifferential();
                    char diffBuf[16];
                    formatTemp(diffBuf, sizeof(diffBuf), diff);
                    LOG_DEBUG(TAG, "Cycle %u: pump ON for %u ms (diff=%s)",
                             currentCycle_, getOnDurationMs(currentCycle_), diffBuf);
                }
            }
        }
    }

    return false;  // Still preheating
}

bool ReturnPreheater::isComplete() {
    return state_ == State::COMPLETE || state_ == State::TIMEOUT;
}

bool ReturnPreheater::isSuccess() {
    return state_ == State::COMPLETE;
}

ReturnPreheater::State ReturnPreheater::getState() {
    return state_;
}

void ReturnPreheater::stop() {
    if (state_ == State::PREHEATING) {
        setPumpState(false);
        LOG_INFO(TAG, "Preheating stopped manually");
    }
    state_ = State::IDLE;
    currentCycle_ = 0;
    pumpOn_ = false;
}

void ReturnPreheater::reset() {
    state_ = State::IDLE;
    currentCycle_ = 0;
    pumpOn_ = false;
    cycleStartTime_ = 0;
    preheatStartTime_ = 0;
    lastPumpChangeTime_ = 0;
}

uint8_t ReturnPreheater::getCurrentCycle() {
    return (state_ == State::PREHEATING) ? currentCycle_ : 0;
}

uint8_t ReturnPreheater::getProgress() {
    if (state_ == State::IDLE) return 0;
    if (state_ == State::COMPLETE) return 100;
    if (state_ == State::TIMEOUT) return 100;

    SystemSettings& settings = SRP::getSystemSettings();
    if (settings.preheatMaxCycles == 0) return 0;

    // Progress based on cycles completed
    uint8_t progress = (currentCycle_ * 100) / settings.preheatMaxCycles;
    return (progress > 100) ? 100 : progress;
}

bool ReturnPreheater::shouldPumpBeOn() {
    return (state_ == State::PREHEATING) && pumpOn_;
}

const char* ReturnPreheater::stateToString(State state) {
    switch (state) {
        case State::IDLE:       return "IDLE";
        case State::PREHEATING: return "PREHEATING";
        case State::COMPLETE:   return "COMPLETE";
        case State::TIMEOUT:    return "TIMEOUT";
        default:                return "UNKNOWN";
    }
}

uint16_t ReturnPreheater::getOnDurationMs(uint8_t cycle) {
    using namespace SystemConstants::Safety::ReturnPreheat;

    // Progressive ON durations (in seconds, convert to ms)
    switch (cycle) {
        case 1:  return CYCLE_1_ON_SEC * 1000;
        case 2:  return CYCLE_2_ON_SEC * 1000;
        case 3:  return CYCLE_3_ON_SEC * 1000;
        case 4:  return CYCLE_4_ON_SEC * 1000;
        default: return CYCLE_5_PLUS_ON_SEC * 1000;
    }
}

uint16_t ReturnPreheater::getOffDurationMs(uint8_t cycle) {
    using namespace SystemConstants::Safety::ReturnPreheat;
    SystemSettings& settings = SRP::getSystemSettings();

    // Base OFF durations (decreasing - longer wait initially, shorter as system warms)
    uint16_t baseOffSec;
    switch (cycle) {
        case 1:  baseOffSec = CYCLE_1_OFF_SEC; break;
        case 2:  baseOffSec = CYCLE_2_OFF_SEC; break;
        case 3:  baseOffSec = CYCLE_3_OFF_SEC; break;
        case 4:  baseOffSec = CYCLE_4_OFF_SEC; break;
        default: baseOffSec = CYCLE_5_PLUS_OFF_SEC; break;
    }

    // Apply multiplier as scaling factor (5 = 1x, 10 = 2x, 1 = 0.2x)
    uint32_t scaledMs = (static_cast<uint32_t>(baseOffSec) * 1000 * settings.preheatOffMultiplier) / 5;
    return static_cast<uint16_t>(scaledMs);
}

bool ReturnPreheater::isDifferentialSafe() {
    Temperature_t diff = getCurrentDifferential();
    if (diff == TEMP_INVALID) {
        // Can't determine - assume not safe
        return false;
    }

    SystemSettings& settings = SRP::getSystemSettings();
    return diff < settings.preheatSafeDiff;
}

Temperature_t ReturnPreheater::getCurrentDifferential() {
    // Get sensor readings with mutex protection
    auto guard = MutexRetryHelper::acquireGuard(
        SRP::getSensorReadingsMutex(),
        "ReturnPreheater-SensorRead"
    );

    if (!guard) {
        LOG_WARN(TAG, "Failed to acquire sensor mutex");
        return TEMP_INVALID;
    }

    const SharedSensorReadings& readings = SRP::getSensorReadings();

    if (!readings.isBoilerTempOutputValid || !readings.isBoilerTempReturnValid) {
        return TEMP_INVALID;
    }

    return tempSub(readings.boilerTempOutput, readings.boilerTempReturn);
}

void ReturnPreheater::setPumpState(bool on) {
    // Note: We only update internal state here. The actual pump control is done by
    // PumpControlModule which queries shouldPumpBeOn() during preheating.
    // This maintains single point of control for the heating pump relay.
    pumpOn_ = on;
    LOG_DEBUG(TAG, "Pump state set to %s", on ? "ON" : "OFF");
}
