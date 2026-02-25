// src/modules/tasks/RelayCommandProcessor.cpp
#include "RelayCommandProcessor.h"
#include "config/RelayIndices.h"
#include "core/SystemResourceProvider.h"
#include "core/SharedResourceManager.h"
#include "events/SystemEventsGenerated.h"
#include <esp_log.h>

static const char* TAG = "RelayCmdProc";

void RelayCommandProcessor::processRelayRequests(
    EventGroupHandle_t relayRequestEventGroup,
    bool (*setRelayStateFunc)(uint8_t relayIndex, bool state)
) {
    if (!relayRequestEventGroup) {
        LOG_ERROR(TAG, "Failed to get relay request event group!");
        return;
    }

    // Get current state of all request bits without waiting
    // Caller already waited in waitForRelayRequests(), so just get current state
    EventBits_t requestBits = xEventGroupGetBits(relayRequestEventGroup);

    // Mask to only process valid bits (FreeRTOS event groups only support 24 bits)
    const EventBits_t ALL_RELAY_REQUEST_BITS = 0x00FFFFFF;  // Mask for bits 0-23
    requestBits &= ALL_RELAY_REQUEST_BITS;

    if (requestBits == 0) {
        return;  // No requests pending
    }

    LOG_DEBUG(TAG, "processRelayRequests: Got request bits: 0x%08X", requestBits);

    // Process heating pump requests
    if (requestBits & SystemEvents::RelayRequest::HEATING_PUMP_ON) {
        LOG_DEBUG(TAG, "Processing heating pump ON request");
        bool success = setRelayStateFunc(RelayIndex::toPhysical(RelayIndex::HEATING_PUMP), true);
        // Clear bit unconditionally - event bits are edge-triggered, not level
        xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::HEATING_PUMP_ON);
        if (!success) {
            LOG_DEBUG(TAG, "Heating pump ON blocked by protection (will retry on next request)");
        }
    }

    if (requestBits & SystemEvents::RelayRequest::HEATING_PUMP_OFF) {
        LOG_DEBUG(TAG, "Processing heating pump OFF request");
        bool success = setRelayStateFunc(RelayIndex::toPhysical(RelayIndex::HEATING_PUMP), false);
        // Clear bit unconditionally - event bits are edge-triggered, not level
        xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::HEATING_PUMP_OFF);
        if (!success) {
            LOG_DEBUG(TAG, "Heating pump OFF blocked by protection (will retry on next request)");
        }
    }

    // Process water pump requests
    if (requestBits & SystemEvents::RelayRequest::WATER_PUMP_ON) {
        LOG_INFO(TAG, "Processing water pump ON request for relay %d", RelayIndex::toPhysical(RelayIndex::WATER_PUMP));

        bool success = setRelayStateFunc(RelayIndex::toPhysical(RelayIndex::WATER_PUMP), true);
        // Clear bit unconditionally - event bits are edge-triggered, not level
        xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::WATER_PUMP_ON);
        if (success) {
            LOG_INFO(TAG, "Water pump ON request completed");
        } else {
            LOG_DEBUG(TAG, "Water pump ON blocked by protection (will retry on next request)");
        }
    }

    if (requestBits & SystemEvents::RelayRequest::WATER_PUMP_OFF) {
        LOG_DEBUG(TAG, "Processing water pump OFF request");
        bool success = setRelayStateFunc(RelayIndex::toPhysical(RelayIndex::WATER_PUMP), false);
        // Clear bit unconditionally - event bits are edge-triggered, not level
        xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::WATER_PUMP_OFF);
        if (!success) {
            LOG_DEBUG(TAG, "Water pump OFF blocked by protection (will retry on next request)");
        }
    }

    // Process burner requests
    if (requestBits & SystemEvents::RelayRequest::BURNER_ENABLE) {
        LOG_DEBUG(TAG, "Processing burner ON request");
        bool success = setRelayStateFunc(RelayIndex::toPhysical(RelayIndex::BURNER_ENABLE), true);
        // Clear bit unconditionally - event bits are edge-triggered, not level
        xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::BURNER_ENABLE);
        if (!success) {
            LOG_DEBUG(TAG, "Burner ON blocked by protection (will retry on next request)");
        }
    }

    if (requestBits & SystemEvents::RelayRequest::BURNER_DISABLE) {
        LOG_DEBUG(TAG, "Processing burner OFF request");
        bool success = setRelayStateFunc(RelayIndex::toPhysical(RelayIndex::BURNER_ENABLE), false);
        // Clear bit unconditionally - event bits are edge-triggered, not level
        xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::BURNER_DISABLE);
        if (!success) {
            LOG_DEBUG(TAG, "Burner OFF blocked by protection (will retry on next request)");
        }
    }

    // Process power boost requests
    if (requestBits & SystemEvents::RelayRequest::POWER_HALF) {
        LOG_DEBUG(TAG, "Processing half power request");
        bool success = setRelayStateFunc(RelayIndex::toPhysical(RelayIndex::POWER_BOOST), false);
        // Clear bit unconditionally - event bits are edge-triggered, not level
        xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::POWER_HALF);
        if (!success) {
            LOG_DEBUG(TAG, "Power HALF blocked by protection (will retry on next request)");
        }
    }

    if (requestBits & SystemEvents::RelayRequest::POWER_FULL) {
        LOG_DEBUG(TAG, "Processing full power request");
        bool success = setRelayStateFunc(RelayIndex::toPhysical(RelayIndex::POWER_BOOST), true);
        // Clear bit unconditionally - event bits are edge-triggered, not level
        xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::POWER_FULL);
        if (!success) {
            LOG_DEBUG(TAG, "Power FULL blocked by protection (will retry on next request)");
        }
    }

    // Process water mode requests
    if (requestBits & SystemEvents::RelayRequest::WATER_MODE_ON) {
        LOG_DEBUG(TAG, "Processing water mode ON request");
        bool success = setRelayStateFunc(RelayIndex::toPhysical(RelayIndex::WATER_MODE), true);
        // Clear bit unconditionally - event bits are edge-triggered, not level
        xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::WATER_MODE_ON);
        if (!success) {
            LOG_DEBUG(TAG, "Water mode ON blocked by protection (will retry on next request)");
        }
    }

    if (requestBits & SystemEvents::RelayRequest::WATER_MODE_OFF) {
        LOG_DEBUG(TAG, "Processing water mode OFF request");
        bool success = setRelayStateFunc(RelayIndex::toPhysical(RelayIndex::WATER_MODE), false);
        // Clear bit unconditionally - event bits are edge-triggered, not level
        xEventGroupClearBits(relayRequestEventGroup, SystemEvents::RelayRequest::WATER_MODE_OFF);
        if (!success) {
            LOG_DEBUG(TAG, "Water mode OFF blocked by protection (will retry on next request)");
        }
    }
}
