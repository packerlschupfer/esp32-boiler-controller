// src/modules/control/BurnerAntiFlapping.cpp
#include "modules/control/BurnerAntiFlapping.h"
#include "config/SystemConstants.h"
#include "utils/Utils.h"  // Round 16 Issue #8: Safe elapsed time calculation
#include "LoggingMacros.h"
#include <MutexGuard.h>
#include <Arduino.h>
#include <cmath>

// Static member definitions
SemaphoreHandle_t BurnerAntiFlapping::mutex_ = nullptr;
bool BurnerAntiFlapping::isBurnerOn_ = false;
BurnerAntiFlapping::PowerLevel BurnerAntiFlapping::currentPowerLevel_ = PowerLevel::OFF;
bool BurnerAntiFlapping::reservationPending_ = false;  // Round 14 Issue #16
BurnerAntiFlapping::PowerLevel BurnerAntiFlapping::reservedLevel_ = PowerLevel::OFF;  // Round 14 Issue #16
uint32_t BurnerAntiFlapping::lastBurnerOnTime_ = 0;
uint32_t BurnerAntiFlapping::lastBurnerOffTime_ = 0;
uint32_t BurnerAntiFlapping::lastPowerChangeTime_ = 0;
float BurnerAntiFlapping::lastPIDOutput_ = 0.0f;
const char* BurnerAntiFlapping::TAG = "BurnerAntiFlap";

void BurnerAntiFlapping::initialize() {
    // Create mutex for thread-safe access
    if (mutex_ == nullptr) {
        mutex_ = xSemaphoreCreateMutex();
        if (mutex_ == nullptr) {
            LOG_ERROR(TAG, "Failed to create anti-flapping mutex");
        }
    }

    isBurnerOn_ = false;
    currentPowerLevel_ = PowerLevel::OFF;
    reservationPending_ = false;  // Round 14 Issue #16
    reservedLevel_ = PowerLevel::OFF;
    lastBurnerOnTime_ = 0;
    // Set lastBurnerOffTime_ far enough in the past to allow immediate startup
    // This avoids the 20s MIN_OFF_TIME delay on first boot
    uint32_t now = millis();
    lastBurnerOffTime_ = (now > SystemConstants::Burner::MIN_OFF_TIME_MS)
                         ? (now - SystemConstants::Burner::MIN_OFF_TIME_MS)
                         : 0;
    lastPowerChangeTime_ = 0;
    lastPIDOutput_ = 0.0f;

    LOG_INFO(TAG, "Anti-flapping initialized - MinOn:%lums MinOff:%lums PowerChange:%lums",
             SystemConstants::Burner::MIN_ON_TIME_MS,
             SystemConstants::Burner::MIN_OFF_TIME_MS,
             SystemConstants::Burner::MIN_POWER_CHANGE_INTERVAL_MS);
}

bool BurnerAntiFlapping::canTurnOn() {
    MutexGuard guard(mutex_, MUTEX_TIMEOUT);
    if (!guard.hasLock()) {
        LOG_WARN(TAG, "canTurnOn: mutex timeout - denying for safety");
        return false;  // Fail-safe: deny if can't get lock
    }

    if (isBurnerOn_) {
        return true; // Already on
    }

    uint32_t now = millis();
    uint32_t elapsedOff = now - lastBurnerOffTime_;

    bool allowed = elapsedOff >= SystemConstants::Burner::MIN_OFF_TIME_MS;

    if (!allowed) {
        [[maybe_unused]] uint32_t remaining = SystemConstants::Burner::MIN_OFF_TIME_MS - elapsedOff;
        LOG_DEBUG(TAG, "Cannot turn on yet - %lu ms remaining of minimum off time", remaining);
    }

    return allowed;
}

bool BurnerAntiFlapping::canTurnOff() {
    MutexGuard guard(mutex_, MUTEX_TIMEOUT);
    if (!guard.hasLock()) {
        LOG_WARN(TAG, "canTurnOff: mutex timeout - allowing for safety");
        return true;  // Fail-safe: allow OFF if can't get lock
    }

    if (!isBurnerOn_) {
        return true; // Already off
    }

    uint32_t now = millis();
    uint32_t elapsedOn = now - lastBurnerOnTime_;

    bool allowed = elapsedOn >= SystemConstants::Burner::MIN_ON_TIME_MS;

    if (!allowed) {
        [[maybe_unused]] uint32_t remaining = SystemConstants::Burner::MIN_ON_TIME_MS - elapsedOn;
        LOG_DEBUG(TAG, "Cannot turn off yet - %lu ms remaining of minimum on time", remaining);
    }

    return allowed;
}

