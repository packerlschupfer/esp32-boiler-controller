// src/shared/RelayState.cpp
#include "shared/RelayState.h"
#include "LoggingMacros.h"
#include <Arduino.h>  // For millis()

static const char* TAG = "RelayState";

// Global relay state instance
RelayState g_relayState;

// Initialize the delay mutex (called from SystemInitializer)
void initRelayState() {
    g_relayState.delayMutex = xSemaphoreCreateMutex();
    if (!g_relayState.delayMutex) {
        LOG_ERROR(TAG, "Failed to create delay mutex");
    } else {
        LOG_INFO(TAG, "Delay tracking initialized");
    }
}

// Track DELAY command for a relay
void RelayState::setDelayCommand(uint8_t relay, uint8_t delaySeconds) {
    if (relay >= 8) return;

    if (xSemaphoreTake(delayMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        uint32_t now = millis();
        delayExpiry[relay] = now + (delaySeconds * 1000);
        delayMask.fetch_or(1 << relay, std::memory_order_release);
        xSemaphoreGive(delayMutex);
        LOG_DEBUG(TAG, "DELAY set for relay %d: %d seconds (expires at %lu)",
                  relay + 1, delaySeconds, delayExpiry[relay]);
    }
}

// Clear DELAY tracking for a relay
void RelayState::clearDelay(uint8_t relay) {
    if (relay >= 8) return;

    if (xSemaphoreTake(delayMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        delayExpiry[relay] = 0;
        delayMask.fetch_and(~(1 << relay), std::memory_order_release);
        xSemaphoreGive(delayMutex);
        LOG_DEBUG(TAG, "DELAY cleared for relay %d", relay + 1);
    }
}

// Check if DELAY is still active for a relay
bool RelayState::isDelayActive(uint8_t relay) const {
    if (relay >= 8) return false;

    // Quick check without mutex (atomic)
    if (!(delayMask.load(std::memory_order_acquire) & (1 << relay))) {
        return false;
    }

    // Check expiration time (needs mutex)
    bool active = false;
    if (xSemaphoreTake(delayMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        uint32_t now = millis();
        active = (delayExpiry[relay] > now);
        xSemaphoreGive(delayMutex);
    }

    return active;
}
