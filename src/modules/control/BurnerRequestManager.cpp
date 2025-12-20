// src/modules/control/BurnerRequestManager.cpp
#include "modules/control/BurnerRequestManager.h"
#include "core/SystemResourceProvider.h"
#include "config/SystemConstants.h"
#include "utils/MutexRetryHelper.h"
#include "LoggingMacros.h"
#include "events/TemperatureEventHelpers.h"
#include "shared/Temperature.h"
#include "modules/tasks/HeatingControlTask.h"
#include "modules/tasks/WheaterControlTask.h"
#include <algorithm>

static const char* TAG = "BurnerRequestManager";

// Static member definitions
SemaphoreHandle_t BurnerRequestManager::requestMutex = nullptr;
bool BurnerRequestManager::initialized = false;
uint32_t BurnerRequestManager::lastHeatingRequestTime = 0;
uint32_t BurnerRequestManager::lastWaterRequestTime = 0;

void BurnerRequestManager::initialize() {
    if (initialized) {
        LOG_WARN(TAG, "Already initialized");
        return;
    }

    requestMutex = xSemaphoreCreateMutex();
    if (!requestMutex) {
        LOG_ERROR(TAG, "Failed to create request mutex");
        return;
    }

    // Initialize timestamps to current time to avoid false expiration on first request
    // Without this, a request made immediately after boot would be flagged as expired
    // because lastHeatingRequestTime == 0 is treated as "no timestamp"
    uint32_t now = millis();
    lastHeatingRequestTime = now;
    lastWaterRequestTime = now;

    initialized = true;
    LOG_INFO(TAG, "Burner request manager initialized");
}

void BurnerRequestManager::cleanup() {
    if (requestMutex != nullptr) {
        vSemaphoreDelete(requestMutex);
        requestMutex = nullptr;
    }
    lastHeatingRequestTime = 0;
    lastWaterRequestTime = 0;
    initialized = false;
    LOG_INFO(TAG, "Burner request manager cleaned up");
}

EventGroupHandle_t BurnerRequestManager::getBurnerRequestEventGroup() {
    return SRP::getBurnerRequestEventGroup();
}

bool BurnerRequestManager::atomicUpdateBits(EventBits_t setBits, EventBits_t clearBits) {
    if (!initialized || !requestMutex) {
        LOG_ERROR(TAG, "Not initialized");
        return false;
    }

    auto guard = MutexRetryHelper::acquireGuard(
        requestMutex,
        "BurnerRequest-AtomicUpdate",
        pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)
    );
    if (!guard) {
        LOG_ERROR(TAG, "Failed to acquire mutex");
        return false;
    }

    EventGroupHandle_t eventGroup = getBurnerRequestEventGroup();
    if (!eventGroup) {
        LOG_ERROR(TAG, "Failed to get event group");
        return false;
    }

    // Atomic read-modify-write
    EventBits_t currentBits = xEventGroupGetBits(eventGroup);
    EventBits_t newBits = (currentBits & ~clearBits) | setBits;

    // Check if anything actually changed
    bool changed = (newBits != currentBits);

    if (changed) {
        // Set change event bits BEFORE updating the request
        // Determine which change events to set based on what's changing
        EventBits_t changeEvents = SystemEvents::BurnerRequest::CHANGED;

        // Check if heating request changed
        if ((currentBits & SystemEvents::BurnerRequest::HEATING) != (newBits & SystemEvents::BurnerRequest::HEATING)) {
            changeEvents |= SystemEvents::BurnerRequest::HEATING_CHANGED;
        }

        // Check if water request changed
        if ((currentBits & SystemEvents::BurnerRequest::WATER) != (newBits & SystemEvents::BurnerRequest::WATER)) {
            changeEvents |= SystemEvents::BurnerRequest::WATER_CHANGED;
        }

        // Define state bits (everything except change event bits)
        const EventBits_t STATE_BITS = SystemEvents::BurnerRequest::HEATING |
                                       SystemEvents::BurnerRequest::WATER |
                                       SystemEvents::BurnerRequest::POWER_LOW |
                                       SystemEvents::BurnerRequest::POWER_HIGH |
                                       SystemEvents::BurnerRequest::TEMPERATURE_MASK;

        if (changeEvents != 0) {
            LOG_DEBUG(TAG, "Setting change event bits: 0x%06X", changeEvents);
        }

        // Atomic clear-and-set using critical section to prevent TOCTOU race
        // Without this, readers could see empty state between clear and set
        portMUX_TYPE spinlock = portMUX_INITIALIZER_UNLOCKED;
        portENTER_CRITICAL(&spinlock);
        xEventGroupClearBits(eventGroup, STATE_BITS);
        if (newBits != 0 || changeEvents != 0) {
            xEventGroupSetBits(eventGroup, newBits | changeEvents);
        }
        portEXIT_CRITICAL(&spinlock);
    }

    if (changed) {
        LOG_DEBUG(TAG, "Updated bits: 0x%06X -> 0x%06X (changed)", currentBits, newBits);
    }
    return changed;
}

