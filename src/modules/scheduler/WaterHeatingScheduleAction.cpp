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

    // NO DEFERRAL LOGIC HERE ANY MORE, and that is the point.
    //
    // This used to check POST_PURGE and, with priority off, whether heating was active - and
    // RETURN without setting a request. But TimerSchedulerTask::activateSchedule() marks the
    // occurrence active and publishes schedule_start regardless, and checkSchedule() never
    // re-invokes the handler while that id is active. So a deferred start silently lost the
    // whole scheduled charge, and the window end then cleared a request this never made. The
    // deferral also fired whenever HeatingControlTask merely held the HEATING request bit, not
    // only while the burner was firing, so it was easy to hit.
    //
    // Both checks were also duplicating gating that already exists downstream, which is the
    // recurring shape in this codebase: the burner state machine enforces POST_PURGE itself
    // (RESTART_FROM_POST_PURGE requires canTurnOn(), i.e. MIN_OFF_TIME), and precedence
    // between heating and water is BurnerRequestManager's job - setHeatingRequest() clears
    // WATER, and WATER_PRIORITY decides who yields. A third copy of those rules here could
    // only ever disagree with them.
    //
    // So the action now states intent - "the schedule wants hot water now" - and
    // WheaterControlTask decides when that is safe. No deferral, nothing to lose, and the
    // "marked active but never acted" finding dissolves rather than needing a bool return.
    (void)waterPriority;

    // KNOWN LIMITATION, stated rather than hidden. A water schedule carries its own
    // target_temp (the documented "Morning Shower" example uses 55), but WATER_ON_OVERRIDE
    // carries no target - WheaterControlTask charges to the configured wheater/tempLimitHigh.
    // The old direct setWaterRequest(targetTemp) path did honour the schedule's figure, but it
    // was the orphan-request bug: it never reached the burner and ended in a latched emergency
    // stop, so it honoured the target only in the sense that nothing happened at that target.
    //
    // Carrying a per-window target properly means plumbing it to the task that owns the
    // charge; that is a feature, not part of this fix, and inventing it here would put a
    // second target source next to the configured limits - the exact shape this change is
    // removing. Logged at WARN so an operator who sets a target and sees the tank charge to
    // the configured limit can find out why.
    {
        char tgtBuf[16];
        formatTemp(tgtBuf, sizeof(tgtBuf), targetTemp);
        LOG_WARN(TAG, "Schedule '%s' requests %s°C, but a scheduled charge uses the configured "
                      "wheater/tempLimitHigh - the per-schedule target is not applied",
                 schedule.name.c_str(), tgtBuf);
    }

    // EXPRESS INTENT TO THE MODE TASK; DO NOT ACTUATE THE BURNER.
    //
    // This used to call BurnerRequestManager::setWaterRequest() directly, which sets the
    // shared WATER request bit but NOT SystemState::WATER_ON - the only setter of that is
    // WheaterControlTask:373. So the request was an orphan: the burner cannot start on it
    // (BurnerTransitions::hasActiveModeDemand needs WATER_ON && WATER), heating is suppressed
    // while priority is on, nothing refreshed it, and after REQUEST_EXPIRATION_MS (600 s) the
    // burner request watchdog cleared every request and called triggerEmergencyShutdown:
    // burner relays off, BOTH PUMPS FORCED ON, EMERGENCY_STOP latched, BOILER_ENABLED cleared
    // (which also disables frost protection), an FRAM emergency record written, and only
    // boiler/cmd/emergency_reset to release it. A scheduled morning shower could therefore
    // end in a latched emergency stop with no heat and no hot water.
    //
    // It was also intermittent in the most confusing way: if the tank happened to be below
    // the low limit at the window start, WheaterControlTask adopted the charge normally and
    // the window worked. So it would have looked like a random overnight emergency stop.
    //
    // WATER_ON_OVERRIDE is the correct mechanism and already exists - the task honours it at
    // WheaterControlTask.cpp:340/382, sets WATER_ON itself, and refreshes the request every
    // 5 min. The same shape SpaceHeatingScheduleAction already uses via HEATING_ON_OVERRIDE.
    // Found by the 2026-10-28 review.
    xEventGroupSetBits(SRP::getControlRequestsEventGroup(),
                       SystemEvents::ControlRequest::WATER_ON_OVERRIDE);

    // Update active schedule name for status reporting
    activeScheduleName_ = schedule.name;
}

void WaterHeatingScheduleAction::onScheduleEnd(const TimerSchedule& schedule) {
    LOG_INFO(TAG, "Schedule '%s' ending", schedule.name.c_str());

    // CLEAR OUR OWN OVERRIDE, NOT THE SHARED REQUEST BIT.
    //
    // This used to call clearRequest(WATER) unconditionally, without checking who set it. If
    // WheaterControlTask was mid-charge on its own thermostat decision, that dropped
    // hasActiveModeDemand and sent the burner to POST_PURGE within 10 s - bypassing the 2 min
    // minimum on-time - while the task stayed in WheaterOn with the water pump running and
    // only re-issued its request on the next >1.0 K target drift or its 300 s refresh. One
    // unnecessary stop/restart and up to ~5 min of pump-only circulation per overlapping
    // window end. Clearing the override instead lets the task decide whether the charge
    // should continue, which is the whole point of it owning the mode.
    xEventGroupClearBits(SRP::getControlRequestsEventGroup(),
                         SystemEvents::ControlRequest::WATER_ON_OVERRIDE);

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