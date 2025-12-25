// src/modules/tasks/RelayVerificationManager.cpp
#include "RelayVerificationManager.h"
#include "config/RelayIndices.h"
#include "config/SafetyConfig.h"
#include "core/SystemResourceProvider.h"
#include "events/SystemEventsGenerated.h"
#include "modules/control/CentralizedFailsafe.h"
#include <esp_log.h>

static const char* TAG = "RelayVerifyMgr";

bool RelayVerificationManager::checkPumpProtection(
    uint8_t relayIndex,
    bool desiredState,
    const bool* relayStates,
    bool relayStatesKnown,
    TickType_t* pumpLastChangeTime
) {
    // Only applies to pump relays (physical 5=heating pump, physical 6=water pump)
    const uint8_t heatingPumpPhysical = RelayIndex::toPhysical(RelayIndex::HEATING_PUMP);  // 5
    const uint8_t waterPumpPhysical = RelayIndex::toPhysical(RelayIndex::WATER_PUMP);      // 6

    uint8_t pumpIdx;
    if (relayIndex == heatingPumpPhysical) {
        pumpIdx = 0;  // Heating pump
    } else if (relayIndex == waterPumpPhysical) {
        pumpIdx = 1;  // Water pump
    } else {
        return true;  // Not a pump relay, no protection needed
    }

    TickType_t now = xTaskGetTickCount();

    // If this is the first state change (timestamp is 0), allow it
    if (pumpLastChangeTime[pumpIdx] == 0) {
        return true;
    }

    // Check if relay is already in desired state - no protection needed
    if (relayStatesKnown) {
        bool currentState = relayStates[relayIndex - 1];
        if (currentState == desiredState) {
            return true;  // No actual state change, protection doesn't apply
        }
    }

    // Calculate elapsed time since last state change
    TickType_t elapsed = now - pumpLastChangeTime[pumpIdx];
    uint32_t elapsedMs = pdTICKS_TO_MS(elapsed);

    if (elapsedMs < SafetyConfig::pumpProtectionMs) {
        // Block the state change - motor protection
        uint32_t remainingMs = SafetyConfig::pumpProtectionMs - elapsedMs;

        static TickType_t lastBlockLog[2] = {0, 0};
        if (now - lastBlockLog[pumpIdx] > pdMS_TO_TICKS(5000)) {  // Log every 5s max
            LOG_WARN(TAG, "Pump %d state change blocked by motor protection - %lu ms remaining",
                     relayIndex, remainingMs);
            lastBlockLog[pumpIdx] = now;
        }
        return false;
    }

    return true;  // Protection period has elapsed, allow state change
}

uint32_t RelayVerificationManager::getPumpProtectionTimeRemaining(
    uint8_t relayIndex,
    const TickType_t* pumpLastChangeTime
) {
    // Only applies to pump relays (physical 5=heating pump, physical 6=water pump)
    const uint8_t heatingPumpPhysical = RelayIndex::toPhysical(RelayIndex::HEATING_PUMP);  // 5
    const uint8_t waterPumpPhysical = RelayIndex::toPhysical(RelayIndex::WATER_PUMP);      // 6

    uint8_t pumpIdx;
    if (relayIndex == heatingPumpPhysical) {
        pumpIdx = 0;  // Heating pump
    } else if (relayIndex == waterPumpPhysical) {
        pumpIdx = 1;  // Water pump
    } else {
        return 0;  // Not a pump relay
    }

    if (pumpLastChangeTime[pumpIdx] == 0) {
        return 0;  // No protection active yet
    }

    TickType_t now = xTaskGetTickCount();
    TickType_t elapsed = now - pumpLastChangeTime[pumpIdx];
    uint32_t elapsedMs = pdTICKS_TO_MS(elapsed);

    if (elapsedMs >= SafetyConfig::pumpProtectionMs) {
        return 0;  // Protection period has elapsed
    }

    return SafetyConfig::pumpProtectionMs - elapsedMs;
}

void RelayVerificationManager::checkRelayHealthAndEscalate(
    uint8_t relayIndex,
    bool success,
    uint8_t* consecutiveFailures
) {
    if (relayIndex < 1 || relayIndex > 8) {
        return;
    }

    uint8_t idx = relayIndex - 1;

    if (success) {
        // Reset consecutive failure counter on success
        if (consecutiveFailures[idx] > 0) {
            LOG_INFO(TAG, "Relay %d recovered after %d failures", relayIndex, consecutiveFailures[idx]);
            consecutiveFailures[idx] = 0;
        }
    } else {
        // Increment failure counter
        consecutiveFailures[idx]++;

        LOG_WARN(TAG, "Relay %d consecutive failures: %d/%d",
                 relayIndex, consecutiveFailures[idx], MAX_CONSECUTIVE_FAILURES);

        // Check if escalation threshold reached
        if (consecutiveFailures[idx] >= MAX_CONSECUTIVE_FAILURES) {
            LOG_ERROR(TAG, "CRITICAL: Relay %d failed %d consecutive times - escalating to failsafe",
                     relayIndex, consecutiveFailures[idx]);

            // Set relay error bit
            xEventGroupSetBits(SRP::getErrorNotificationEventGroup(), SystemEvents::Error::RELAY);

            // FMEA Round 6: Use CRITICAL level for burner relay (triggers emergency shutdown)
            // Other relays use WARNING level (triggers monitoring only)
            // idx is array index (0-7), RelayIndex::BURNER_ENABLE is array index 0
            CentralizedFailsafe::FailsafeLevel level =
                (idx == RelayIndex::BURNER_ENABLE)
                    ? CentralizedFailsafe::FailsafeLevel::CRITICAL
                    : CentralizedFailsafe::FailsafeLevel::WARNING;

            CentralizedFailsafe::triggerFailsafe(
                level,
                SystemError::RELAY_OPERATION_FAILED,
                (idx == RelayIndex::BURNER_ENABLE)
                    ? "BURNER relay verification failed - emergency shutdown"
                    : "Relay verification failed repeatedly"
            );

            // Reset counter to avoid repeated escalations
            // Next failure will start counting again
            consecutiveFailures[idx] = 0;
        }
    }
}
