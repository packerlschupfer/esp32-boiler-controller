// src/modules/tasks/BoilerTempControlTask.cpp
// Boiler temperature control task - cascade control inner loop
#include "modules/tasks/BoilerTempControlTask.h"

#include "config/SystemConstants.h"
#include "modules/control/BoilerTempController.h"
#include "modules/control/BurnerStateMachine.h"
#include "modules/control/BurnerRequestManager.h"
#include "shared/SharedResources.h"
#include "events/SystemEventsGenerated.h"
#include "core/SystemResourceProvider.h"
#include "core/StateManager.h"
#include "config/SystemSettingsStruct.h"  // For SystemSettings full definition
#include "utils/ResourceGuard.h"  // For TaskCleanupHandler
#include "modules/tasks/PersistentStorageTask.h"  // For NVS save
#include "modules/tasks/MQTTTask.h"  // For MQTT publish
#include "LoggingMacros.h"
#include <TaskManager.h>

static const char* TAG = "BoilerTempCtrl";

// Task handle for external access
static TaskHandle_t taskHandle = nullptr;

// Cached event group handles
static struct {
    EventGroupHandle_t sensorEventGroup = nullptr;
    EventGroupHandle_t burnerRequestEventGroup = nullptr;
    bool initialized = false;
} cachedHandles;

// Controller instance
static BoilerTempController controller;

// Statistics tracking
static struct {
    uint32_t cycleCount = 0;
    uint32_t powerChanges = 0;
    uint32_t lastCycleTime = 0;
} stats;

TaskHandle_t getBoilerTempControlTaskHandle() {
    return taskHandle;
}

BoilerTempController* getBoilerTempController() {
    return controller.isInitialized() ? &controller : nullptr;
}

