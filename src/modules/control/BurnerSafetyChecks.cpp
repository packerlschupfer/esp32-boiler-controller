// src/modules/control/BurnerSafetyChecks.cpp
#include "BurnerSafetyChecks.h"
#include "BurnerStateMachine.h"
#include "modules/control/BurnerSystemController.h"
#include "modules/control/BurnerRequestManager.h"
#include "core/SystemResourceProvider.h"
#include <Arduino.h>
#include "events/SystemEventsGenerated.h"
#include <esp_log.h>
#include <atomic>

static const char* TAG = "BurnerSafety";

bool BurnerSafetyChecks::isFlameDetected() {
    // WARNING: No flame detection hardware installed
    // System assumes flame is present when burner relay is active

    // Round 14 Issue #2: Use atomic for thread-safe one-time log
    static std::atomic<bool> warningLogged{false};
    if (!warningLogged.exchange(true, std::memory_order_relaxed)) {
        LOG_DEBUG(TAG, "No flame detection sensor installed - assuming flame when burner active");
    }

    // Without a flame sensor, we assume flame is present when burner is active
    // In a real system, this would check an actual flame sensor
    // TODO: Integrate actual flame sensor when hardware is available

    BurnerSystemController* controller = SRP::getBurnerSystemController();
    return controller ? controller->isActive() : false;
}

bool BurnerSafetyChecks::checkSafetyConditions() {
    BurnerSystemController* controller = SRP::getBurnerSystemController();
    if (controller) {
        auto result = controller->performSafetyCheck();
        return result.isSuccess();
    }
    return false;  // Fail-safe: no controller = not safe
}

bool BurnerSafetyChecks::hasActiveModeDemand() {
    EventBits_t systemBits = xEventGroupGetBits(SRP::getSystemStateEventGroup());
    EventBits_t requestBits = BurnerRequestManager::getCurrentRequests();

    bool heatingActive = (systemBits & SystemEvents::SystemState::HEATING_ON) &&
                         (requestBits & SystemEvents::BurnerRequest::HEATING);
    bool waterActive = (systemBits & SystemEvents::SystemState::WATER_ON) &&
                       (requestBits & SystemEvents::BurnerRequest::WATER);

    return heatingActive || waterActive;
}
