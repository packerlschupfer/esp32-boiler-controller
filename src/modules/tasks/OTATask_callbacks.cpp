// src/modules/tasks/OTATask_callbacks.cpp
// OTA callbacks implementation

#include "OTATask.h"
#include <SemaphoreGuard.h>
#include <OTAManager.h>
#include <EthernetManager.h>
#include "utils/ErrorHandler.h"
#include "utils/CriticalDataStorage.h"
#include "LoggingMacros.h"
#include "config/ProjectConfig.h"
#include "modules/control/BurnerStateMachine.h"    // F38: graceful burner stop before flash
#include "modules/control/BurnerRequestManager.h"  // F38
#include <MQTTManager.h>                           // OTA DRAM: stop MQTT for the transfer
#include "core/SystemResourceProvider.h"
#include <esp_heap_caps.h>

static const char* TAG = "OTA";

// OTA callback implementations
void OTATask::onOTAStart() {
    LOG_INFO(TAG, "OTA update starting");

    {
        SemaphoreGuard guard(otaStatusMutex, pdMS_TO_TICKS(100));
        if (guard.hasLock()) {
            otaUpdateInProgress = true;
        } else {
            LOG_ERROR(TAG, "Failed to acquire OTA status mutex on start");
        }
    }

    // F38: initiate a graceful burner shutdown BEFORE the flash completes and
    // reboots. Without this, an OTA pushed while the burner is RUNNING reboots
    // the ESP32 on completion, cutting the burner-enable relay via the 10s
    // hardware watchdog with NO post-purge and NO pump overrun - the exact
    // "ESP32 died" emergency path (thermal stress on the exchanger), on every
    // field update during a burn. Clear demand now (non-blocking, so we do not
    // stall the OTA transfer) and let the FSM run POST_PURGE during the
    // tens-of-seconds transfer window before the reboot.
    BurnerSMState burnerState = BurnerStateMachine::getCurrentState();
    if (burnerState != BurnerSMState::IDLE) {
        LOG_WARN(TAG, "OTA started while burner active (state=%d) - requesting graceful shutdown",
                 static_cast<int>(burnerState));
        BurnerRequestManager::emergencyClearAll();
        BurnerStateMachine::setHeatDemand(false, 0, false);
    }

    // Round 21: Save critical state before OTA update
    LOG_INFO(TAG, "Saving critical state before OTA...");

    // Save runtime counters to FRAM
    if (CriticalDataStorage::saveRuntimeCounters()) {
        LOG_INFO(TAG, "Runtime counters saved");
    } else {
        LOG_WARN(TAG, "Failed to save runtime counters");
    }

    // Log OTA event to safety log
    CriticalDataStorage::logSafetyEvent(
        0x01,  // Event type: System update
        0x01,  // Action: OTA started
        0       // No additional data
    );

    // Free DRAM for the transfer (2026-09-16). OTA here fails for lack of
    // byte-addressable DRAM, not bandwidth: Update.begin() takes a 4096 B sector
    // buffer and NetworkClient::fillBuffer() then needs 1436 B contiguous, while
    // measured free DRAM is 5-7 kB with a largest block of only 3.5-5.4 kB. Under
    // MQTT load the largest block fell to 3572 B - below what begin() itself needs,
    // which is why three OTA attempts failed three different ways. Tearing the MQTT
    // client down releases the esp-mqtt task stack (6144 B) and its in/out buffers
    // (~2 kB), far more than the shortfall, and the timing is right: begin() has
    // already succeeded and the first socket read has not happened yet.
    //
    // Only when actually connected: MQTTManager::disconnect() clears loopStarted
    // inside its isConnected() guard, so calling it while disconnected would leave
    // the flag set and connect() would never re-init the client - MQTT dead until
    // reboot. Auto-reconnect must go first, or onDisconnect() restarts the reconnect
    // timer and the memory comes straight back within about a second.
    //
    // Synchronous on purpose: this runs in OTATask, where stop/destroy is legal (the
    // IDF forbids it only from the MQTT event handler). Deferring to MQTTTask would
    // race the first read. onOTAError restores MQTT; on success the device reboots.
    {
        MQTTManager* mqtt = SRP::getMQTTManager();
        if (mqtt && mqtt->isConnected()) {
            LOG_WARN(TAG, "Stopping MQTT for OTA - DRAM before: %u free, largest %u",
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)));
            mqtt->setAutoReconnect(false);
            mqtt->disconnect();
            // Serial-only from here until the reboot or onOTAError - MQTT is down
            LOG_WARN(TAG, "MQTT stopped - DRAM after: %u free, largest %u",
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)));
        } else {
            LOG_INFO(TAG, "MQTT not connected - no teardown needed for OTA");
        }
    }

    // Indicate OTA in progress with LED
    // StatusLed::setPattern(1, 200, 200);  // Fast blink
}