bool BurnerRequestManager::setHeatingRequest(Temperature_t targetTemp, bool highPower) {
    // Two-level validation: Mode-specific limits, then absolute safety
    SystemSettings& settings = SRP::getSystemSettings();

    // Level 1: Space heating operational limits (mode-specific)
    Temperature_t minTemp = settings.heating_low_limit;   // e.g., 40°C
    Temperature_t maxTemp = settings.heating_high_limit;  // e.g., 75°C

    // Level 2: Enforce absolute safety limits (constants)
    if (minTemp < SystemConstants::Temperature::MIN_BOILER_TEMP_C) {
        minTemp = SystemConstants::Temperature::MIN_BOILER_TEMP_C;  // 20°C hard floor
    }
    if (maxTemp > SystemConstants::Temperature::MAX_BOILER_TEMP_C) {
        maxTemp = SystemConstants::Temperature::MAX_BOILER_TEMP_C;  // 110°C hard ceiling
    }

    // Clamp target to validated range
    if (targetTemp < minTemp) targetTemp = minTemp;
    if (targetTemp > maxTemp) targetTemp = maxTemp;

    EventBits_t setBits = SystemEvents::BurnerRequest::HEATING | SystemEvents::BurnerRequest::encode_temperature_t(targetTemp);
    if (highPower) {
        setBits |= SystemEvents::BurnerRequest::POWER_HIGH;
    } else {
        setBits |= SystemEvents::BurnerRequest::POWER_LOW;
    }

    // Check if we're preempting water heating
    EventGroupHandle_t eventGroup = getBurnerRequestEventGroup();
    EventBits_t currentBits = xEventGroupGetBits(eventGroup);
    bool wasWaterActive = (currentBits & SystemEvents::BurnerRequest::WATER) != 0;

    // Also check SystemState for water heating active
    EventBits_t systemBits = xEventGroupGetBits(SRP::getSystemStateEventGroup());
    bool waterStateActive = (systemBits & SystemEvents::SystemState::WATER_ON) != 0;

    LOG_DEBUG(TAG, "setHeatingRequest: burnerBits=0x%08lX, WATER_REQ=%d, WATER_STATE=%d",
             (unsigned long)currentBits, wasWaterActive ? 1 : 0, waterStateActive ? 1 : 0);

    // Clear water bits and old temperature
    EventBits_t clearBits = SystemEvents::BurnerRequest::WATER |
                           SystemEvents::BurnerRequest::TEMPERATURE_MASK | SystemEvents::BurnerRequest::POWER_BITS;

    char tempStr[16];
    formatTemp(tempStr, sizeof(tempStr), targetTemp);
    LOG_DEBUG(TAG, "Setting heating request: %s°C, %s power",
            tempStr, highPower ? "HIGH" : "LOW");

    // Record request timestamp for watchdog
    lastHeatingRequestTime = millis();

    // If preempting water, clear WATER_ON state BEFORE clearing request bit
    // This prevents timing window where request is cleared but state is still set
    if (wasWaterActive || waterStateActive) {
        LOG_INFO(TAG, "Heating preempting water - clearing water state");
        SRP::clearSystemStateEventBits(SystemEvents::SystemState::WATER_ON);
        // Wake water task immediately to handle preemption
        notifyWheaterTaskPreempted();
    }

    return atomicUpdateBits(setBits, clearBits);
}

