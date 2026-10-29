// src/modules/tasks/HeatingControlTask_EventDriven.cpp
// Heating control task - manages space heating with PID control
#include "HeatingControlTask.h"

#include "config/SystemConstants.h"
#include "shared/SharedResources.h"
#include "shared/Temperature.h"
#include "events/SystemEventsGenerated.h"
#include "modules/control/HeatingControlModule.h"
#include "modules/control/BurnerRequestManager.h"
#include "modules/control/BurnerSafetyValidator.h"
#include "modules/control/TemperatureSensorFallback.h"
#include "modules/control/FrostProtection.h"  // the two frost latches
#include "modules/control/SpaceHeatingPolicy.h"
#include "modules/control/RoomTempFallbackPolicy.h"
#include "modules/control/OutsideTempFallbackPolicy.h"  // hold + room-thermostat fallback
#include "modules/control/HeatingControlDiagnostics.h"
#include "modules/tasks/MQTTTask.h"   // publishControlDiagnostics
#include "MQTTTopics.h"
#include "utils/ResourceGuard.h"
#include "utils/MutexRetryHelper.h"
#include "LoggingMacros.h"
#include "config/ProjectConfig.h"
#include <esp_task_wdt.h>
#include <algorithm>  // for std::max
#include "core/SystemResourceProvider.h"
#include <TaskManager.h>
#include <esp_log.h>

// Constants
#define DEFAULT_BOILER_TARGET_TEMP tempFromFloat(70.0f)

// Timer handles
static TimerHandle_t safetyCheckTimer = nullptr;
static TimerHandle_t processTimer = nullptr;
static TaskHandle_t heatingTaskHandle = nullptr;

// State tracking - all task state is consolidated here to prevent scattered statics
// NOTE: Only accessed by HeatingControlTask - single task, no mutex needed
static struct {
    HeatingControlState state = HeatingOff;
    Temperature_t lastBoilerTarget = 0;
    bool initialized = false;
    HeatingControlModule* heatingControl = nullptr;
    bool waterDeferralLogged = false;  // Prevents log spam when water has priority
} heatingState;

// Forward declarations
static void safetyCheckCallback(TimerHandle_t xTimer);
static void processTimerCallback(TimerHandle_t xTimer);
static void processHeatingState(EventBits_t pendingControl = 0);
static bool checkIfSpaceHeatingNeededEvent();

// Diagnostics for boiler/status/heating/control. Written only by this task
// (checkIfSpaceHeatingNeededEvent and processHeatingState both run on it), so no
// synchronisation is needed; published by publishControlDiagnostics() below.
//
// STARTUP is the honest initial value, and it took two attempts to get here.
//
// It was Block::OK, which *means* "heating is wanted" - so for the ~4 s between boot and the
// first run of checkIfSpaceHeatingNeededEvent() this topic claimed heating was wanted.
// Measured at the 2026-10-25 01:48:04 UTC boot: `dem:false blk:"none" i:192 isrc:"tgt"`, then
// four seconds later the first real evaluation gave `blk:"backstop" i:204 isrc:"meas"`.
// Nothing actuated - dem stayed false and every relay is de-energised at boot - but this is
// the one topic that exists so "why is heating off" can be answered from the firmware's own
// state, and for those seconds it answered wrongly.
//
// c1ed1b0 changed it to SENSOR_INVALID, which was better but still not true: that reason means
// a READING IS BROKEN, and Home Assistant renders it "Sensor fault". So every single reboot
// wrote a "Sensor fault" event into HA's history. Two costs, neither cosmetic: it trains an
// operator to discount a genuinely alarming string, and it makes a real sensor fault
// indistinguishable from a restart when reading that history back later.
//
// The actual state at boot is "nothing has been evaluated yet", which is now its own reason.
// Nothing is wrong, and the topic says so. See HeatingControlDiagnostics::Block::STARTUP.
// Caller-owned memory for the outside-reading hold, and the last reported source so the
// transition is logged once rather than every 5 s. Statics are safe: every caller of
// checkIfSpaceHeatingNeeded() is HeatingControlTask, a single task.
static OutsideTempFallbackPolicy::Sample g_outsideSample;
static OutsideTempFallbackPolicy::Source g_lastOutsideSource =
    OutsideTempFallbackPolicy::Source::MEASURED;

static HeatingControlDiagnostics::Block g_blockReason =
    HeatingControlDiagnostics::Block::STARTUP;
static HeatingControlModule::CurveDebug g_curveDebug;
static Temperature_t g_commandedTarget = 0;
static void publishControlDiagnostics();