void OTATask::onOTAEnd() {
    LOG_INFO(TAG, "OTA update ended");

    {
        SemaphoreGuard guard(otaStatusMutex, pdMS_TO_TICKS(100));
        if (guard.hasLock()) {
            otaUpdateInProgress = false;
        } else {
            LOG_ERROR(TAG, "Failed to acquire OTA status mutex on end");
        }
    }

    // Resume any suspended operations
    // SensorTask::resume();

    // Indicate OTA complete with LED
    // StatusLed::setPattern(2, 500, 1000);  // Double blink

    LOG_INFO(TAG, "OTA update successful - system will restart");
}

void OTATask::onOTAProgress(unsigned int progress, unsigned int total) {
    static unsigned int lastPercent = 0;
    unsigned int percent = (progress * 100) / total;

    // Only log at 10% intervals to reduce spam
    if (percent != lastPercent && percent % 10 == 0) {
        LOG_INFO(TAG, "OTA progress: %u%%", percent);
        lastPercent = percent;
    }

    // Could update LED brightness or pattern based on progress
    // StatusLed::setBrightness(percent);
}

void OTATask::onOTAError(ota_error_t error) {
    const char* errorStr = "Unknown error";
    switch (error) {
        case OTA_AUTH_ERROR: errorStr = "Authentication failed"; break;
        case OTA_BEGIN_ERROR: errorStr = "Begin failed"; break;
        case OTA_CONNECT_ERROR: errorStr = "Connect failed"; break;
        case OTA_RECEIVE_ERROR: errorStr = "Receive failed"; break;
        case OTA_END_ERROR: errorStr = "End failed"; break;
        default: break;
    }

    LOG_ERROR(TAG, "OTA error: %s (code: %d)", errorStr, error);

    {
        SemaphoreGuard guard(otaStatusMutex, pdMS_TO_TICKS(100));
        if (guard.hasLock()) {
            otaUpdateInProgress = false;
        } else {
            LOG_ERROR(TAG, "Failed to acquire OTA status mutex on error");
        }
    }

    // Resume any suspended operations
    // SensorTask::resume();

    // Restore MQTT: onOTAStart tore the client down to free DRAM for the transfer.
    // Only reached when the OTA failed - on success the device reboots instead.
    {
        MQTTManager* mqtt = SRP::getMQTTManager();
        if (mqtt && !mqtt->isConnected()) {
            LOG_WARN(TAG, "Restoring MQTT after failed OTA");
            mqtt->setAutoReconnect(true);
            const auto result = mqtt->connect();
            if (!result.isOk()) {
                LOG_ERROR(TAG, "MQTT reconnect failed - auto-reconnect will keep retrying");
            }
        }
    }

    // Indicate error with LED pattern - 5 quick blinks
    // StatusLed::setPattern(5, 100, 1500);
}

// Network check callback
bool OTATask::isNetworkConnected() {
    return EthernetManager::isConnected();
}