bool BurnerAntiFlapping::canChangePowerLevel(PowerLevel newLevel) {
    MutexGuard guard(mutex_, MUTEX_TIMEOUT);
    if (!guard.hasLock()) {
        // Fail-safe: allow OFF, deny ON
        LOG_WARN(TAG, "canChangePowerLevel: mutex timeout");
        return (newLevel == PowerLevel::OFF);
    }

    if (currentPowerLevel_ == newLevel) {
        return true; // No change
    }

    uint32_t now = millis();

    // Allow turning off immediately for safety (inline canTurnOff logic)
    if (newLevel == PowerLevel::OFF) {
        if (!isBurnerOn_) {
            return true; // Already off
        }
        uint32_t elapsedOn = now - lastBurnerOnTime_;
        return elapsedOn >= SystemConstants::Burner::MIN_ON_TIME_MS;
    }

    // Allow initial turn on (inline canTurnOn logic)
    if (currentPowerLevel_ == PowerLevel::OFF && newLevel != PowerLevel::OFF) {
        if (isBurnerOn_) {
            return true; // Already on
        }
        uint32_t elapsedOff = now - lastBurnerOffTime_;
        return elapsedOff >= SystemConstants::Burner::MIN_OFF_TIME_MS;
    }

    // Check power level change interval for LOW<->HIGH changes
    uint32_t elapsedSinceChange = now - lastPowerChangeTime_;

    bool allowed = elapsedSinceChange >= SystemConstants::Burner::MIN_POWER_CHANGE_INTERVAL_MS;

    if (!allowed) {
        [[maybe_unused]] uint32_t remaining = SystemConstants::Burner::MIN_POWER_CHANGE_INTERVAL_MS - elapsedSinceChange;
        LOG_DEBUG(TAG, "Cannot change power level yet - %lu ms remaining", remaining);
    }

    return allowed;
}

// Round 14 Issue #16: Atomic check-and-reserve to prevent TOCTOU race
bool BurnerAntiFlapping::reservePowerLevelChange(PowerLevel newLevel) {
    MutexGuard guard(mutex_, MUTEX_TIMEOUT);
    if (!guard.hasLock()) {
        // Fail-safe: allow OFF, deny ON
        LOG_WARN(TAG, "reservePowerLevelChange: mutex timeout");
        return (newLevel == PowerLevel::OFF);
    }

    // Check if a reservation is already pending
    if (reservationPending_) {
        LOG_DEBUG(TAG, "Power level change already reserved (pending: %d), denying new reservation",
                 static_cast<int>(reservedLevel_));
        return false;
    }

    if (currentPowerLevel_ == newLevel) {
        return true; // No change needed
    }

    uint32_t now = millis();

    // Allow turning off immediately for safety
    if (newLevel == PowerLevel::OFF) {
        if (!isBurnerOn_) {
            return true;
        }
        uint32_t elapsedOn = now - lastBurnerOnTime_;
        if (elapsedOn >= SystemConstants::Burner::MIN_ON_TIME_MS) {
            reservationPending_ = true;
            reservedLevel_ = newLevel;
            LOG_DEBUG(TAG, "Reserved power level change to OFF");
            return true;
        }
        return false;
    }

    // Allow initial turn on
    if (currentPowerLevel_ == PowerLevel::OFF && newLevel != PowerLevel::OFF) {
        if (isBurnerOn_) {
            reservationPending_ = true;
            reservedLevel_ = newLevel;
            return true;
        }
        uint32_t elapsedOff = now - lastBurnerOffTime_;
        if (elapsedOff >= SystemConstants::Burner::MIN_OFF_TIME_MS) {
            reservationPending_ = true;
            reservedLevel_ = newLevel;
            LOG_DEBUG(TAG, "Reserved power level change to %s",
                     newLevel == PowerLevel::POWER_LOW ? "LOW" : "HIGH");
            return true;
        }
        return false;
    }

    // Check power level change interval for LOW<->HIGH changes
    uint32_t elapsedSinceChange = now - lastPowerChangeTime_;

    if (elapsedSinceChange >= SystemConstants::Burner::MIN_POWER_CHANGE_INTERVAL_MS) {
        reservationPending_ = true;
        reservedLevel_ = newLevel;
        LOG_DEBUG(TAG, "Reserved power level change: %s -> %s",
                 currentPowerLevel_ == PowerLevel::POWER_LOW ? "LOW" : "HIGH",
                 newLevel == PowerLevel::POWER_LOW ? "LOW" : "HIGH");
        return true;
    }

    return false;
}

