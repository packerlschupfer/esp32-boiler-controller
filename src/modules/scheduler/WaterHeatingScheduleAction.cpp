// src/modules/scheduler/WaterHeatingScheduleAction.cpp
// Water heating schedule action handler implementation
#include "modules/scheduler/WaterHeatingScheduleAction.h"
#include "TimerSchedule.h"
#include "modules/control/BurnerRequestManager.h"
#include "modules/control/BurnerStateMachine.h"  // S2: For POST_PURGE check
#include "core/SystemResourceProvider.h"
#include "events/SystemEventsGenerated.h"
#include "shared/Temperature.h"
#include "shared/SharedSensorReadings.h"
#include "LoggingMacros.h"
#include "config/SystemSettings.h"

static const char* TAG = "WaterHeatingAction";

WaterHeatingScheduleAction::WaterHeatingScheduleAction() {
    LOG_INFO(TAG, "Water heating schedule action handler initialized");
}

void WaterHeatingScheduleAction::onScheduleStart(const TimerSchedule& schedule) {
    LOG_INFO(TAG, "Schedule '%s' starting - target temp: %d°C, priority: %s",
             schedule.name.c_str(),
             schedule.actionData.waterHeating.targetTempC,
             schedule.actionData.waterHeating.priority ? "YES" : "NO");
    
    // Get target temperature (fixed-point)
    Temperature_t targetTemp = tempFromWhole(schedule.actionData.waterHeating.targetTempC);

    // If no specific temperature set, use system default
    if (schedule.actionData.waterHeating.targetTempC == 0) {
        if (SRP::takeSystemSettingsMutex(pdMS_TO_TICKS(100))) {
            targetTemp = SRP::getSystemSettings().wHeaterConfTempLimitHigh;
            SRP::giveSystemSettingsMutex();
            LOG_INFO(TAG, "Using system default target temperature");
        }
    }
    
    // Check if water has priority from system state (single source of truth)
    EventBits_t systemStateBits = xEventGroupGetBits(SRP::getSystemStateEventGroup());
    bool waterPriority = (systemStateBits & SystemEvents::SystemState::WATER_PRIORITY) != 0;

    // S2: Check if burner is in POST_PURGE (safety cooldown)
    // Starting water heating during post-purge could restart burner prematurely
    BurnerSMState burnerState = BurnerStateMachine::getCurrentState();
    if (burnerState == BurnerSMState::POST_PURGE) {
        LOG_INFO(TAG, "Burner in POST_PURGE - deferring schedule start until cooldown complete");
        // Schedule will be re-evaluated on next check (30s)
        return;
    }

    // If water doesn't have priority, check if heating is already active
    if (!waterPriority) {
        EventBits_t burnerRequestBits = xEventGroupGetBits(SRP::getBurnerRequestEventGroup());
        bool heatingActive = (burnerRequestBits & SystemEvents::BurnerRequest::HEATING) != 0;

        if (heatingActive) {
            LOG_INFO(TAG, "Heating is active and water doesn't have priority - deferring water heating");
            // Don't set the water request - heating continues
            return;
        }
    }

    BurnerRequestManager::setWaterRequest(targetTemp, true);
    
    // Update active schedule name for status reporting
    activeScheduleName_ = schedule.name;
}

void WaterHeatingScheduleAction::onScheduleEnd(const TimerSchedule& schedule) {
    LOG_INFO(TAG, "Schedule '%s' ending", schedule.name.c_str());
    
    // Clear water heating request
    BurnerRequestManager::clearRequest(BurnerRequestManager::RequestSource::WATER);
    
    // Clear active schedule name
    activeScheduleName_ = "";
    isPreheating_ = false;
}