void HeatingControlTask(void *parameter) {
    const char* TAG = "HeatingControlTask";
    
    heatingTaskHandle = xTaskGetCurrentTaskHandle();
    
    LOG_INFO(TAG, "Started (Event-Driven) C%d Stk:%d", xPortGetCoreID(), uxTaskGetStackHighWaterMark(NULL));
    
    // Get heating control module from service container
    heatingState.heatingControl = SRP::getHeatingControl();

    // Validate critical module is available
    if (heatingState.heatingControl == nullptr) {
        LOG_ERROR(TAG, "CRITICAL: HeatingControlModule not available - cannot operate");
        ErrorHandler::logError(TAG, SystemError::NOT_INITIALIZED,
                              "HeatingControlModule not initialized");
        // Continue task but log the issue - system will operate in degraded mode
        // All operations will be safely skipped due to nullptr checks
    }

    LOG_INFO(TAG, "Init OK");
    
    // Watchdog will be registered after initialization
    
    // Create process timer (runs every 5 seconds like MB8ART)
    processTimer = xTimerCreate(
        "HeatingProcess",
        pdMS_TO_TICKS(5000),   // 5 second interval
        pdTRUE,                // Auto-reload
        nullptr,
        processTimerCallback
    );
    
    if (!processTimer) {
        LOG_ERROR(TAG, "Failed to create process timer");
        // No cleanup needed - timer wasn't created
        vTaskDelete(NULL);
        return;
    }

    // Create safety check timer (runs every 60 seconds to handle sensor failures)
    safetyCheckTimer = xTimerCreate(
        "HeatingSafety",
        pdMS_TO_TICKS(60000),  // 60 second safety interval
        pdTRUE,                // Auto-reload
        nullptr,
        safetyCheckCallback
    );

    if (!safetyCheckTimer) {
        LOG_ERROR(TAG, "Failed to create safety timer");
        // Cleanup: delete process timer before exiting
        xTimerDelete(processTimer, 0);
        processTimer = nullptr;
        vTaskDelete(NULL);
        return;
    }

    heatingState.initialized = true;

    // Start timers
    if (xTimerStart(processTimer, pdMS_TO_TICKS(100)) != pdPASS) {
        LOG_ERROR(TAG, "Failed to start process timer");
        // Cleanup: delete both timers before exiting
        xTimerDelete(processTimer, 0);
        xTimerDelete(safetyCheckTimer, 0);
        processTimer = nullptr;
        safetyCheckTimer = nullptr;
        vTaskDelete(NULL);
        return;
    }
    if (xTimerStart(safetyCheckTimer, pdMS_TO_TICKS(100)) != pdPASS) {
        LOG_ERROR(TAG, "Failed to start safety timer");
        // Cleanup: stop and delete both timers before exiting
        xTimerStop(processTimer, 0);
        xTimerDelete(processTimer, 0);
        xTimerDelete(safetyCheckTimer, 0);
        processTimer = nullptr;
        safetyCheckTimer = nullptr;
        vTaskDelete(NULL);
        return;
    }

    LOG_INFO(TAG, "Event-driven mode activated");
    
    // Register with watchdog after initialization
    // Timeout should be at least 4x the room sensor read interval to allow for delays
    // Use configured minimum as a floor to prevent too-short timeouts
    uint32_t sensorInterval = SRP::getRoomSensorReadInterval();  // Use ANDRTF3 interval since we wait for INSIDE_TEMP
    uint32_t watchdogTimeout = std::max(sensorInterval * 4, SystemConstants::System::WDT_HEATING_CONTROL_MS);

    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::enabled(
        false,  // not critical (won't reset system)
        watchdogTimeout
    );

    LOG_INFO(TAG, "Watchdog timeout set to %lu ms (max of 4x sensor %lu ms or min %lu ms)",
             watchdogTimeout, sensorInterval, SystemConstants::System::WDT_HEATING_CONTROL_MS);

    if (!SRP::getTaskManager().registerCurrentTaskWithWatchdog("HeatingControl", wdtConfig)) {
        LOG_ERROR(TAG, "WDT reg failed");
    } else {
        LOG_INFO(TAG, "WDT OK %lums", watchdogTimeout);
        (void)SRP::getTaskManager().feedWatchdog();  // Feed immediately
    }
    
    // Main task loop - use timer notifications like MB8ART
    while (true) {
        // Wait for timer notification with 1 second timeout for watchdog
        uint32_t ulNotificationValue = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        
        if (ulNotificationValue > 0) {
            // Timer triggered - process heating state
            processHeatingState();
        }
        
        // Also check for immediate control events
        EventBits_t controlBits = xEventGroupGetBits(SRP::getControlRequestsEventGroup());
        const EventBits_t CONTROL_EVENTS = SystemEvents::ControlRequest::HEATING_ON_OVERRIDE |
                                          SystemEvents::ControlRequest::HEATING_OFF_OVERRIDE |
                                          SystemEvents::ControlRequest::WATER_PRIORITY_RELEASED;

        if (controlBits & CONTROL_EVENTS) {
            // Control events trigger immediate processing. The bits are cleared first,
            // so hand the override bits over - processHeatingState() re-read the cleared
            // bits and never acted on an override (review 2026-09-14).
            xEventGroupClearBits(SRP::getControlRequestsEventGroup(), controlBits & CONTROL_EVENTS);
            processHeatingState(controlBits & (SystemEvents::ControlRequest::HEATING_ON_OVERRIDE |
                                               SystemEvents::ControlRequest::HEATING_OFF_OVERRIDE));
        }

        // After the state has been evaluated, not before - so the payload reflects this
        // cycle's decision. Self-rate-limited: publishes on change, else once per
        // heartbeat, so the 1 s loop does not become a 1 s publisher.
        publishControlDiagnostics();

        // Feed watchdog
        (void)SRP::getTaskManager().feedWatchdog();
    }
}

static void processTimerCallback(TimerHandle_t xTimer) {
    (void)xTimer;
    // Notify task to process - like MB8ART pattern
    if (heatingTaskHandle != nullptr) {
        xTaskNotifyGive(heatingTaskHandle);
    }
}

static void safetyCheckCallback(TimerHandle_t xTimer) {
    (void)xTimer;
    const char* TAG = "HeatingSafety";
    
    // Safety check - ensure we haven't missed sensor updates for too long
    if (heatingState.state == HeatingOn) {
        // Check if sensors are still available
        if (!TemperatureSensorFallback::canContinueOperation()) {
            LOG_ERROR(TAG, "Sensor failure detected during heating - shutting down");
            if (heatingState.heatingControl != nullptr) {
                heatingState.heatingControl->stopHeating();
            } else {
                LOG_ERROR(TAG, "HeatingControlModule unavailable - cannot issue stop command");
            }
            SRP::clearSystemStateEventBits(SystemEvents::SystemState::HEATING_ON);
            SRP::clearHeatingEventBits(SystemEvents::HeatingEvent::True);
            BurnerRequestManager::clearRequest(BurnerRequestManager::RequestSource::HEATING);
            heatingState.state = HeatingOff;
        }
    }
}

/**
 * @brief Is it cold enough that the burner must run regardless of the heating switch?
 *
 * The evaluation and the latch moved to FrostProtection (2026-10-25): a cold DHW TANK needs a
 * water charge, not space heating, and the water task cannot see a static in this file. The
 * static_assert tying the trigger to FREEZE_PROTECTION_TEMP_C moved with it.
 *
 * Deliberately NOT a bypass of anything except HEATING_ENABLED and the summer override. It
 * does not override BOILER_ENABLED (a drained-for-service system is a likely reason to
 * switch the boiler off, and firing into a drained boiler is worse than frost), and it does
 * not bypass the sensor fallback, the pressure interlock or anti-flapping. See
 * FrostProtectionPolicy.h for what it therefore cannot protect against - a dead controller
 * above all, since CH1 BURNER_ENABLE is normally open.
 */
static inline bool frostDemand() {
    return FrostProtection::demand();
}

/**
 * @brief Drop every trace of an active space-heating run.
 *
 * Extracted because the four sites that end a run were each open-coding the same four steps,
 * and the two abandon paths in the HeatingOff case were doing NONE of them - see the invariant
 * note in that case. HEATING_ON is not a private flag: PumpControlModule runs the heating pump
 * off it (`modeActiveBit`), WheaterControlTask refuses to start a water charge while it is set
 * and water priority is off, and SafetyInterlocks/BurnerStateMachine both read it. Leaving it
 * set with no burner request therefore runs the pump and locks out hot water indefinitely.
 */