bool BurnerRequestManager::setWaterRequest(Temperature_t targetTemp, bool highPower) {
    // Two-level validation: Mode-specific limits, then absolute safety
    // Allows proper water heating with charge delta (tank + 10°C)
    SystemSettings& settings = SRP::getSystemSettings();

    // Level 1: Water heating operational limits (mode-specific)
    Temperature_t minTemp = settings.water_heating_low_limit;   // e.g., 40°C
    Temperature_t maxTemp = settings.water_heating_high_limit;  // e.g., 90°C

    // Level 2: Enforce absolute safety limits (constants)
    if (minTemp < SystemConstants::Temperature::MIN_BOILER_TEMP_C) {
        minTemp = SystemConstants::Temperature::MIN_BOILER_TEMP_C;  // 20°C hard floor
    }
    if (maxTemp > SystemConstants::Temperature::MAX_BOILER_TEMP_C) {
        maxTemp = SystemConstants::Temperature::MAX_BOILER_TEMP_C;  // 110°C hard ceiling
    }

    // Clamp target to validated range
    if (targetTemp < minTemp) targetTemp = minTemp;
    if (targetTemp > maxTemp) targetTemp = maxTemp;

    EventBits_t setBits = SystemEvents::BurnerRequest::WATER | SystemEvents::BurnerRequest::encode_temperature_t(targetTemp);
    if (highPower) {
        setBits |= SystemEvents::BurnerRequest::POWER_HIGH;
    } else {
        setBits |= SystemEvents::BurnerRequest::POWER_LOW;
    }

    // Check if we're preempting heating (when water has priority)
    EventGroupHandle_t eventGroup = getBurnerRequestEventGroup();
    EventBits_t currentBits = xEventGroupGetBits(eventGroup);
    bool wasHeatingActive = (currentBits & SystemEvents::BurnerRequest::HEATING) != 0;

    EventBits_t systemBits = xEventGroupGetBits(SRP::getSystemStateEventGroup());
    bool priority = (systemBits & SystemEvents::SystemState::WATER_PRIORITY) != 0;
    bool heatingStateActive = (systemBits & SystemEvents::SystemState::HEATING_ON) != 0;

    // Clear heating bit and old temperature if water has priority
    EventBits_t clearBits = SystemEvents::BurnerRequest::TEMPERATURE_MASK | SystemEvents::BurnerRequest::POWER_BITS;
    if (priority) {
        clearBits |= SystemEvents::BurnerRequest::HEATING;
    }

    char tempStr[16];
    formatTemp(tempStr, sizeof(tempStr), targetTemp);
    LOG_DEBUG(TAG, "Setting water request: %s°C, %s power, priority=%s",
            tempStr, highPower ? "HIGH" : "LOW", priority ? "YES" : "NO");

    // Record request timestamp for watchdog
    lastWaterRequestTime = millis();

    // If preempting heating (priority mode), clear HEATING_ON state BEFORE clearing request bit
    // This prevents timing window where request is cleared but state is still set
    if (priority && (wasHeatingActive || heatingStateActive)) {
        LOG_INFO(TAG, "Water (priority) preempting heating - clearing heating state");
        SRP::clearSystemStateEventBits(SystemEvents::SystemState::HEATING_ON);
        // Wake heating task immediately to handle preemption
        notifyHeatingTaskPreempted();
    }

    return atomicUpdateBits(setBits, clearBits);
}

bool BurnerRequestManager::clearRequest(RequestSource source) {
    EventBits_t clearBits = 0;
    
    switch (source) {
        case RequestSource::HEATING:
            clearBits = SystemEvents::BurnerRequest::HEATING;
            LOG_INFO(TAG, "Clearing heating request");
            break;
            
        case RequestSource::WATER:
            clearBits = SystemEvents::BurnerRequest::WATER;
            LOG_INFO(TAG, "Clearing water request");
            break;
            
        case RequestSource::MANUAL:
        case RequestSource::EMERGENCY:
            clearBits = SystemEvents::BurnerRequest::ALL_BITS;
            LOG_INFO(TAG, "Clearing all requests (%s)", 
                    source == RequestSource::EMERGENCY ? "EMERGENCY" : "MANUAL");
            break;
    }
    
    return atomicUpdateBits(0, clearBits);
}

bool BurnerRequestManager::clearAllRequests() {
    LOG_INFO(TAG, "Clearing all burner requests");
    return atomicUpdateBits(0, SystemEvents::BurnerRequest::ALL_BITS);
}

EventBits_t BurnerRequestManager::getCurrentRequests() {
    EventGroupHandle_t eventGroup = getBurnerRequestEventGroup();
    if (!eventGroup) {
        return 0;
    }
    return xEventGroupGetBits(eventGroup);
}

Temperature_t BurnerRequestManager::getCurrentTargetTemp() {
    EventBits_t bits = getCurrentRequests();
    return SystemEvents::BurnerRequest::decode_temperature_t(bits);
}

bool BurnerRequestManager::isHeatingRequested() {
    return (getCurrentRequests() & SystemEvents::BurnerRequest::HEATING) != 0;
}

bool BurnerRequestManager::isWaterRequested() {
    return (getCurrentRequests() & SystemEvents::BurnerRequest::WATER) != 0;
}

bool BurnerRequestManager::isWaterPriority() {
    // Read from SystemState (single source of truth for priority setting)
    EventBits_t systemBits = xEventGroupGetBits(SRP::getSystemStateEventGroup());
    return (systemBits & SystemEvents::SystemState::WATER_PRIORITY) != 0;
}