void BurnerAntiFlapping::commitPowerLevelChange() {
    MutexGuard guard(mutex_, MUTEX_TIMEOUT);
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "commitPowerLevelChange: mutex timeout - state may be inconsistent");
        return;
    }

    if (!reservationPending_) {
        LOG_WARN(TAG, "commitPowerLevelChange: no reservation pending");
        return;
    }

    LOG_DEBUG(TAG, "Committing power level change to %s",
             reservedLevel_ == PowerLevel::OFF ? "OFF" :
             reservedLevel_ == PowerLevel::POWER_LOW ? "LOW" : "HIGH");

    // The actual state update is handled by recordPowerLevelChange
    // Just clear the reservation
    reservationPending_ = false;
}

void BurnerAntiFlapping::rollbackPowerLevelChange() {
    MutexGuard guard(mutex_, MUTEX_TIMEOUT);
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "rollbackPowerLevelChange: mutex timeout");
        return;
    }

    if (!reservationPending_) {
        LOG_DEBUG(TAG, "rollbackPowerLevelChange: no reservation pending");
        return;
    }

    LOG_INFO(TAG, "Rolling back power level reservation (was: %s)",
            reservedLevel_ == PowerLevel::OFF ? "OFF" :
            reservedLevel_ == PowerLevel::POWER_LOW ? "LOW" : "HIGH");

    reservationPending_ = false;
    reservedLevel_ = currentPowerLevel_;
}

void BurnerAntiFlapping::recordBurnerOn() {
    MutexGuard guard(mutex_, MUTEX_TIMEOUT);
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "recordBurnerOn: mutex timeout - state may be inconsistent");
        return;
    }

    if (!isBurnerOn_) {
        isBurnerOn_ = true;
        lastBurnerOnTime_ = millis();
        LOG_INFO(TAG, "Burner turned ON - minimum runtime enforced");
    }
}

void BurnerAntiFlapping::recordBurnerOff() {
    MutexGuard guard(mutex_, MUTEX_TIMEOUT);
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "recordBurnerOff: mutex timeout - state may be inconsistent");
        return;
    }

    if (isBurnerOn_) {
        isBurnerOn_ = false;
        lastBurnerOffTime_ = millis();
        currentPowerLevel_ = PowerLevel::OFF;
        uint32_t runtime = lastBurnerOffTime_ - lastBurnerOnTime_;
        LOG_INFO(TAG, "Burner turned OFF after %lu ms runtime", runtime);
    }
}

void BurnerAntiFlapping::recordPowerLevelChange(PowerLevel level) {
    MutexGuard guard(mutex_, MUTEX_TIMEOUT);
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "recordPowerLevelChange: mutex timeout - state may be inconsistent");
        return;
    }

    if (currentPowerLevel_ != level) {
        PowerLevel oldLevel = currentPowerLevel_;
        currentPowerLevel_ = level;
        lastPowerChangeTime_ = millis();

        const char* levelStr = (level == PowerLevel::OFF) ? "OFF" :
                              (level == PowerLevel::POWER_LOW) ? "LOW" : "HIGH";
        LOG_INFO(TAG, "Power level changed: %s -> %s",
                (oldLevel == PowerLevel::OFF) ? "OFF" :
                (oldLevel == PowerLevel::POWER_LOW) ? "LOW" : "HIGH",
                levelStr);

        // Record on/off transitions (inline to avoid mutex deadlock)
        if (level == PowerLevel::OFF && oldLevel != PowerLevel::OFF) {
            // Inline recordBurnerOff
            if (isBurnerOn_) {
                isBurnerOn_ = false;
                lastBurnerOffTime_ = millis();
                uint32_t runtime = lastBurnerOffTime_ - lastBurnerOnTime_;
                LOG_INFO(TAG, "Burner turned OFF after %lu ms runtime", runtime);
            }
        } else if (level != PowerLevel::OFF && oldLevel == PowerLevel::OFF) {
            // Inline recordBurnerOn
            if (!isBurnerOn_) {
                isBurnerOn_ = true;
                lastBurnerOnTime_ = millis();
                LOG_INFO(TAG, "Burner turned ON - minimum runtime enforced");
            }
        }
    }
}