static void clearHeatingActive(const char* TAG) {
    if (heatingState.heatingControl != nullptr) {
        heatingState.heatingControl->stopHeating();
    } else {
        LOG_WARN(TAG, "HeatingControlModule unavailable - clearing state only");
    }
    SRP::clearSystemStateEventBits(SystemEvents::SystemState::HEATING_ON);
    SRP::clearHeatingEventBits(SystemEvents::HeatingEvent::True);
    BurnerRequestManager::clearRequest(BurnerRequestManager::RequestSource::HEATING);
    heatingState.state = HeatingOff;
}

static void processHeatingState(EventBits_t pendingControl) {
    const char* TAG = "HeatingProcess";
    EventBits_t systemStateBits = SRP::getSystemStateEventBits();

    FrostProtection::evaluate();

    // Check if both boiler and heating are enabled. Frost demand overrides the HEATING
    // switch - that is exactly when freezing happens - but never BOILER_ENABLED.
    if (!(systemStateBits & SystemEvents::SystemState::BOILER_ENABLED) || 
        (!(systemStateBits & SystemEvents::SystemState::HEATING_ENABLED) && !frostDemand())) {
        
        // System disabled - turn off heating if on.
        //
        // Keyed on the HEATING_ON BIT as well as our own state variable. It used to test
        // `state == HeatingOn` alone, so a run abandoned mid-start (state still HeatingOff,
        // bit already set) was invisible here: the boiler could then be switched off with the
        // heating pump still running off that bit and water charging still locked out.
        if (heatingState.state == HeatingOn ||
            (systemStateBits & SystemEvents::SystemState::HEATING_ON)) {
            LOG_INFO(TAG, "System disabled - turning off heating");
            clearHeatingActive(TAG);
        }
        return;
    }
    
    // Operation mode is managed by BurnerControlTask - don't set it here
    
    // Get control bits
    EventBits_t controlBits = SRP::getControlRequestsEventBits() | pendingControl;

    switch (heatingState.state) {
        case HeatingOff: {
            // INVARIANT: believing heating is off while the HEATING_ON bit is set is never
            // correct between invocations, and it is not harmless - the heating pump runs off
            // that bit and a water charge is refused while it is set with priority off. Heal
            // it here rather than trusting every exit path to be symmetric, because the two
            // abandon paths below were not and nothing else in the system clears the bit when
            // our state says HeatingOff.
            if (systemStateBits & SystemEvents::SystemState::HEATING_ON) {
                LOG_WARN(TAG, "HEATING_ON was set while heating is off - clearing");
                clearHeatingActive(TAG);
            }

            // Check if we should turn on heating. Frost demand is a third reason, alongside
            // the normal room/weather decision and the remote ON override.
            if (checkIfSpaceHeatingNeededEvent() || frostDemand() ||
                (controlBits & SystemEvents::ControlRequest::HEATING_ON_OVERRIDE)) {
                // Check settings for water priority (race prevention)
                // H2 fix: snapshot bool setting under mutex before use
                bool waterPriorityEnabled = false;
                if (SRP::takeSystemSettingsMutex(pdMS_TO_TICKS(50))) {
                    waterPriorityEnabled = SRP::getSystemSettings().wheaterPriorityEnabled;
                    SRP::giveSystemSettingsMutex();
                }
                EventBits_t systemStateBits = SRP::getSystemStateEventBits();
                bool waterEnabled = (systemStateBits & SystemEvents::SystemState::WATER_ENABLED) != 0;

                // Round 15 Issue #13 fix: Use event-based synchronization instead of timing delay
                // If water priority enabled, check if water task has had a chance to claim priority
                if (waterPriorityEnabled && waterEnabled) {
                    // Wait briefly for water to claim burner OR release priority
                    // This is event-based: WATER request bit indicates water wants burner
                    EventBits_t waitBits = xEventGroupWaitBits(
                        SRP::getBurnerRequestEventGroup(),
                        SystemEvents::BurnerRequest::WATER,
                        pdFALSE,  // Don't clear bits
                        pdFALSE,  // Wait for any bit
                        pdMS_TO_TICKS(200)  // 200ms timeout - if not set, water doesn't need burner
                    );
                    (void)waitBits;  // We check the result below
                }

                // Check if water heating has priority BEFORE setting HEATING_ON
                // (prevents start/stop loop when water reclaims priority)
                EventBits_t burnerRequestBits = xEventGroupGetBits(SRP::getBurnerRequestEventGroup());
                EventBits_t systemStateBitsLocal = xEventGroupGetBits(SRP::getSystemStateEventGroup());
                bool waterActive = (burnerRequestBits & SystemEvents::BurnerRequest::WATER) != 0;
                bool waterPriority = (systemStateBitsLocal & SystemEvents::SystemState::WATER_PRIORITY) != 0;

                if (waterActive && waterPriority) {
                    if (!heatingState.waterDeferralLogged) {
                        LOG_INFO(TAG, "Water heating has priority - deferring heating activation");
                        heatingState.waterDeferralLogged = true;
                    }
                    // Clear the override bit since we can't act on it now
                    xEventGroupClearBits(SRP::getControlRequestsEventGroup(),
                                        SystemEvents::ControlRequest::HEATING_ON_OVERRIDE);
                    return;
                }

                // Reset deferral flag when we're no longer being deferred
                heatingState.waterDeferralLogged = false;

                // Checked BEFORE startHeating(), not after. It used to sit after, so a
                // sensor outage set HEATING_ON and then returned - running the heating pump
                // and blocking water charging with no burner request behind it, for as long as
                // the outage lasted. Nothing downstream needs the bit to answer this.
                if (!TemperatureSensorFallback::canContinueOperation()) {
                    LOG_WARN(TAG, "Cannot start heating - required sensors unavailable");
                    return;
                }

                // Set HEATING_ON bit now that we know water doesn't have priority
                // (needed for seamless mode switching from water to heating)
                if (heatingState.heatingControl != nullptr) {
                    heatingState.heatingControl->startHeating();
                } else {
                    LOG_ERROR(TAG, "Cannot start heating - HeatingControlModule unavailable");
                    return;
                }

                
                // Calculate boiler target temperature
                Temperature_t boilerTargetTemp = DEFAULT_BOILER_TARGET_TEMP;
                
                // Get sensor readings for PID calculation
                {
                    auto guard = MutexRetryHelper::acquireGuard(
                        SRP::getSensorReadingsMutex(),
                        "SensorReadings-HeatingCalc"
                    );
                    if (guard) {
                        SharedSensorReadings readings = SRP::getSensorReadings();
                        // H2 fix: take settings mutex inside sensor guard (Level 2, alphabetical order safe)
                        if (SRP::takeSystemSettingsMutex(pdMS_TO_TICKS(50))) {
                            if (heatingState.heatingControl != nullptr) {
                                boilerTargetTemp = heatingState.heatingControl->calculateSpaceHeatingTargetTemp(
                                    readings, SRP::getSystemSettings(), &g_curveDebug);
                                g_commandedTarget = boilerTargetTemp;
                            } else {
                                LOG_WARN(TAG, "HeatingControlModule unavailable - using default target");
                            }
                            SRP::giveSystemSettingsMutex();
                        } else {
                            LOG_ERROR(TAG, "Failed to acquire settings mutex - using default target");
                        }
                    } else {
                        // Past startHeating(), so the bit is already set: undo the whole start
                        // rather than returning with a pump running and no burner request.
                        LOG_ERROR(TAG, "Failed to acquire sensor mutex - abandoning start");
                        clearHeatingActive(TAG);
                        return;
                    }
                }

                // A frost burn asks for the FLOOR, not the curve. The curve is a function of
                // outside temperature, so at frost temperatures it would ask for a high flow
                // and heat the house - the opposite of the intent, which is to get the water
                // off 5 °C and stop. setHeatingRequest clamps to heating_low_limit anyway,
                // so requesting anything lower would be clamped back up to it.
                //
                // Gated on "nothing BUT frost authorised this burn", not on
                // `!checkIfSpaceHeatingNeededEvent()`. That predicate asks the weather/room
                // decision and knows nothing about the heating SWITCH - so with heating
                // switched off on a genuinely cold day it returns TRUE (outside is cold, room
                // below the backstop), the floor branch was skipped, and a frost burn heated
                // the house to the full curve target. In other words the branch only ever fired
                // in mild weather and never in a frost, which is the one case it was written
                // for. Found by the 2026-10-26 review.
                const bool normalHeatingWanted =
                    (systemStateBits & SystemEvents::SystemState::HEATING_ENABLED) &&
                    !(controlBits & SystemEvents::ControlRequest::HEATING_OFF_OVERRIDE) &&
                    checkIfSpaceHeatingNeededEvent();
                if (frostDemand() && !normalHeatingWanted) {
                    if (SRP::takeSystemSettingsMutex(pdMS_TO_TICKS(50))) {
                        boilerTargetTemp = SRP::getSystemSettings().heating_low_limit;
                        SRP::giveSystemSettingsMutex();
                    }
                    g_commandedTarget = boilerTargetTemp;
                    char frostBuf[16];
                    formatTemp(frostBuf, sizeof(frostBuf), boilerTargetTemp);
                    LOG_WARN(TAG, "Frost burn starting at the flow floor %s°C (curve ignored)",
                             frostBuf);
                }

                // Note: HEATING_ON already set by startHeating() above
                SRP::setHeatingEventBits(SystemEvents::HeatingEvent::True);

                // Set heating request
                BurnerRequestManager::setHeatingRequest(boilerTargetTemp, false);

                heatingState.state = HeatingOn;
                heatingState.lastBoilerTarget = boilerTargetTemp;

                // Log activation with context based on control mode
                if (SRP::takeSensorReadingsMutex(pdMS_TO_TICKS(100))) {
                    SharedSensorReadings readings = SRP::getSensorReadings();
                    char boilerBuf[16];
                    formatTemp(boilerBuf, sizeof(boilerBuf), boilerTargetTemp);
                    // H2 fix: acquire settings mutex inside sensor mutex (Level 2, alphabetical order safe)
                    if (SRP::takeSystemSettingsMutex(pdMS_TO_TICKS(50))) {
                        const SystemSettings& settings = SRP::getSystemSettings();
                        if (settings.useWeatherCompensatedControl) {
                            // Weather-compensated mode logging
                            char outsideBuf[16], threshBuf[16];
                            formatTemp(outsideBuf, sizeof(outsideBuf), readings.outsideTemp);
                            formatTemp(threshBuf, sizeof(threshBuf), settings.outsideTempHeatingThreshold);
                            LOG_INFO(TAG, "Heating needed: outside %s°C < threshold %s°C",
                                    outsideBuf, threshBuf);
                        } else {
                            // Traditional room-temp mode logging
                            char currBuf[16], targetBuf[16];
                            formatTemp(currBuf, sizeof(currBuf), readings.insideTemp);
                            formatTemp(targetBuf, sizeof(targetBuf), settings.targetTemperatureInside);
                            LOG_INFO(TAG, "Heating needed: room %s°C < target %s°C",
                                    currBuf, targetBuf);
                        }
                        SRP::giveSystemSettingsMutex();
                    }
                    LOG_INFO(TAG, "Space heating activated - Boiler target: %s°C", boilerBuf);
                    SRP::giveSensorReadingsMutex();
                } else {
                    char tempStr[16];
                    formatTemp(tempStr, sizeof(tempStr), boilerTargetTemp);
                    LOG_INFO(TAG, "Space heating activated - Boiler target: %s°C", tempStr);
                }
            }
            break;
        }
        
        case HeatingOn: {
            // Check if we should turn off heating
            bool heatingStillNeeded = checkIfSpaceHeatingNeededEvent();
            bool sensorsAvailable = TemperatureSensorFallback::canContinueOperation();

            // Check if water heating has priority - if so, we must yield
            EventBits_t burnerRequestBits = xEventGroupGetBits(SRP::getBurnerRequestEventGroup());
            bool waterActive = (burnerRequestBits & SystemEvents::BurnerRequest::WATER) != 0;
            bool waterPriority = (systemStateBits & SystemEvents::SystemState::WATER_PRIORITY) != 0;
            bool mustYieldToWater = waterActive && waterPriority;

            // Frost demand suppresses the three "nobody wants heat" reasons - the remote
            // override, the heating switch and the room being satisfied - and nothing else.
            // A sensor failure, a disabled boiler or water priority still stop the burn.
            if (((controlBits & SystemEvents::ControlRequest::HEATING_OFF_OVERRIDE) && !frostDemand()) ||
                !(systemStateBits & SystemEvents::SystemState::BOILER_ENABLED) ||
                (!(systemStateBits & SystemEvents::SystemState::HEATING_ENABLED) && !frostDemand()) ||
                (!heatingStillNeeded && !frostDemand()) ||
                !sensorsAvailable ||
                mustYieldToWater) {
                
                // Determine reason for shutdown
                // The qualifiers MUST match the condition above. Without them this chain
                // named causes that frost had already suppressed - a stop actually triggered
                // by a sensor failure was logged as "remote override OFF" whenever that
                // override happened to be set. Found by the 2026-10-26 review; a wrong reason
                // in the log is worse than "unknown" because it sends the next person after
                // the wrong thing.
                const char* reason = "unknown";
                if ((controlBits & SystemEvents::ControlRequest::HEATING_OFF_OVERRIDE) && !frostDemand()) {
                    reason = "remote override OFF";
                } else if (!(systemStateBits & SystemEvents::SystemState::BOILER_ENABLED)) {
                    reason = "boiler disabled";
                } else if (!(systemStateBits & SystemEvents::SystemState::HEATING_ENABLED) && !frostDemand()) {
                    reason = "heating disabled";
                } else if (mustYieldToWater) {
                    reason = "water heating has priority";
                } else if (!heatingStillNeeded && !frostDemand()) {
                    reason = "target temperature reached";
                } else if (!sensorsAvailable) {
                    reason = "sensor failure";
                }
                
                LOG_INFO(TAG, "Deactivating space heating - reason: %s", reason);
                
                // Deactivate heating
                clearHeatingActive(TAG);
                
            } else {
                // Update boiler target temperature periodically
                Temperature_t newBoilerTarget = DEFAULT_BOILER_TARGET_TEMP;

                {
                    auto guard = MutexRetryHelper::acquireGuard(
                        SRP::getSensorReadingsMutex(),
                        "SensorReadings-HeatingUpdate"
                    );
                    if (guard) {
                        SharedSensorReadings readings = SRP::getSensorReadings();
                        // H2 fix: take settings mutex inside sensor guard (Level 2, alphabetical safe)
                        if (SRP::takeSystemSettingsMutex(pdMS_TO_TICKS(50))) {
                            if (heatingState.heatingControl != nullptr) {
                                newBoilerTarget = heatingState.heatingControl->calculateSpaceHeatingTargetTemp(
                                    readings, SRP::getSystemSettings(), &g_curveDebug);
                                g_commandedTarget = newBoilerTarget;
                            }
                            SRP::giveSystemSettingsMutex();
                        }
                        // If heatingControl is nullptr or mutex failed, keep default target
                    }
                }

                // Same floor substitution as the turn-on path, or a frost burn that outlived
                // one target refresh would silently switch to the weather curve.
                if (frostDemand() && !heatingStillNeeded) {
                    if (SRP::takeSystemSettingsMutex(pdMS_TO_TICKS(50))) {
                        newBoilerTarget = SRP::getSystemSettings().heating_low_limit;
                        SRP::giveSystemSettingsMutex();
                    }
                    g_commandedTarget = newBoilerTarget;
                }

                // Update if changed significantly (>1°C) OR every 5 minutes to refresh watchdog
                static uint32_t lastRefreshTime = 0;
                uint32_t now = millis();
                // H1: Use temp-safe functions for proper overflow handling
                bool targetChanged = tempAbs(tempSub(newBoilerTarget, heatingState.lastBoilerTarget)) > tempFromWhole(1);
                bool needRefresh = (now - lastRefreshTime) > SystemConstants::Tasks::Heating::REFRESH_INTERVAL_MS;

                if (targetChanged || needRefresh) {
                    BurnerRequestManager::setHeatingRequest(newBoilerTarget, false);
                    heatingState.lastBoilerTarget = newBoilerTarget;
                    lastRefreshTime = now;

                    if (targetChanged) {
                        char tempStr[16];
                        formatTemp(tempStr, sizeof(tempStr), newBoilerTarget);
                        LOG_INFO(TAG, "Updated boiler target: %s°C", tempStr);
                    }
                }
            }
            break;
        }
        
        case HeatingError: {
            // In error state - try to recover
            LOG_WARN(TAG, "Heating in error state - attempting recovery");
            // Check if conditions are now valid
            if (TemperatureSensorFallback::canContinueOperation()) {
                heatingState.state = HeatingOff;
                LOG_INFO(TAG, "Recovered from error state");
            }
            break;
        }
    }
}

