// src/modules/scheduler/SpaceHeatingScheduleAction.cpp
// Space heating schedule action handler implementation
#include "modules/scheduler/SpaceHeatingScheduleAction.h"
#include "modules/tasks/PersistentStorageTask.h"  // SetRoomTarget, GetDurableRoomTarget
#include "TimerSchedule.h"
#include "core/SystemResourceProvider.h"
#include "shared/Temperature.h"
#include "shared/SharedSensorReadings.h"
#include "LoggingMacros.h"
#include "config/SystemSettings.h"
#include "config/SystemConstants.h"
#include "events/SystemEventsGenerated.h"

static const char* TAG = "SpaceHeatingAction";

// Heating modes
enum HeatingMode : uint8_t {
    MODE_COMFORT = 0,         // Normal comfort temperature
    MODE_ECO = 1,             // Energy saving temperature
    MODE_FROST_PROTECT = 2    // Minimum temperature to prevent freezing
};

SpaceHeatingScheduleAction::SpaceHeatingScheduleAction() {
    LOG_INFO(TAG, "Space heating schedule action handler initialized");
}

void SpaceHeatingScheduleAction::onScheduleStart(const TimerSchedule& schedule) {
    LOG_INFO(TAG, "Schedule '%s' starting - mode: %s, target: %d°C",
             schedule.name.c_str(),
             schedule.actionData.spaceHeating.mode == MODE_COMFORT ? "COMFORT" :
             schedule.actionData.spaceHeating.mode == MODE_ECO ? "ECO" : "FROST",
             schedule.actionData.spaceHeating.targetTempC);
    
    // NO PRE-WINDOW CAPTURE ANY MORE. This used to stash the live target in a member so
    // the window end could restore it. That cache was one slot per schedule TYPE, so
    // back-to-back or overlapping windows clobbered it; it did not survive a reboot
    // inside a window; and restoring it overwrote any durable params/set the owner made
    // DURING the window. The window end now reads the persisted target instead - which is
    // what "back to the configured value" actually means, needs no state, and is correct
    // across all three of those cases. Found by the 2026-10-30 diff review.

    // Apply heating mode
    applyHeatingMode(schedule.actionData.spaceHeating.mode, 
                    schedule.actionData.spaceHeating.targetTempC);
    
    // Update active schedule info
    activeScheduleName_ = schedule.name;
    activeMode_ = schedule.actionData.spaceHeating.mode;
    isPreheating_ = false;
}

void SpaceHeatingScheduleAction::onScheduleEnd(const TimerSchedule& schedule) {
    LOG_INFO(TAG, "Schedule '%s' ending", schedule.name.c_str());
    
    // RESTORE THE PRE-WINDOW TARGET instead of parking at DEFAULT_FROST_TEMP (10.0 C).
    //
    // applyHeatingMode(MODE_FROST_PROTECT, 0) wrote 10.0 into the live room target and left it
    // there, so between windows the curve and the room backstop both followed 10.0 and the
    // controller withheld heat until the house fell to ~11.5-12 C. For the documented
    // Mon-Fri 18:00-22:00 example that is every night and the whole weekend. It also became
    // durable by accident via the pre-save shadow sync, so a reboot's setpoint depended on
    // whether it happened inside a window.
    //
    // Nothing documented says a window end means "let the house go to 10 C" - ECO mode exists
    // for a configurable setback.
    //
    // Restore the PERSISTED target: the value params/set last wrote, which is what the
    // house is configured to run at outside any window. Durable, because live now equals
    // the stored value, so that is precisely the not-overridden state. Note this
    // deliberately does NOT restore a temporary cmd/room_target override that was in
    // force before the window - a temporary override is documented as temporary, and
    // remembering its durability across a window boundary would put back the per-type
    // cached state this change removes.
    const Temperature_t durable = PersistentStorageTask_GetDurableRoomTarget();
    if (durable > 0) {
        if (PersistentStorageTask_SetRoomTarget(durable, RoomTargetDurability::Durable)) {
            char prevBuf[16];
            formatTemp(prevBuf, sizeof(prevBuf), durable);
            LOG_INFO(TAG, "Window ended - room target restored to the configured %s°C", prevBuf);
        } else {
            LOG_ERROR(TAG, "Window ended but the settings mutex was busy - room target left "
                           "at the window value, NOT parked at 10 C");
        }
    } else {
        LOG_WARN(TAG, "Window ended before any room target was loaded from NVS - leaving the "
                      "target as the schedule set it");
    }

    // Clear active schedule info
    activeScheduleName_ = "";
    activeMode_ = MODE_FROST_PROTECT;
    isPreheating_ = false;
}