void BoilerTempControlTask(void* parameter) {
    (void)parameter;

    LOG_INFO(TAG, "Started C%d Stk:%d", xPortGetCoreID(), uxTaskGetStackHighWaterMark(NULL));

    // Cache event group handles
    if (!cachedHandles.initialized) {
        cachedHandles.sensorEventGroup = SRP::getSensorEventGroup();
        cachedHandles.burnerRequestEventGroup = SRP::getBurnerRequestEventGroup();

        if (!cachedHandles.sensorEventGroup || !cachedHandles.burnerRequestEventGroup) {
            LOG_ERROR(TAG, "Failed to get event group handles");
            vTaskDelete(NULL);
            return;
        }
        cachedHandles.initialized = true;
    }

    // Initialize the controller
    if (!controller.initialize()) {
        LOG_ERROR(TAG, "Failed to initialize BoilerTempController");
        vTaskDelete(NULL);
        return;
    }

    // Register with watchdog - use 2x sensor interval plus margin
    constexpr uint32_t WATCHDOG_TIMEOUT_MS =
        SystemConstants::Timing::MB8ART_SENSOR_READ_INTERVAL_MS * 2 + 5000;

    if (!SRP::getTaskManager().feedWatchdog()) {
        LOG_WARN(TAG, "Initial watchdog feed failed");
    }
    LOG_INFO(TAG, "WDT OK %lums", WATCHDOG_TIMEOUT_MS);

    // Register cleanup handler
    TaskCleanupHandler::registerCleanup([&]() {
        LOG_WARN(TAG, "BoilerTempControlTask cleanup");
        controller.reset();
    });

    LOG_INFO(TAG, "Entering main loop - waiting for sensor updates");

    // Main control loop
    while (true) {
        // Wait for boiler output temperature sensor update
        // This synchronizes our control loop with the sensor read interval (~2.5s)
        EventBits_t bits = xEventGroupWaitBits(
            cachedHandles.sensorEventGroup,
            SystemEvents::SensorUpdate::BOILER_OUTPUT,
            pdTRUE,  // Clear on exit
            pdFALSE, // Wait for any bit
            pdMS_TO_TICKS(SystemConstants::Timing::MB8ART_SENSOR_READ_INTERVAL_MS * 2)
        );

        // Feed watchdog
        (void)SRP::getTaskManager().feedWatchdog();

        // Check if we got a sensor update or timeout
        if (!(bits & SystemEvents::SensorUpdate::BOILER_OUTPUT)) {
            LOG_DEBUG(TAG, "Timeout waiting for sensor - checking state anyway");
        }

        // Check for auto-tuning requests
        EventBits_t controlBits = SRP::getControlRequestsEventBits();

        if (controlBits & SystemEvents::ControlRequest::PID_AUTOTUNE) {
            SRP::clearControlRequestsEventBits(SystemEvents::ControlRequest::PID_AUTOTUNE);

            // Get current boiler target from any active request
            Temperature_t tuneSetpoint = BurnerRequestManager::getCurrentTargetTemp();
            if (tuneSetpoint == 0 || tuneSetpoint == TEMP_INVALID) {
                // Default to 55°C if no active request
                tuneSetpoint = tempFromFloat(55.0f);
            }

            if (controller.startAutoTuning(tuneSetpoint)) {
                SRP::setHeatingEventBits(SystemEvents::HeatingEvent::AUTOTUNE_RUNNING);
                SRP::clearHeatingEventBits(SystemEvents::HeatingEvent::AUTOTUNE_COMPLETE |
                                           SystemEvents::HeatingEvent::AUTOTUNE_FAILED);
            } else {
                SRP::setHeatingEventBits(SystemEvents::HeatingEvent::AUTOTUNE_FAILED);
                LOG_ERROR(TAG, "Failed to start auto-tuning");
            }
        }

        if (controlBits & SystemEvents::ControlRequest::PID_AUTOTUNE_STOP) {
            SRP::clearControlRequestsEventBits(SystemEvents::ControlRequest::PID_AUTOTUNE_STOP);
            controller.stopAutoTuning();
            SRP::clearHeatingEventBits(SystemEvents::HeatingEvent::AUTOTUNE_RUNNING);
            LOG_INFO(TAG, "Auto-tuning stopped by user");
        }

        // Handle auto-tuning if active
        if (controller.isAutoTuning()) {
            // SAFETY: Check if there's still an active heating/water request
            // Auto-tuning MUST stop if heating is disabled to prevent running
            // burner without pump (pump control follows HEATING_ON bit)
            EventBits_t requestBits = BurnerRequestManager::getCurrentRequests();
            bool hasActiveRequest = (requestBits & SystemEvents::BurnerRequest::HEATING) ||
                                   (requestBits & SystemEvents::BurnerRequest::WATER);

            if (!hasActiveRequest) {
                LOG_WARN(TAG, "No active heating request - stopping auto-tuning for safety");
                controller.stopAutoTuning();
                SRP::clearHeatingEventBits(SystemEvents::HeatingEvent::AUTOTUNE_RUNNING);
                SRP::setHeatingEventBits(SystemEvents::HeatingEvent::AUTOTUNE_FAILED);
                stats.cycleCount++;
                continue;
            }

            SharedSensorReadings readings = StateManager::getSensorReadingsCopy();
            if (readings.isBoilerTempOutputValid) {
                auto output = controller.updateAutoTuning(readings.boilerTempOutput);

                // Apply the auto-tuner's relay output
                // During auto-tuning, we MUST actively control the burner for oscillations
                if (output.changed) {
                    Temperature_t tuneTarget = BurnerRequestManager::getCurrentTargetTemp();
                    if (tuneTarget == 0 || tuneTarget == TEMP_INVALID) {
                        tuneTarget = tempFromFloat(55.0f);  // Default
                    }

                    if (output.powerLevel == BoilerTempController::PowerLevel::OFF) {
                        // Auto-tuner wants burner OFF - actively stop it
                        BurnerStateMachine::setHeatDemand(false, tuneTarget, false);
                        LOG_INFO(TAG, "Autotune: Burner OFF");
                    } else {
                        // Auto-tuner wants burner ON at FULL power
                        BurnerStateMachine::setHeatDemand(true, tuneTarget, true);
                        LOG_INFO(TAG, "Autotune: Burner FULL");
                    }
                }

                // Check if tuning completed
                if (!controller.isAutoTuning()) {
                    SRP::clearHeatingEventBits(SystemEvents::HeatingEvent::AUTOTUNE_RUNNING);

                    if (controller.applyAutoTuningResults()) {
                        SRP::setHeatingEventBits(SystemEvents::HeatingEvent::AUTOTUNE_COMPLETE);

                        // Get the tuned gains
                        float kp, ki, kd;
                        if (controller.getTunedGains(kp, ki, kd)) {
                            // Update SystemSettings for persistence
                            SystemSettings& settings = SRP::getSystemSettings();

                            // Save to correct gain set based on current mode
                            if (controller.isWaterMode()) {
                                settings.wHeaterKp = kp;
                                settings.wHeaterKi = ki;
                                settings.wHeaterKd = kd;
                                LOG_INFO(TAG, "Auto-tuning complete - WATER heating PID gains saved");
                            } else {
                                settings.spaceHeatingKp = kp;
                                settings.spaceHeatingKi = ki;
                                settings.spaceHeatingKd = kd;
                                LOG_INFO(TAG, "Auto-tuning complete - SPACE heating PID gains saved");
                            }

                            // Request NVS save
                            PersistentStorageTask_RequestSave();

                            // Publish MQTT notification with mode
                            char buffer[160];
                            snprintf(buffer, sizeof(buffer),
                                "{\"status\":\"complete\",\"mode\":\"%s\",\"kp\":%.4f,\"ki\":%.5f,\"kd\":%.4f}",
                                controller.isWaterMode() ? "water" : "space",
                                kp, ki, kd);
                            MQTTTask::publish("boiler/status/pid/autotune/result", buffer, 0, true,
                                MQTTPriority::PRIORITY_HIGH);
                        }

                        LOG_INFO(TAG, "Auto-tuning complete - PID gains applied");
                    } else {
                        SRP::setHeatingEventBits(SystemEvents::HeatingEvent::AUTOTUNE_FAILED);
                        MQTTTask::publish("boiler/status/pid/autotune/result",
                            "{\"status\":\"failed\"}", 0, true, MQTTPriority::PRIORITY_HIGH);
                        LOG_ERROR(TAG, "Auto-tuning completed but failed to apply results");
                    }
                }
            }

            // Continue loop - don't process normal control during tuning
            stats.cycleCount++;
            continue;
        }

        // Check if there's an active burner request
        EventBits_t requestBits = BurnerRequestManager::getCurrentRequests();

        // Only process if there's an active heating or water request
        bool hasActiveRequest = (requestBits & SystemEvents::BurnerRequest::HEATING) ||
                               (requestBits & SystemEvents::BurnerRequest::WATER);

        if (!hasActiveRequest) {
            // No active request - controller should not interfere
            // The HeatingControlModule/WheaterControlModule handle request clearing
            stats.cycleCount++;
            continue;
        }

        // Get target temperature from BurnerRequest
        Temperature_t targetTemp = BurnerRequestManager::getCurrentTargetTemp();

        // Get current boiler output temperature
        SharedSensorReadings readings = StateManager::getSensorReadingsCopy();

        // Validate sensor data
        if (!readings.isBoilerTempOutputValid) {
            LOG_WARN(TAG, "Boiler output temp invalid - skipping cycle");
            stats.cycleCount++;
            continue;
        }

        Temperature_t currentTemp = readings.boilerTempOutput;

        // Check for sensor staleness
        if (StateManager::isSensorStale(StateManager::SensorChannel::BOILER_OUTPUT)) {
            LOG_WARN(TAG, "Boiler output temp stale - skipping cycle");
            stats.cycleCount++;
            continue;
        }

        // Check and update mode (switches PID gains if mode changed)
        controller.updateMode();

        // Calculate control output
        auto output = controller.calculate(targetTemp, currentTemp);

        // Update BurnerStateMachine if output changed
        if (output.changed) {
            // Convert our PowerLevel to highPower flag
            bool highPower = (output.powerLevel == BoilerTempController::PowerLevel::FULL);

            // When power level is OFF (temp above target):
            // - Turn off burner via setHeatDemand(false)
            // - Pump continues because PumpControlModule watches HEATING_ON bit
            //   which is set by HeatingControlTask (room temp control), not by burner
            // - Heat distributes until room reaches target or boiler temp drops

            if (output.powerLevel == BoilerTempController::PowerLevel::OFF) {
                // Above target - turn off burner, pump continues via PumpControlModule
                BurnerStateMachine::setHeatDemand(false, targetTemp, false);
                LOG_INFO(TAG, "Coasting - burner OFF (target:%.1f curr:%.1f) - pump continues",
                         tempToFloat(targetTemp),
                         tempToFloat(currentTemp));
            } else {
                // HALF or FULL - update BurnerStateMachine with power level
                // Pump control is independent (PumpControlModule watches HEATING_ON bit)
                BurnerStateMachine::setHeatDemand(true, targetTemp, highPower);

                LOG_INFO(TAG, "Power: %s (target:%.1f curr:%.1f)",
                         BoilerTempController::powerLevelToString(output.powerLevel),
                         tempToFloat(targetTemp),
                         tempToFloat(currentTemp));
            }

            stats.powerChanges++;
        }

        stats.cycleCount++;
        stats.lastCycleTime = millis();

        // Log status periodically (every ~30 seconds = 12 cycles at 2.5s)
        if (stats.cycleCount % 12 == 0) {
            [[maybe_unused]] auto lastOutput = controller.getLastOutput();
            LOG_DEBUG(TAG, "Status: cycles=%lu changes=%lu power=%s target=%.1f curr=%.1f",
                     stats.cycleCount,
                     stats.powerChanges,
                     BoilerTempController::powerLevelToString(lastOutput.powerLevel),
                     tempToFloat(targetTemp),
                     tempToFloat(currentTemp));
        }
    }
}
