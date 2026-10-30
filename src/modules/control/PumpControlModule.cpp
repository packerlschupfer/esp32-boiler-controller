// src/modules/control/PumpControlModule.cpp
// Unified pump control module for heating and water pumps
#include "PumpControlModule.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "core/SystemResourceProvider.h"
#include "core/SharedResourceManager.h"
#include "config/SystemConstants.h"
#include "config/SystemSettingsStruct.h"
#include "events/SystemEventsGenerated.h"
#include "modules/control/ReturnPreheater.h"  // For yield during preheating
#include "modules/control/EmergencyStopRelease.h"  // Emergency heat dissipation rule
#include "shared/RelayCommandPolicy.h"             // Pump request re-send rule
#include "shared/RelayState.h"                     // g_relayState (relay desired state)
#include "config/RelayIndices.h"
#include "core/StateManager.h"
#include "shared/SharedSensorReadings.h"
#include "LoggingMacros.h"
#include <MutexGuard.h>
#include <TaskManager.h>
#include <RuntimeStorage.h>

// Static state tracking
PumpState PumpControlModule::heatingPumpState_ = PumpState::Off;
PumpState PumpControlModule::waterPumpState_ = PumpState::Off;

// Mutex for thread-safe state access
SemaphoreHandle_t PumpControlModule::stateMutex_ = nullptr;
bool PumpControlModule::mutexInitialized_ = false;

void PumpControlModule::initMutexIfNeeded() {
    if (!mutexInitialized_) {
        stateMutex_ = xSemaphoreCreateMutex();
        if (stateMutex_ != nullptr) {
            mutexInitialized_ = true;
        }
    }
}

// Static configurations for each pump type
const PumpConfig PumpControlModule::heatingPumpConfig_ = {
    .modeActiveBit = SystemEvents::SystemState::HEATING_ON,
    .pumpOnStateBit = SystemEvents::SystemState::HEATING_PUMP_ON,
    .relayOnRequestBit = SystemEvents::RelayRequest::HEATING_PUMP_ON,
    .relayOffRequestBit = SystemEvents::RelayRequest::HEATING_PUMP_OFF,
    .startCounterId = rtstorage::COUNTER_HEATING_PUMP_STARTS,
    .taskName = "HeatingPump",
    .logTag = "HeatingPumpCtrl"
};

const PumpConfig PumpControlModule::waterPumpConfig_ = {
    .modeActiveBit = SystemEvents::SystemState::WATER_ON,
    .pumpOnStateBit = SystemEvents::SystemState::WATER_PUMP_ON,
    .relayOnRequestBit = SystemEvents::RelayRequest::WATER_PUMP_ON,
    .relayOffRequestBit = SystemEvents::RelayRequest::WATER_PUMP_OFF,
    .startCounterId = rtstorage::COUNTER_WATER_PUMP_STARTS,
    .taskName = "WaterPump",
    .logTag = "WaterPumpCtrl"
};

// Entry points that pass the appropriate config
void PumpControlModule::HeatingPumpTask(void* pvParameters) {
    (void)pvParameters;
    PumpControlTask(const_cast<PumpConfig*>(&heatingPumpConfig_));
}

void PumpControlModule::WaterPumpTask(void* pvParameters) {
    (void)pvParameters;
    PumpControlTask(const_cast<PumpConfig*>(&waterPumpConfig_));
}