void SpaceHeatingScheduleAction::onPreheatingStart(const TimerSchedule& schedule, uint32_t minutesUntilStart) {
    LOG_INFO(TAG, "Starting preheating for schedule '%s' - %lu minutes until start",
             schedule.name.c_str(), minutesUntilStart);

    // Get current room temperature (fixed-point)
    Temperature_t currentTemp = tempFromWhole(20);  // Default 20.0°C if sensor unavailable

    if (SRP::takeSensorReadingsMutex(pdMS_TO_TICKS(100))) {
        currentTemp = SRP::getSensorReadings().insideTemp;
        SRP::giveSensorReadingsMutex();
    }

    // Get target temperature (fixed-point)
    Temperature_t targetTemp = tempFromWhole(schedule.actionData.spaceHeating.targetTempC);
    if (targetTemp == 0) {
        using namespace SystemConstants::Temperature::SpaceHeating;
        // Use default based on mode
        switch (schedule.actionData.spaceHeating.mode) {
            case MODE_COMFORT:
                targetTemp = DEFAULT_COMFORT_TEMP;
                break;
            case MODE_ECO:
                targetTemp = DEFAULT_ECO_TEMP;
                break;
            case MODE_FROST_PROTECT:
                targetTemp = DEFAULT_FROST_TEMP;
                break;
        }
    }

    Temperature_t tempRise = tempSub(targetTemp, currentTemp);

    // Only preheat if we need to raise temperature (>1.0°C)
    if (tempGreater(tempRise, 10)) {  // 10 = 1.0°C in fixed-point
        LOG_INFO(TAG, "Preheating needed: current %d.%d°C -> target %d.%d°C (%d.%d°C rise)",
                 currentTemp / 10, abs(currentTemp % 10),
                 targetTemp / 10, abs(targetTemp % 10),
                 tempRise / 10, abs(tempRise % 10));

        // Start heating with comfort mode to warm up quickly
        applyHeatingMode(MODE_COMFORT, schedule.actionData.spaceHeating.targetTempC);

        isPreheating_ = true;
        activeScheduleName_ = schedule.name + " (preheating)";
    } else {
        LOG_INFO(TAG, "No preheating needed - room already at temperature");
    }
}

bool SpaceHeatingScheduleAction::needsPreheating() const {
    // Space heating benefits from preheating to ensure comfortable temperature
    return true;
}

uint32_t SpaceHeatingScheduleAction::getPreheatingMinutes() const {
    using namespace SystemConstants::Temperature::SpaceHeating;

    // Calculate preheat time based on typical heating rate
    // HEATING_RATE_PER_HOUR = 20 (2.0°C/hour in fixed-point)
    // Assume we need to heat from 15°C to 21°C = 60 (6.0°C) rise in fixed-point
    const Temperature_t TEMP_RISE_TYPICAL = 60;  // 6.0°C in tenths

    // Convert: minutes = (tempRise * 60) / heatingRatePerHour
    // Example: (60 * 60) / 20 = 180 minutes
    uint32_t preheatMinutes = (TEMP_RISE_TYPICAL * 60) / HEATING_RATE_PER_HOUR;
    
    // Cap at reasonable maximum
    if (preheatMinutes > 180) {
        preheatMinutes = 180;  // 3 hours max
    }
    
    LOG_DEBUG(TAG, "Preheat time: %lu minutes", preheatMinutes);
    return preheatMinutes;
}

const char* SpaceHeatingScheduleAction::getTypeName() const {
    return "Space Heating";
}

ScheduleType SpaceHeatingScheduleAction::getType() const {
    return ScheduleType::SPACE_HEATING;
}

