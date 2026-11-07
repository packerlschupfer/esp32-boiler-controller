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
     * Processes pending relay request bits from the event group. Clears each bit after
     * processing (edge-triggered). Blocked requests (by pump protection) are logged for retry.
     *
     * ONLY THE FOUR PUMP BITS ARE EVER SENT. The list below used to read as six live
     * capabilities; it is not, and the difference matters to anyone reconstructing what this
     * controller can be asked to do (2026-11-07):
     *
     *   HEATING_PUMP_ON/OFF   LIVE   - PumpControlModule is the only writer
     *   WATER_PUMP_ON/OFF     LIVE   - likewise
     *   BURNER_ENABLE/DISABLE  never sent
     *   POWER_HALF/FULL        never sent
     *   WATER_MODE_ON/OFF      never sent
     *
     * Verified by enumeration rather than by grepping the constant names, because a name grep
     * would miss a bit set through a variable or a mask: exactly three files touch
     * EventGroups::RELAY_REQUEST - SystemInitializer creates it, PumpControlModule writes it,
     * RelayControlTask reads it - PumpControlModule writes only config->relayOnRequestBit /
     * relayOffRequestBit, and there are exactly two PumpConfig instances, both carrying pump
     * bits. The generic SRP::setRelayEventBits() writes a DIFFERENT group (RelayControl,
     * DATA_AVAILABLE) and cannot reach this one.
     *
     * The burner, boost and water-mode relays are driven directly by BurnerSystemController
     * via RelayControlTask::setRelayState, NOT through this arbiter. So the system has two
     * parallel relay-write paths and this one arbitrates only the pumps. The uniform
     * request-bit protocol in SystemEventsGenerated.h (which also declares VALVE_* and ALARM_*,
     * handled nowhere at all) was only ever half adopted.
     *
     * The six branches are KEPT, not deleted: they are the right plumbing if manual relay
     * control is ever added behind a test mode, and the arbiter is where interlocks would
     * belong. But they are dormant, and this comment should say so - reading it as a list of
     * live capabilities is how a manual-override feature gets believed into existence that
     * was never wired. See also BurnerBoostPolicy.h: h/boostMin has exactly TWO exemptions
     * (water, autotune), not three, for the same reason.
     */
    static void processRelayRequests(
        EventGroupHandle_t relayRequestEventGroup,
        bool (*setRelayStateFunc)(uint8_t relayIndex, bool state)
    );
};

#endif // RELAY_COMMAND_PROCESSOR_H