// State accessors (thread-safe)
PumpState PumpControlModule::getHeatingPumpState() {
    initMutexIfNeeded();
    PumpState state = PumpState::Off;
    if (stateMutex_ && xSemaphoreTake(stateMutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
        state = heatingPumpState_;
        xSemaphoreGive(stateMutex_);
    }
    return state;
}

PumpState PumpControlModule::getWaterPumpState() {
    initMutexIfNeeded();
    PumpState state = PumpState::Off;
    if (stateMutex_ && xSemaphoreTake(stateMutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
        state = waterPumpState_;
        xSemaphoreGive(stateMutex_);
    }
    return state;
}

// Unified pump control task
void PumpControlModule::PumpControlTask(void* pvParameters) {
    const PumpConfig* config = static_cast<const PumpConfig*>(pvParameters);
    if (!config) {
        vTaskDelete(NULL);
        return;
    }

    const char* TAG = config->logTag;
    LOG_INFO(TAG, "Task started");

    // Initialize mutex for thread-safe state access
    initMutexIfNeeded();

    // Determine which state variable this task controls
    const bool isHeatingPump = (config == &heatingPumpConfig_);
    PumpState& currentStateRef = isHeatingPump ? heatingPumpState_ : waterPumpState_;

    // Register with watchdog - pumps are critical for proper circulation
    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::enabled(
        true,   // critical task
        10000   // 10 second timeout
    );

    if (!SRP::getTaskManager().registerCurrentTaskWithWatchdog(config->taskName, wdtConfig)) {
        LOG_ERROR(TAG, "Failed to register with watchdog");
    } else {
        LOG_INFO(TAG, "Registered with watchdog (10s timeout)");
    }

    // Get shared resources
    auto& resourceManager = SharedResourceManager::getInstance();
    EventGroupHandle_t systemStateEventGroup = resourceManager.getEventGroup(
        SharedResourceManager::EventGroups::SYSTEM_STATE);
    EventGroupHandle_t relayRequestEventGroup = resourceManager.getEventGroup(
        SharedResourceManager::EventGroups::RELAY_REQUEST);

    if (!systemStateEventGroup || !relayRequestEventGroup) {
        LOG_ERROR(TAG, "Failed to get required event groups");
        vTaskDelete(NULL);
        return;
    }

    PumpState lastLoggedState = PumpState::Off;

    // Pump overrun tracking - keep pump running after heating stops to dissipate residual heat
    bool wasModeActive = false;
    uint32_t overrunStartTime = 0;
    bool inOverrun = false;

    // Emergency heat dissipation (starts ON at every emergency stop onset)
    bool dissipating = true;

    // PER-TASK, NOT static. Both HeatingPumpTask and WaterPumpTask run this one body
    // (see :63-70), so a function-local static is ONE object shared by two tasks and an
    // edge detected by whichever ticks first is invisible to the other. That is exactly
    // what happened to the boiler-switch run-on added in 1f7fd33: the first pump to tick
    // consumed the switched-off edge and the second never started its overrun, leaving
    // the heat-carrying pump stopped with a stage-2 charge in a cast-iron block. Found by
    // the 2026-10-30 review; docs/SAFETY_SYSTEM.md called it closed. Initialised true so
    // a task that starts while the boiler is already off does not see a false edge.
    bool wasSystemEnabled = true;
    // Same reason: the latch onset must be stamped once per latch PER PUMP, or the second
    // pump inherits the first pump's clock for the blind-dissipation cap.
    uint32_t latchStartMs = 0;
    bool latchedLastCycle = false;
    bool dissipatingAfterRelease = false;
    uint32_t releaseTimeMs = 0;

    // Relay requests are re-sent while the relay does not follow this task's state
    uint32_t lastRelayRequestMs = 0;
    const uint8_t pumpRelayBit = static_cast<uint8_t>(
        RelayIndex::toPhysical(isHeatingPump ? RelayIndex::HEATING_PUMP : RelayIndex::WATER_PUMP) - 1);

    while (true) {
        // Feed watchdog
        (void)SRP::getTaskManager().feedWatchdog();

        // During preheating, let ReturnPreheater decide pump state (thermal shock mitigation)
        bool preheatingActive = isHeatingPump &&
            ReturnPreheater::getState() == ReturnPreheater::State::PREHEATING;

        // Check system state
        EventBits_t systemBits = xEventGroupGetBits(systemStateEventGroup);

        // Determine desired state based on system conditions
        PumpState desiredState = PumpState::Off;

        // Check if system is enabled first - if not, pump must be off (no overrun)
        bool systemEnabled = (systemBits & SystemEvents::SystemState::BOILER_ENABLED) != 0;

        // F13: an emergency stop clears BOILER_ENABLED but leaves residual heat in
        // the exchanger. The failsafe explicitly turns pumps ON for dissipation;
        // without this override PumpControlModule would compute Off (systemEnabled
        // false) and stop the pump within one 500ms cycle, defeating the failsafe.
        // While EMERGENCY_STOP is latched the pumps run until the boiler output is below
        // 60.0 °C (again from 65.0 °C, always without a usable reading).
        const bool latched = (systemBits & SystemEvents::SystemState::EMERGENCY_STOP) != 0;
        if (latchedLastCycle && !latched && dissipating) {
            // Released (emergency_reset or sensor recovery) while still dissipating:
            // continue until the boiler output has cooled
            dissipatingAfterRelease = true;
            releaseTimeMs = millis();
            LOG_WARN(TAG, "Emergency stop released - heat dissipation continues until boiler output < 60.0°C");
        }
        if (latched) {
            dissipatingAfterRelease = false;
        }
        // Onset time for the blind-dissipation cap below. Stamped once per latch, not per
        // cycle, or the cap would never be reached. Declared per-task above.
        if (latched && !latchedLastCycle) {
            latchStartMs = millis();
        }
        latchedLastCycle = latched;

        bool emergencyDissipation = false;
        if (latched || dissipatingAfterRelease) {
            bool outputUsable = false;
            Temperature_t output = 0;
            {
                MutexGuard guard(SRP::getSensorReadingsMutex(), pdMS_TO_TICKS(50));
                if (guard.hasLock()) {
                    const SharedSensorReadings& readings = SRP::getSensorReadings();
                    outputUsable = readings.isBoilerTempOutputValid;
                    output = readings.boilerTempOutput;
                }
            }
            outputUsable = outputUsable &&
                           !StateManager::isSensorStale(StateManager::SensorChannel::BOILER_OUTPUT);

            if (latched) {
                const bool wasDissipating = dissipating;
                dissipating = EmergencyStopRelease::dissipationPumpOn(
                    outputUsable, output, dissipating, millis() - latchStartMs);
                if (wasDissipating != dissipating) {
                    LOG_WARN(TAG, "Emergency heat dissipation %s (boiler output %d.%d°C)",
                             dissipating ? "resumed" : "done", output / 10, abs(output % 10));
                }
                emergencyDissipation = dissipating;
            } else {
                dissipatingAfterRelease = EmergencyStopRelease::dissipationAfterRelease(
                    outputUsable, output, millis() - releaseTimeMs, SRP::getSystemSettings().pumpCooldownMs);
                if (!dissipatingAfterRelease) {
                    LOG_WARN(TAG, "Heat dissipation after release done (boiler output %d.%d°C%s)",
                             output / 10, abs(output % 10), outputUsable ? "" : ", no usable reading");
                }
                emergencyDissipation = dissipatingAfterRelease;
            }
        }
        if (!latched) {
            dissipating = true;  // next onset starts dissipating
        }

        // Pump should be on if system is enabled AND in the appropriate mode
        bool modeActive = (systemBits & config->modeActiveBit) != 0;

        // SWITCHING THE BOILER OFF MID-BURN MUST STILL GET A COOLDOWN.
        //
        // Clearing BOILER_ENABLED (the HA boiler switch, or params/set system/boilerEnabled)
        // stops the burner at once - min on-time is bypassed - and used to stop BOTH pumps
        // within one 500 ms cycle with no overrun, because all three branches below were gated
        // on systemEnabled and the only override required an EMERGENCY_STOP latch. Nothing
        // restarted them afterwards either: frost protection requires BOILER_ENABLED, and even
        // the 115 C critical check is skipped while it is clear. So a 34 kW stage-2 burn at
        // 72 C flow could be followed by zero circulation, with the after-heat of a 169 kg
        // cast-iron block soaking into standing water. Every OTHER stop path has run-on
        // (switch and demand end: pumpCooldownMs; emergency: forced dissipation to 60 C), and
        // POST_PURGE's own comment asserts "the pumps carry the heat away independently" -
        // false for exactly this one stop reason. Found by the 2026-10-28 review; it dates to
        // the initial release and predates the 2026-09-14 emergency-dissipation fix.
        //
        // Safe for the drained-for-service case, which is a real reason to switch the boiler
        // off: the overrun only starts if this pump's mode WAS active, and a drained system
        // was not circulating or burning.
        // wasSystemEnabled is declared per-task above - NOT static. See the note there.
        const bool boilerJustSwitchedOff = wasSystemEnabled && !systemEnabled;
        wasSystemEnabled = systemEnabled;

        if (systemEnabled && modeActive) {
            desiredState = PumpState::On;
            inOverrun = false;  // Clear overrun when mode becomes active again
        } else if ((systemEnabled && wasModeActive && !modeActive) ||
                   (boilerJustSwitchedOff && (modeActive || wasModeActive))) {
            // Mode just turned off, OR the boiler was switched off while this pump was serving
            // an active mode - both leave residual heat that needs carrying away.
            overrunStartTime = millis();
            inOverrun = true;
            SystemSettings& settings = SRP::getSystemSettings();
            LOG_INFO(TAG, "Starting pump overrun (%lu ms) to dissipate residual heat%s",
                     settings.pumpCooldownMs,
                     boilerJustSwitchedOff ? " (boiler switched off mid-operation)" : "");
        }

        // Check if we're in overrun period. Deliberately NOT gated on systemEnabled: an
        // overrun that has already started is carrying real heat out of the exchanger, and the
        // boiler being switched off is not a reason to abandon it halfway.
        if (inOverrun) {
            uint32_t elapsed = millis() - overrunStartTime;
            // Use configurable cooldown from SystemSettings
            SystemSettings& settings = SRP::getSystemSettings();
            if (elapsed < settings.pumpCooldownMs) {
                // Still in overrun - keep pump running
                desiredState = PumpState::On;
            } else {
                // Overrun complete
                LOG_INFO(TAG, "Pump overrun complete - stopping pump after %lu ms",
                         settings.pumpCooldownMs);
                inOverrun = false;
            }
        }

        // Track mode state for next iteration
        wasModeActive = modeActive;

        // Override: During preheating, ReturnPreheater controls pump cycling
        if (preheatingActive) {
            desiredState = ReturnPreheater::shouldPumpBeOn() ? PumpState::On : PumpState::Off;
        }

        // F13: emergency heat-dissipation has final say - keep the pump running
        // while the emergency-stop latch is set (or was just released) and the boiler
        // is still hot, regardless of enabled state, overrun, or preheating.
        if (emergencyDissipation) {
            desiredState = PumpState::On;
            inOverrun = false;
        }

        // Read current state with mutex protection
        PumpState currentState = PumpState::Off;
        if (stateMutex_ && xSemaphoreTake(stateMutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
            currentState = currentStateRef;
            xSemaphoreGive(stateMutex_);
        }

        // Update state if changed
        // Motor protection is enforced at RelayControlTask layer (SafetyConfig::pumpProtectionMs)
        if (desiredState != currentState) {
            LOG_INFO(TAG, "State change: %s -> %s",
                     currentState == PumpState::On ? "ON" : "OFF",
                     desiredState == PumpState::On ? "ON" : "OFF");

            // Update logical state with mutex protection
            if (stateMutex_ && xSemaphoreTake(stateMutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
                currentStateRef = desiredState;
                xSemaphoreGive(stateMutex_);
            }

            // Request relay change via event bits
            // RelayControlTask will enforce motor protection timing
            if (desiredState == PumpState::On) {
                xEventGroupSetBits(relayRequestEventGroup, config->relayOnRequestBit);
                xEventGroupSetBits(systemStateEventGroup, config->pumpOnStateBit);

                // Increment pump start counter in FRAM
                rtstorage::RuntimeStorage* storage = SRP::getRuntimeStorage();
                if (storage) {
                    if (storage->incrementCounter(config->startCounterId)) {
                        uint32_t count = storage->getCounter(config->startCounterId);
                        LOG_INFO(TAG, "Pump start count: %lu", count);
                    }
                }
            } else {
                xEventGroupSetBits(relayRequestEventGroup, config->relayOffRequestBit);
                xEventGroupClearBits(systemStateEventGroup, config->pumpOnStateBit);
            }
            lastRelayRequestMs = millis();
        } else if (RelayCommandPolicy::pumpRequestResendDue(desiredState == PumpState::On,
                                                            g_relayState.getRelay(pumpRelayBit),
                                                            millis(), lastRelayRequestMs)) {
            // The relay did not follow: request refused by pump motor protection (the
            // request bit is cleared regardless) or relay switched directly by the failsafe
            xEventGroupSetBits(relayRequestEventGroup, desiredState == PumpState::On
                                                           ? config->relayOnRequestBit
                                                           : config->relayOffRequestBit);
            lastRelayRequestMs = millis();
            LOG_DEBUG(TAG, "Relay does not match pump state %s - re-sending request",
                      desiredState == PumpState::On ? "ON" : "OFF");
        }

        // Debug log on state change (less frequent)
        if (lastLoggedState != currentState) {
            #if defined(LOG_MODE_DEBUG_SELECTIVE) || defined(LOG_MODE_DEBUG_FULL)
            LOG_DEBUG(TAG, "Pump state: %s (mode: %s)",
                     currentState == PumpState::On ? "ON" : "OFF",
                     modeActive ? "ACTIVE" : "INACTIVE");
            #endif
            lastLoggedState = currentState;
        }

        vTaskDelay(pdMS_TO_TICKS(SystemConstants::Timing::PUMP_CHECK_INTERVAL_MS));
    }
}