size_t SpaceHeatingScheduleAction::serializeActionData(const TimerSchedule& schedule, 
                                                       uint8_t* buffer, size_t size) const {
    if (size < 3) return 0;
    
    buffer[0] = schedule.actionData.spaceHeating.targetTempC;
    buffer[1] = schedule.actionData.spaceHeating.mode;
    buffer[2] = schedule.actionData.spaceHeating.zones;
    
    return 3;
}

bool SpaceHeatingScheduleAction::deserializeActionData(TimerSchedule& schedule, 
                                                      const uint8_t* buffer, size_t size) {
    if (size < 3) return false;
    
    schedule.actionData.spaceHeating.targetTempC = buffer[0];
    schedule.actionData.spaceHeating.mode = buffer[1];
    schedule.actionData.spaceHeating.zones = buffer[2];
    
    return true;
}

const String& SpaceHeatingScheduleAction::getActiveScheduleName() const {
    return activeScheduleName_;
}

bool SpaceHeatingScheduleAction::isPreheating() const {
    return isPreheating_;
}

uint8_t SpaceHeatingScheduleAction::getActiveMode() const {
    return activeMode_;
}

void SpaceHeatingScheduleAction::applyHeatingMode(uint8_t mode, uint8_t targetTempC) {
    using namespace SystemConstants::Temperature::SpaceHeating;

    // Get the appropriate temperature for the mode (fixed-point)
    Temperature_t targetTemp = tempFromWhole(targetTempC);

    if (targetTemp == 0) {
        // Use default temperature for the mode
        switch (mode) {
            case MODE_COMFORT:
                targetTemp = DEFAULT_COMFORT_TEMP;
                break;
            case MODE_ECO:
                targetTemp = DEFAULT_ECO_TEMP;
                break;
            case MODE_FROST_PROTECT:
                targetTemp = DEFAULT_FROST_TEMP;
                break;
        }
    }

    LOG_INFO(TAG, "Applying heating mode %s with target %d.%d°C",
             mode == MODE_COMFORT ? "COMFORT" :
             mode == MODE_ECO ? "ECO" : "FROST",
             targetTemp / 10, abs(targetTemp % 10));

    // TEMPORARY. This write had no mark at all, so the storage loop synced the window's
    // target into the save shadow within a second of the window starting, and the mark
    // applied at window END then FROZE the shadow holding it - after which any later save
    // persisted the window target and a reboot ran the house at it indefinitely. The
    // highest-severity finding of the 2026-10-30 diff review; it needed no save during the
    // window at all, and fired after every window.
    //
    // SetRoomTarget takes and releases the settings mutex itself, so this block must NOT
    // give it back - it no longer holds it.
    if (PersistentStorageTask_SetRoomTarget(targetTemp, RoomTargetDurability::Temporary)) {
        // Note: There's no heatingEnabled flag in SystemSettings.
        // The heating system state is controlled via event bits and system state.
        // For frost protection mode, we just set a low target temperature.

        // Notify heating control task that settings have changed
        // The heating control task will pick up the new target temperature
        LOG_DEBUG(TAG, "Updated heating target temperature to %d.%d°C",
                 targetTemp / 10, abs(targetTemp % 10));
        
        // For enabling/disabling heating, we use the control request bits
        if (mode == MODE_FROST_PROTECT) {
            // Don't disable heating entirely, just set low temperature
            // The system will maintain minimum temperature
        } else {
            // Check if water heating is active before overriding
            EventBits_t burnerRequestBits = xEventGroupGetBits(SRP::getBurnerRequestEventGroup());
            EventBits_t systemStateBits = xEventGroupGetBits(SRP::getSystemStateEventGroup());
            bool waterActive = (burnerRequestBits & SystemEvents::BurnerRequest::WATER) != 0;
            bool waterPriority = (systemStateBits & SystemEvents::SystemState::WATER_PRIORITY) != 0;
            
            if (waterActive) {
                if (waterPriority) {
                    // Water heating has priority - don't override heating
                    LOG_INFO(TAG, "Water heating active with priority - deferring heating activation");
                } else {
                    // Water heating is active but doesn't have priority - still respect it
                    LOG_INFO(TAG, "Water heating active - waiting for completion before starting heating");
                }
            } else {
                // Ensure heating is enabled for comfort/eco modes
                SRP::setControlRequestsEventBits(SystemEvents::ControlRequest::HEATING_ON_OVERRIDE);
            }
        }
    }
}