void WaterHeatingScheduleAction::onPreheatingStart(const TimerSchedule& schedule, uint32_t minutesUntilStart) {
    LOG_INFO(TAG, "Starting preheating for schedule '%s' - %lu minutes until start",
             schedule.name.c_str(), minutesUntilStart);

    // Get current water temperature (fixed-point)
    Temperature_t currentTemp = tempFromWhole(20);  // Default 20.0°C if sensor unavailable

    if (SRP::takeSensorReadingsMutex(pdMS_TO_TICKS(100))) {
        currentTemp = SRP::getSensorReadings().waterHeaterTempTank;
        SRP::giveSensorReadingsMutex();
    }

    // Get target temperature (fixed-point)
    Temperature_t targetTemp = tempFromWhole(schedule.actionData.waterHeating.targetTempC);
    if (targetTemp == 0) {
        // Use system default
        if (SRP::takeSystemSettingsMutex(pdMS_TO_TICKS(100))) {
            targetTemp = SRP::getSystemSettings().wHeaterConfTempLimitHigh;
            SRP::giveSystemSettingsMutex();
        }
    }

    Temperature_t tempRise = tempSub(targetTemp, currentTemp);

    // Only preheat if we need to raise temperature (>2.0°C)
    if (tempGreater(tempRise, 20)) {  // 20 = 2.0°C in fixed-point
        LOG_INFO(TAG, "Preheating needed: current %d.%d°C -> target %d.%d°C (%d.%d°C rise)",
                 currentTemp / 10, abs(currentTemp % 10),
                 targetTemp / 10, abs(targetTemp % 10),
                 tempRise / 10, abs(tempRise % 10));

        // Check if water has priority from system state (single source of truth)
        EventBits_t systemStateBits = xEventGroupGetBits(SRP::getSystemStateEventGroup());
        bool waterPriority = (systemStateBits & SystemEvents::SystemState::WATER_PRIORITY) != 0;

        // If water doesn't have priority, check if heating is already active
        if (!waterPriority) {
            EventBits_t burnerRequestBits = xEventGroupGetBits(SRP::getBurnerRequestEventGroup());
            bool heatingActive = (burnerRequestBits & SystemEvents::BurnerRequest::HEATING) != 0;

            if (heatingActive) {
                LOG_INFO(TAG, "Heating is active and water doesn't have priority - deferring preheating");
                return;
            }
        }

        BurnerRequestManager::setWaterRequest(targetTemp, true);
        
        isPreheating_ = true;
        activeScheduleName_ = schedule.name + " (preheating)";
    } else {
        LOG_INFO(TAG, "No preheating needed - water already at temperature");
    }
}

bool WaterHeatingScheduleAction::needsPreheating() const {
    // Water heating benefits from preheating to ensure hot water is ready
    return true;
}

uint32_t WaterHeatingScheduleAction::getPreheatingMinutes() const {
    // Get heating rate from system settings
    float heatingRate = 1.0f;  // Default 1°C per minute
    
    if (SRP::takeSystemSettingsMutex(pdMS_TO_TICKS(100))) {
        heatingRate = SRP::getSystemSettings().waterHeatingRate;
        SRP::giveSystemSettingsMutex();
    }
    
    // Calculate preheat time based on typical temperature rise
    // Assume we need to heat from 20°C to 60°C = 40°C rise
    uint32_t preheatMinutes = static_cast<uint32_t>(40.0f / heatingRate);
    
    // Cap at reasonable maximum
    if (preheatMinutes > 120) {
        preheatMinutes = 120;  // 2 hours max
    }
    
    LOG_DEBUG(TAG, "Preheat time: %lu minutes (heating rate: %.2f°C/min)",
              preheatMinutes, heatingRate);
    
    return preheatMinutes;
}

const char* WaterHeatingScheduleAction::getTypeName() const {
    return "Water Heating";
}

ScheduleType WaterHeatingScheduleAction::getType() const {
    return ScheduleType::WATER_HEATING;
}

size_t WaterHeatingScheduleAction::serializeActionData(const TimerSchedule& schedule, 
                                                       uint8_t* buffer, size_t size) const {
    if (size < 2) return 0;
    
    buffer[0] = schedule.actionData.waterHeating.targetTempC;
    buffer[1] = schedule.actionData.waterHeating.priority;
    
    return 2;
}

bool WaterHeatingScheduleAction::deserializeActionData(TimerSchedule& schedule, 
                                                      const uint8_t* buffer, size_t size) {
    if (size < 2) return false;
    
    schedule.actionData.waterHeating.targetTempC = buffer[0];
    schedule.actionData.waterHeating.priority = buffer[1];
    
    return true;
}

const String& WaterHeatingScheduleAction::getActiveScheduleName() const {
    return activeScheduleName_;
}

bool WaterHeatingScheduleAction::isPreheating() const {
    return isPreheating_;
}