bool BurnerRequestManager::isHighPowerRequested() {
    return (getCurrentRequests() & SystemEvents::BurnerRequest::POWER_HIGH) != 0;
}

bool BurnerRequestManager::updateTargetTemp(Temperature_t newTemp) {
    if (!initialized || !requestMutex) {
        LOG_ERROR(TAG, "Not initialized");
        return false;
    }

    // Validate temperature (20°C to 90°C)
    Temperature_t minTemp = tempFromWhole(20);
    Temperature_t maxTemp = tempFromWhole(90);
    if (newTemp < minTemp) newTemp = minTemp;
    if (newTemp > maxTemp) newTemp = maxTemp;

    auto guard = MutexRetryHelper::acquireGuard(
        requestMutex,
        "BurnerRequest-UpdateTemp",
        pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)
    );
    if (!guard) {
        LOG_ERROR(TAG, "Failed to acquire mutex");
        return false;
    }

    EventGroupHandle_t eventGroup = getBurnerRequestEventGroup();
    if (!eventGroup) {
        return false;
    }

    // Read current bits, clear temperature, set new temperature
    EventBits_t currentBits = xEventGroupGetBits(eventGroup);
    EventBits_t newBits = (currentBits & ~SystemEvents::BurnerRequest::TEMPERATURE_MASK) |
                         SystemEvents::BurnerRequest::encode_temperature(newTemp);

    xEventGroupClearBits(eventGroup, SystemEvents::BurnerRequest::ALL_BITS);
    xEventGroupSetBits(eventGroup, newBits);

    char tempStr[16];
    formatTemp(tempStr, sizeof(tempStr), newTemp);
    LOG_DEBUG(TAG, "Updated target temp to %s°C", tempStr);
    return true;
}

void BurnerRequestManager::emergencyClearAll() {
    LOG_ERROR(TAG, "EMERGENCY: Clearing all burner requests");

    // C1: Use mutex to prevent race with normal updates
    // Short timeout - emergency should not block long
    if (initialized && requestMutex) {
        auto guard = MutexRetryHelper::acquireGuard(
            requestMutex,
            "BurnerRequest-Emergency",
            pdMS_TO_TICKS(50)  // Short timeout for emergency
        );
        // Proceed even if mutex acquisition fails - emergency clear is critical
        if (!guard) {
            LOG_WARN(TAG, "Emergency clear proceeding without mutex");
        }
    }

    EventGroupHandle_t eventGroup = getBurnerRequestEventGroup();
    if (eventGroup) {
        xEventGroupClearBits(eventGroup, SystemEvents::BurnerRequest::ALL_BITS);
    }
}

uint32_t BurnerRequestManager::getLastHeatingRequestTime() {
    return lastHeatingRequestTime;
}

uint32_t BurnerRequestManager::getLastWaterRequestTime() {
    return lastWaterRequestTime;
}

bool BurnerRequestManager::isHeatingRequestExpired(uint32_t maxAgeMs) {
    if (!isHeatingRequested()) {
        return false;  // No request, so not expired
    }
    if (lastHeatingRequestTime == 0) {
        return true;  // Request exists but no timestamp - treat as expired
    }
    uint32_t age = millis() - lastHeatingRequestTime;
    return age > maxAgeMs;
}

bool BurnerRequestManager::isWaterRequestExpired(uint32_t maxAgeMs) {
    if (!isWaterRequested()) {
        return false;  // No request, so not expired
    }
    if (lastWaterRequestTime == 0) {
        return true;  // Request exists but no timestamp - treat as expired
    }
    uint32_t age = millis() - lastWaterRequestTime;
    return age > maxAgeMs;
}

bool BurnerRequestManager::checkAndClearExpiredRequests(uint32_t maxAgeMs) {
    bool anyCleared = false;

    if (isHeatingRequestExpired(maxAgeMs)) {
        uint32_t age = millis() - lastHeatingRequestTime;
        LOG_ERROR(TAG, "WATCHDOG: Heating request expired (age: %lu ms, max: %lu ms) - clearing",
                  age, maxAgeMs);
        clearRequest(RequestSource::EMERGENCY);
        anyCleared = true;
    }

    if (isWaterRequestExpired(maxAgeMs)) {
        uint32_t age = millis() - lastWaterRequestTime;
        LOG_ERROR(TAG, "WATCHDOG: Water request expired (age: %lu ms, max: %lu ms) - clearing",
                  age, maxAgeMs);
        clearRequest(RequestSource::EMERGENCY);
        anyCleared = true;
    }

    return anyCleared;
}