// Publish boiler/status/heating/control: why heating is on or off, and what the curve
// asked for before the h/sLo clamp flattened it. Added 2026-10-18 so Home Assistant can
// show the real reason instead of inferring one from templates - four values were
// unobservable before this (pre-clamp curve output, the room-deviation shift on its own,
// the active mode, and outside-gate vs room-backstop).
//
// Retained, because it is state a reconnecting subscriber should see; published on change
// and at least every DIAG_HEARTBEAT_MS so a retained pre-reboot value self-corrects. LOW
// priority so it never competes with safety publishes. snprintf into a stack buffer - this
// runs every cycle and the project avoids heap on runtime paths.
static void publishControlDiagnostics() {
    const char* TAG = "HeatingDiag";
    constexpr uint32_t DIAG_HEARTBEAT_MS = 10000;
    static HeatingControlDiagnostics::State lastPublished;
    static uint32_t lastPublishMs = 0;
    static bool everPublished = false;

    HeatingControlDiagnostics::State st;
    const EventBits_t systemBits = SRP::getSystemStateEventBits();
    st.demand = (systemBits & SystemEvents::SystemState::HEATING_ON) != 0;

    if (!(systemBits & SystemEvents::SystemState::HEATING_ENABLED)) {
        g_blockReason = HeatingControlDiagnostics::Block::HEATING_DISABLED;
    }
    // A burn that only frost authorised reports blk:"frost" rather than blk:"none". It used to
    // publish dem:true with blk:"none", which reads as ordinary demand and left an operator
    // with nothing to explain a burn running with the heating switch off (2026-10-26 review).
    // Same "nothing but frost wanted this" discriminator as the floor decision and the
    // operation mode, so all three agree about what kind of burn it is.
    const bool frostOnlyBurn =
        st.demand && frostDemand() &&
        !((systemBits & SystemEvents::SystemState::HEATING_ENABLED) &&
          checkIfSpaceHeatingNeededEvent());
    st.block = frostOnlyBurn ? HeatingControlDiagnostics::Block::FROST
                             : (st.demand ? HeatingControlDiagnostics::Block::OK : g_blockReason);

    // Evaluate the curve here rather than reusing the last control-loop value.
    // calculateSpaceHeatingTargetTemp() only runs on the turn-on and target-update paths,
    // so with heating OFF the cached values stayed 0 - blank in exactly the situation this
    // topic is for ("what does the curve want right now?"). It is a const computation over
    // the same inputs the control loop uses, so this is the curve's current answer, not a
    // prediction. Not quite side-effect-free since 2026-10-23: it also remembers the last
    // valid room reading (RoomTempFallbackPolicy). Harmless from here - this task is the
    // only caller on either path and both pass the same snapshot, so recording from the
    // diagnostics path records exactly what the control path would have.
    //
    // `cmd` is therefore the clamped CURVE output, which equals the active setpoint while
    // demand is on and is what the curve would ask for while it is off. The actually
    // commanded burner target remains t.bt on boiler/status/sensors - and during a water
    // charge bt is the water target, which this topic deliberately does not track.
    HeatingControlModule::CurveDebug dbg;
    Temperature_t curveTarget = 0;
    Temperature_t effectiveLow = 0;
    Temperature_t effectiveHigh = 0;
    if (heatingState.heatingControl == nullptr) {
        return;
    }
    {
        // ONE acquisition for the inputs AND the curve evaluation, so o/i/tgt in the
        // payload are exactly the values `raw` was computed from. Reading them separately
        // let a sensor update land in between and broke that consistency, which is the
        // main reason this topic carries its own inputs at all.
        //
        // Nesting order is Sensor -> Settings, which is the documented direction
        // (docs/MUTEX_HIERARCHY.md; every nested pair in this file is already that way).
        auto guard = MutexRetryHelper::acquireGuard(SRP::getSensorReadingsMutex(),
                                                    "SensorReadings-HeatingDiag");
        if (!guard) {
            return;  // no snapshot, no publish - a stale mix is worse than a skipped cycle
        }
        if (!SRP::takeSystemSettingsMutex(pdMS_TO_TICKS(20))) {
            return;
        }
        const SharedSensorReadings& r = SRP::getSensorReadings();
        const SystemSettings& cfg = SRP::getSystemSettings();
        st.weatherMode = cfg.useWeatherCompensatedControl;
        st.roomTarget = cfg.targetTemperatureInside;
        st.outside = r.outsideTemp;
        effectiveLow = cfg.heating_low_limit;    // h/sLo - the EFFECTIVE floor
        effectiveHigh = cfg.heating_high_limit;  // h/sHi
        curveTarget = heatingState.heatingControl->calculateSpaceHeatingTargetTemp(r, cfg, &dbg);
        SRP::giveSystemSettingsMutex();
    }
    st.rawTarget = dbg.rawTarget;
    st.shift = dbg.shift;
    // The room value the curve USED, not the raw field: with the sensor invalid those
    // differ (held / target / nominal substitution), and `raw` and `shift` were computed
    // from the former. `isrc` says which it was. The raw reading is on
    // boiler/status/sensors as t.i.
    st.room = dbg.effectiveInside;
    st.roomSource = dbg.insideSource;

    // Apply the SECOND clamp, the one BurnerRequestManager::setHeatingRequest() does.
    // calculateSpaceHeatingTargetTemp -> HeatingCurve::target() clamps to
    // settings.burner_low_limit (h/bLo, 38.0), but the request handler then clamps to
    // heating_low_limit (h/sLo), so h/sLo is the effective floor. Without this the topic
    // reported cmd = 380 where the boiler would actually command 400 - caught on the device
    // by cross-checking against an independent model of the curve (2026-10-18). The two
    // limits are both 38.0 since 2026-10-19, so they agree at the defaults; h/sLo is still
    // the one that decides, and the mechanism matters the moment either is changed.
    {
        Temperature_t lo = effectiveLow;
        Temperature_t hi = effectiveHigh;
        if (lo < SystemConstants::Temperature::MIN_BOILER_TEMP_C) {
            lo = SystemConstants::Temperature::MIN_BOILER_TEMP_C;
        }
        if (hi > SystemConstants::Temperature::MAX_BOILER_TEMP_C) {
            hi = SystemConstants::Temperature::MAX_BOILER_TEMP_C;
        }
        if (curveTarget < lo) {
            curveTarget = lo;
        } else if (curveTarget > hi) {
            curveTarget = hi;
        }
    }
    st.commanded = curveTarget;

    // Which bound decided it. h/sLo is the effective floor: setHeatingRequest clamps to it
    // after HeatingCurve has already clamped to h/bLo, so compare against that floor.
    if (st.commanded != 0) {
        if (st.rawTarget < st.commanded) {
            st.clamp = HeatingControlDiagnostics::Clamp::AT_FLOOR;
        } else if (st.rawTarget > st.commanded) {
            st.clamp = HeatingControlDiagnostics::Clamp::AT_CEIL;
        }
    }

    const uint32_t now = millis();
    const bool due = !everPublished || (now - lastPublishMs) >= DIAG_HEARTBEAT_MS;
    if (!due && !HeatingControlDiagnostics::differs(st, lastPublished)) {
        return;
    }

    char payload[160];
    if (HeatingControlDiagnostics::format(payload, sizeof(payload), st) == 0) {
        LOG_WARN(TAG, "Control diagnostics payload would not fit - skipped");
        return;
    }
    if (MQTTTask::publish(MQTT_STATUS_HEATING "/control", payload, 0, true,
                          MQTTPriority::PRIORITY_LOW)) {
        lastPublished = st;
        lastPublishMs = now;
        everPublished = true;
    }
}