uint32_t BurnerAntiFlapping::getTimeUntilCanTurnOn() {
    MutexGuard guard(mutex_, MUTEX_TIMEOUT);
    if (!guard.hasLock()) {
        return 0;  // Return 0 (allowed) on mutex failure
    }

    if (isBurnerOn_) {
        return 0;
    }

    // Round 16 Issue #8: Use Utils::elapsedMs() for safe wraparound handling
    uint32_t elapsedOff = Utils::elapsedMs(lastBurnerOffTime_);

    if (elapsedOff >= SystemConstants::Burner::MIN_OFF_TIME_MS) {
        return 0;
    }

    return SystemConstants::Burner::MIN_OFF_TIME_MS - elapsedOff;
}

uint32_t BurnerAntiFlapping::getTimeUntilCanTurnOff() {
    MutexGuard guard(mutex_, MUTEX_TIMEOUT);
    if (!guard.hasLock()) {
        return 0;  // Return 0 (allowed) on mutex failure
    }

    if (!isBurnerOn_) {
        return 0;
    }

    // Round 16 Issue #8: Use Utils::elapsedMs() for safe wraparound handling
    uint32_t elapsedOn = Utils::elapsedMs(lastBurnerOnTime_);

    if (elapsedOn >= SystemConstants::Burner::MIN_ON_TIME_MS) {
        return 0;
    }

    return SystemConstants::Burner::MIN_ON_TIME_MS - elapsedOn;
}

uint32_t BurnerAntiFlapping::getTimeUntilCanChangePower() {
    MutexGuard guard(mutex_, MUTEX_TIMEOUT);
    if (!guard.hasLock()) {
        return 0;  // Return 0 (allowed) on mutex failure
    }

    // Round 16 Issue #8: Use Utils::elapsedMs() for safe wraparound handling
    uint32_t elapsedSinceChange = Utils::elapsedMs(lastPowerChangeTime_);

    if (elapsedSinceChange >= SystemConstants::Burner::MIN_POWER_CHANGE_INTERVAL_MS) {
        return 0;
    }

    return SystemConstants::Burner::MIN_POWER_CHANGE_INTERVAL_MS - elapsedSinceChange;
}

bool BurnerAntiFlapping::isSignificantPIDChange(float currentOutput, float newOutput) {
    float change = std::abs(newOutput - currentOutput);
    bool significant = change > SystemConstants::Burner::PID_OUTPUT_DEADBAND;
    
    if (!significant) {
        LOG_DEBUG(TAG, "PID change %.1f -> %.1f within deadband (±%.1f), ignoring",
                 currentOutput, newOutput, SystemConstants::Burner::PID_OUTPUT_DEADBAND);
    }
    
    return significant;
}

BurnerAntiFlapping::PowerLevel BurnerAntiFlapping::stateToPowerLevel(BurnerSMState state) {
    switch (state) {
        case BurnerSMState::IDLE:
        case BurnerSMState::PRE_PURGE:
        case BurnerSMState::POST_PURGE:
        case BurnerSMState::LOCKOUT:
        case BurnerSMState::ERROR:
            return PowerLevel::OFF;
            
        case BurnerSMState::RUNNING_LOW:
            return PowerLevel::POWER_LOW;
            
        case BurnerSMState::RUNNING_HIGH:
            return PowerLevel::POWER_HIGH;
            
        case BurnerSMState::IGNITION:
            // Not used - IGNITION is skipped in transition callback
            // Actual power recorded in onEnterIgnition() based on request
            return PowerLevel::POWER_LOW;
            
        default:
            return PowerLevel::OFF;
    }
}

void BurnerAntiFlapping::reset() {
    MutexGuard guard(mutex_, MUTEX_TIMEOUT);
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "reset: mutex timeout - forcing reset anyway");
    }

    LOG_WARN(TAG, "Resetting anti-flapping state");
    isBurnerOn_ = false;
    currentPowerLevel_ = PowerLevel::OFF;
    reservationPending_ = false;  // Round 14 Issue #16
    reservedLevel_ = PowerLevel::OFF;
    lastBurnerOnTime_ = 0;
    lastBurnerOffTime_ = millis();
    lastPowerChangeTime_ = 0;
    lastPIDOutput_ = 0.0f;
}