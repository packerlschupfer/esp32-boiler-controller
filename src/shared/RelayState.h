// src/shared/RelayState.h
// Shared relay state for coordinated Modbus operations
#pragma once

#include <atomic>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

/**
 * @brief Shared relay state between RelayControlTask and RYN4Task
 *
 * RelayControlTask updates desired state, RYN4Task handles Modbus operations.
 * All operations are scheduled via ModbusCoordinator to prevent bus contention.
 */
struct RelayState {
    std::atomic<uint8_t> desired{0};       // Desired relay states (bitmask) - what app wants
    std::atomic<uint8_t> sent{0};          // Last sent to hardware (bitmask) - what we commanded
    std::atomic<uint8_t> actual{0};        // Last verified actual states - what hardware reports
    std::atomic<bool> pendingWrite{false}; // True if changes need to be sent
    std::atomic<uint8_t> consecutiveMismatches{0};  // Verification retry counter

    // DELAY command tracking (for hardware DELAY 0x06XX commands)
    std::atomic<uint8_t> delayMask{0};     // Bitmask of relays with active DELAY
    uint32_t delayExpiry[8] = {0};         // Expiration timestamp for each relay (milliseconds)
    SemaphoreHandle_t delayMutex = nullptr; // Protects delayExpiry array

    // DELAY tracking methods
    void setDelayCommand(uint8_t relay, uint8_t delaySeconds);
    void clearDelay(uint8_t relay);
    bool isDelayActive(uint8_t relay) const;

    // Helper methods for relay manipulation
    void setRelay(uint8_t relay, bool on) {
        uint8_t current = desired.load(std::memory_order_relaxed);
        if (on) {
            current |= (1 << relay);
        } else {
            current &= ~(1 << relay);
        }
        desired.store(current, std::memory_order_release);
        pendingWrite.store(true, std::memory_order_release);
        consecutiveMismatches.store(0, std::memory_order_release);
        clearDelay(relay);  // Clear DELAY when manually controlling relay
    }

    void setAllRelays(uint8_t states) {
        desired.store(states, std::memory_order_release);
        pendingWrite.store(true, std::memory_order_release);
        consecutiveMismatches.store(0, std::memory_order_release);
        // Clear all delays
        for (uint8_t i = 0; i < 8; i++) {
            clearDelay(i);
        }
    }

    void setAllOff() {
        desired.store(0, std::memory_order_release);
        pendingWrite.store(true, std::memory_order_release);
        consecutiveMismatches.store(0, std::memory_order_release);
        // Clear all delays
        for (uint8_t i = 0; i < 8; i++) {
            clearDelay(i);
        }
    }

    bool getRelay(uint8_t relay) const {
        return (desired.load(std::memory_order_acquire) & (1 << relay)) != 0;
    }

    bool getActualRelay(uint8_t relay) const {
        return (actual.load(std::memory_order_acquire) & (1 << relay)) != 0;
    }

    bool hasMismatch() const {
        return sent.load(std::memory_order_acquire) !=
               actual.load(std::memory_order_acquire);
    }

    bool hasPendingChanges() const {
        return desired.load(std::memory_order_acquire) !=
               sent.load(std::memory_order_acquire);
    }
};

// Global relay state instance
extern RelayState g_relayState;

// Initialize relay state (must be called during system initialization)
void initRelayState();