static bool checkIfSpaceHeatingNeededEvent() {
    const char* TAG = "HeatingControlTask";

    // Check if PID auto-tuning is in progress - don't interrupt with ON/OFF cycling
    EventBits_t heatingBits = SRP::getHeatingEventBits();
    if (heatingBits & SystemEvents::HeatingEvent::AUTOTUNE_RUNNING) {
        // Auto-tuning in progress - maintain heating demand
        LOG_DEBUG(TAG, "Auto-tune active - maintaining heating demand");
        return true;
    }

    // Use actual heating state for hysteresis, not a static variable
    // This ensures correct behavior even after state changes from other sources
    bool currentlyHeating = (heatingState.state == HeatingOn);
    bool heatingNeeded = currentlyHeating; // Default to current state

    // H2 fix: Snapshot all settings fields under mutex BEFORE taking sensor mutex
    // Avoids nested Level-2 mutex acquisition and prevents races on Temperature_t/bool fields
    struct {
        bool useWeatherComp;
        Temperature_t outsideThreshold;
        Temperature_t targetInside;
        Temperature_t overheatMargin;
        Temperature_t hysteresis;
        bool overrideOff;
    } ss;
    if (SRP::takeSystemSettingsMutex(pdMS_TO_TICKS(50))) {
        const SystemSettings& s = SRP::getSystemSettings();
        ss.useWeatherComp     = s.useWeatherCompensatedControl;
        ss.outsideThreshold   = s.outsideTempHeatingThreshold;
        ss.targetInside       = s.targetTemperatureInside;
        ss.overheatMargin     = s.roomTempOverheatMargin;
        ss.hysteresis         = s.heating_hysteresis;
        ss.overrideOff        = s.heatingOverrideOff;
        SRP::giveSystemSettingsMutex();
    } else {
        LOG_ERROR(TAG, "Failed to acquire settings mutex - maintaining current heating state");
        return heatingNeeded;  // Fail-safe: keep current state
    }

    // F27: honour the DURABLE summer-mode / heating-OFF override. Previously the
    // override was only carried by the transient HEATING_OFF_OVERRIDE ControlRequest
    // bit, which ControlTask consumed (clearOnExit) before this task ever polled it -
    // so a remote "heating off" override was usually lost and never survived a reboot.
    // The persisted heatingOverrideOff flag is now consulted here, the single decision
    // point for both turn-on (HeatingOff state) and keep-running (HeatingOn state), so
    // the block is reliable regardless of the race and across reboots.
    if (ss.overrideOff) {
        g_blockReason = HeatingControlDiagnostics::Block::SUMMER_OVERRIDE;
        return false;  // heating blocked by override - not needed
    }

    // Get sensor readings with mutex protection
    if (SRP::takeSensorReadingsMutex(pdMS_TO_TICKS(100))) {
        SharedSensorReadings readings = SRP::getSensorReadings();

        if (ss.useWeatherComp) {
            // Weather-compensated mode:
            // - Outside temp determines ON/OFF (not room temp)
            // - Room temp only provides overheat protection (Begrenzung)
            // Resolve the outside reading FIRST: measured, held, or gone. See
            // OutsideTempFallbackPolicy for why the hold is an hour (outside temperature is the
            // slowest-moving input we have) and why "no outside reading" must not mean "no heat".
            const bool roomIsMeasurement = readings.isInsideTempValid;
            const OutsideTempFallbackPolicy::Decision outsideDec =
                OutsideTempFallbackPolicy::resolve(g_outsideSample, readings.isOutsideTempValid,
                                                   readings.outsideTemp, millis(),
                                                   roomIsMeasurement, ss.targetInside);
            if (outsideDec.source != g_lastOutsideSource) {
                LOG_WARN(TAG, "Outside reading %s -> %s",
                         OutsideTempFallbackPolicy::toString(g_lastOutsideSource),
                         OutsideTempFallbackPolicy::toString(outsideDec.source));
                g_lastOutsideSource = outsideDec.source;
            }

            if (OutsideTempFallbackPolicy::curveUsable(outsideDec.source)) {
                Temperature_t outsideTemp = outsideDec.outside;
                Temperature_t threshold = ss.outsideThreshold;  // H2: from snapshot

                // Outside cold enough for heating? Start below the threshold, stop only at
                // threshold + 1.0 °C (without hysteresis, readings around the threshold
                // toggled heating every cycle)
                bool outsideCold = SpaceHeatingPolicy::outsideColdForHeating(outsideTemp, threshold,
                                                                             currentlyHeating);

                // Room overheat protection (Begrenzung) WITH HYSTERESIS
                // Stop at: target + margin, Restart at: target + margin - hysteresis
                // This prevents short-cycling when room temp oscillates near limit
                //
                // The room value comes from the SAME resolver the curve uses, so a brief
                // Modbus dropout holds the backstop on the last measurement instead of
                // deleting it. Before 2026-10-25 this block was gated on
                // `readings.isInsideTempValid` and `roomOverheated` simply stayed false when
                // the sensor was invalid - which removed the only term holding heating off
                // and fired the burner when the sensor was pulled. See
                // SpaceHeatingPolicy::weatherDecision().
                RoomTempFallbackPolicy::Substitute room{readings.insideTemp,
                                                        RoomTempFallbackPolicy::Source::MEASURED};
                if (heatingState.heatingControl != nullptr) {
                    room = heatingState.heatingControl->resolveRoomForControl(readings,
                                                                             ss.targetInside);
                } else if (!readings.isInsideTempValid) {
                    // No module to ask: treat the reading as unusable rather than as a
                    // measurement, so the decision below still fails safe.
                    room.source = RoomTempFallbackPolicy::Source::TARGET;
                }

                const bool roomIsMeasurement = RoomTempFallbackPolicy::isMeasurement(room.source);
                const bool roomOverheated =
                    roomIsMeasurement && ss.targetInside > 0 &&
                    SpaceHeatingPolicy::roomAtBackstop(room.value, ss.targetInside,
                                                       ss.overheatMargin, ss.hysteresis,
                                                       currentlyHeating);

                const SpaceHeatingPolicy::WeatherDecision decision =
                    SpaceHeatingPolicy::weatherDecision(outsideCold, ss.targetInside > 0,
                                                        roomIsMeasurement, roomOverheated);

                if (decision.heat) {
                    heatingNeeded = true;
                    g_blockReason = HeatingControlDiagnostics::Block::OK;
                } else {
                    heatingNeeded = false;
                    // Outside gate FIRST. Corrected after the first hardware check
                    // (2026-10-18): with outside 20.1 C against a 13.0 threshold the topic
                    // reported "backstop", which is misleading twice over. The gate is the
                    // primary weather-mode decision - at 20 C outside the honest answer is
                    // "it is warm" - and `roomOverheated` does not mean what its name
                    // suggests while heating is OFF: it is then the RESTART test
                    // (room >= limit - hysteresis), so a room merely sitting in the
                    // deadband at 19.9 set it true. Reporting the room limit then invites
                    // the user to adjust a margin that is not the cause.
                    //
                    // ROOM_UNKNOWN reuses the existing SENSOR_INVALID reason ("sensor" on
                    // the wire), which already documents itself as "outside or room reading
                    // invalid - weather mode fails safe". Which sensor is unreadable is not
                    // ambiguous on the topic: `isrc` reads tgt/nom exactly when it is the
                    // room one.
                    switch (decision.block) {
                        case SpaceHeatingPolicy::WeatherBlock::OUTSIDE:
                            g_blockReason = HeatingControlDiagnostics::Block::OUTSIDE_GATE;
                            break;
                        case SpaceHeatingPolicy::WeatherBlock::ROOM_UNKNOWN:
                            g_blockReason = HeatingControlDiagnostics::Block::SENSOR_INVALID;
                            break;
                        case SpaceHeatingPolicy::WeatherBlock::ROOM_BACKSTOP:
                        case SpaceHeatingPolicy::WeatherBlock::NONE:
                            g_blockReason = HeatingControlDiagnostics::Block::ROOM_BACKSTOP;
                            break;
                    }
                    if (decision.block == SpaceHeatingPolicy::WeatherBlock::ROOM_UNKNOWN &&
                        currentlyHeating) {
                        LOG_WARN(TAG, "Heating stopped: no usable room reading (%s) - backstop "
                                      "cannot be evaluated",
                                 RoomTempFallbackPolicy::toString(room.source));
                    }
                    if (!outsideCold && currentlyHeating) {
                        char outBuf[16], threshBuf[16];
                        formatTemp(outBuf, sizeof(outBuf), outsideTemp);
                        formatTemp(threshBuf, sizeof(threshBuf),
                                   tempAdd(threshold, SpaceHeatingPolicy::OUTSIDE_THRESHOLD_HYSTERESIS));
                        LOG_INFO(TAG, "Heating not needed: outside %s°C >= stop limit %s°C (threshold + hysteresis)",
                                outBuf, threshBuf);
                    } else if (roomOverheated && currentlyHeating) {
                        char roomBuf[16], limitBuf[16];
                        // room.value, not readings.insideTemp: on a held reading those differ
                        // and the log has to name the number the decision actually used.
                        formatTemp(roomBuf, sizeof(roomBuf), room.value);
                        Temperature_t overheatLimit = tempAdd(ss.targetInside, ss.overheatMargin);
                        formatTemp(limitBuf, sizeof(limitBuf), overheatLimit);
                        LOG_INFO(TAG, "Heating stopped: room %s°C > overheat limit %s°C",
                                roomBuf, limitBuf);
                    }
                }
            } else if (OutsideTempFallbackPolicy::Source::ROOM_THERMOSTAT == outsideDec.source) {
                // NO USABLE OUTSIDE READING, BUT WE STILL KNOW THE ROOM AND ITS TARGET.
                //
                // This branch used to be `heatingNeeded = false` full stop, which stopped space
                // heating completely for as long as the hardware fault lasted and let an
                // occupied house settle into the 5-10 C frost band with a healthy room sensor
                // sitting unused. "Weather mode fails safe" means not firing when you cannot
                // tell if firing is safe - not abandoning the house when you still have exactly
                // the information a room thermostat runs on. Found 2026-10-28.
                //
                // Same predicate as the configured room mode below, deliberately shared rather
                // than copied. The flow target comes from the curve's own fallback (the floor),
                // so this is a conservative room thermostat, not full weather compensation.
                heatingNeeded = SpaceHeatingPolicy::roomThermostatWantsHeat(
                    currentlyHeating, readings.insideTemp, ss.targetInside, ss.hysteresis);
                g_blockReason = heatingNeeded ? HeatingControlDiagnostics::Block::OK
                                              : HeatingControlDiagnostics::Block::ROOM_SATISFIED;
                static uint32_t lastFallbackWarnMs = 0;
                const uint32_t nowMs = millis();
                if (lastFallbackWarnMs == 0 || (nowMs - lastFallbackWarnMs) >= 600000UL) {
                    lastFallbackWarnMs = nowMs;
                    char roomBuf[16], tgtBuf[16];
                    formatTemp(roomBuf, sizeof(roomBuf), readings.insideTemp);
                    formatTemp(tgtBuf, sizeof(tgtBuf), ss.targetInside);
                    LOG_ERROR(TAG, "OUTSIDE SENSOR UNAVAILABLE for over an hour - running as a "
                                   "room thermostat on %s°C against %s°C. CHECK THE OUTSIDE "
                                   "SENSOR WIRING.", roomBuf, tgtBuf);
                }
            } else {
                LOG_DEBUG(TAG, "No outside reading and no usable room reading - heating disabled");
                heatingNeeded = false;
                g_blockReason = HeatingControlDiagnostics::Block::SENSOR_INVALID;
            }
        } else {
            // Traditional room-temp ON/OFF mode (original behavior)
            if (readings.isInsideTempValid &&
                (ss.targetInside > 0)) {  // Ensure valid setpoint (H2: from snapshot)

                Temperature_t setpoint = ss.targetInside;    // H2: from snapshot
                Temperature_t currentTemp = readings.insideTemp;
                Temperature_t hysteresis = ss.hysteresis;    // H2: from snapshot

                // Determine heating state with ASYMMETRIC ABOVE hysteresis
                // Target is the minimum - heating starts when dropping below target,
                // stops when reaching target + hysteresis (slight overshoot)
                // e.g., target 20°C, hysteresis 0.5°C → oscillates 20.0°C to 20.5°C
                Temperature_t stopThreshold = tempAdd(setpoint, hysteresis);  // Turn OFF above this

                // One shared predicate (SpaceHeatingPolicy::roomThermostatWantsHeat) rather
                // than the rule inline here and again in the outside-fallback branch above.
                const bool wantsHeat = SpaceHeatingPolicy::roomThermostatWantsHeat(
                    currentlyHeating, currentTemp, setpoint, hysteresis);

                if (!currentlyHeating) {
                    // Currently OFF - turn ON if temperature drops below setpoint
                    if (wantsHeat) {
                        heatingNeeded = true;
                        g_blockReason = HeatingControlDiagnostics::Block::OK;
                    } else {
                        // OFF and the room is at or above target: the commonest steady state
                        // in room mode, and it used to write nothing at all - so `blk` on
                        // boiler/status/heating/control kept whatever reason happened to be
                        // there last, which after a boot was the initial value. Found by the
                        // review of c1ed1b0, which changed that initial value and made the
                        // staleness visible.
                        g_blockReason = HeatingControlDiagnostics::Block::ROOM_SATISFIED;
                    }
                } else {
                    // Currently ON - turn OFF if temperature rises above (setpoint + hysteresis)
                    if (!wantsHeat) {
                        heatingNeeded = false;
                        g_blockReason = HeatingControlDiagnostics::Block::ROOM_SATISFIED;
                        char currBuf[16], setBuf[16], threshBuf[16];
                        formatTemp(currBuf, sizeof(currBuf), currentTemp);
                        formatTemp(setBuf, sizeof(setBuf), setpoint);
                        formatTemp(threshBuf, sizeof(threshBuf), stopThreshold);
                        LOG_INFO(TAG, "Heating not needed: room %s°C >= stop %s°C (target %s°C)",
                                currBuf, threshBuf, setBuf);
                    }
                }
            } else {
                LOG_DEBUG(TAG, "Invalid sensor data or setpoint - heating not needed");
                heatingNeeded = false;
                g_blockReason = HeatingControlDiagnostics::Block::SENSOR_INVALID;
            }
        }

        SRP::giveSensorReadingsMutex();
    } else {
        LOG_ERROR(TAG, "Failed to acquire sensor mutex - maintaining current state");
    }

    return heatingNeeded;
}

// === External API for preemption notifications ===

TaskHandle_t getHeatingTaskHandle() {
    return heatingTaskHandle;
}

void notifyHeatingTaskPreempted() {
    // Wake the task immediately to handle preemption
    if (heatingTaskHandle != nullptr) {
        xTaskNotifyGive(heatingTaskHandle);
    }
}