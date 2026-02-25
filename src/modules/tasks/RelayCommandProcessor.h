// src/modules/tasks/RelayCommandProcessor.h
#ifndef RELAY_COMMAND_PROCESSOR_H
#define RELAY_COMMAND_PROCESSOR_H

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

// Forward declaration
class RelayControlTask;

/**
 * @brief Relay command request processing
 *
 * Extracted from RelayControlTask.cpp (Round 21 Refactoring).
 * Handles processing of relay request events from event groups.
 *
 * Thread Safety:
 * - Safe to call from RelayControlTask context
 * - Uses event groups for inter-task communication
 */
class RelayCommandProcessor {
public:
    /**
     * @brief Process pending relay request events
     * @param relayRequestEventGroup Event group handle for relay requests
     * @param setRelayStateFunc Callback function to set relay state
     *
     * Processes all pending relay request bits from the event group:
     * - HEATING_PUMP_ON/OFF
     * - WATER_PUMP_ON/OFF
     * - BURNER_ENABLE/DISABLE
     * - POWER_HALF/FULL
     * - WATER_MODE_ON/OFF
     *
     * Clears event bits after processing (edge-triggered behavior).
     * Blocked requests (by pump protection) are logged for retry.
     */
    static void processRelayRequests(
        EventGroupHandle_t relayRequestEventGroup,
        bool (*setRelayStateFunc)(uint8_t relayIndex, bool state)
    );
};

#endif // RELAY_COMMAND_PROCESSOR_H
