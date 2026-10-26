// src/utils/OtaRollbackGuard.cpp
#include "utils/OtaRollbackGuard.h"
#include "utils/OtaValidationPolicy.h"
#include "core/SystemResourceProvider.h"
#include "events/SystemEventsGenerated.h"
#include "LoggingMacros.h"
#include <Arduino.h>
#include <EthernetManager.h>
#include <esp_ota_ops.h>

static const char* TAG = "OtaRollback";

// Arduino core hook (weak in esp32-hal-misc.c): returning true keeps the core from marking a
// freshly updated image valid before setup(). OtaRollbackGuard::check() confirms it later; a
// reset before that makes the bootloader boot the previous image.
extern "C" bool verifyRollbackLater() {
    return true;
}

namespace OtaRollbackGuard {

    void check() {
        static constexpr uint32_t CHECK_INTERVAL_MS = 5000;
        static bool resolved = false;
        static bool checkedOnce = false;
        static uint32_t lastCheckMs = 0;

        if (resolved) {
            return;
        }
        const uint32_t now = millis();
        if (checkedOnce && (now - lastCheckMs) < CHECK_INTERVAL_MS) {
            return;
        }
        checkedOnce = true;
        lastCheckMs = now;

        const esp_partition_t* running = esp_ota_get_running_partition();
        esp_ota_img_states_t state;
        if (running == nullptr || esp_ota_get_state_partition(running, &state) != ESP_OK) {
            LOG_INFO(TAG, "No OTA image state (serial flash) - nothing to confirm");
            resolved = true;
            return;
        }
        if (state != ESP_OTA_IMG_PENDING_VERIFY) {
            LOG_INFO(TAG, "OTA image state %d - nothing to confirm", static_cast<int>(state));
            resolved = true;
            return;
        }

        EventGroupHandle_t sensorGroup = SRP::getSensorEventGroup();
        const bool sensorsRead = sensorGroup != nullptr &&
            (xEventGroupGetBits(sensorGroup) & SystemEvents::SensorUpdate::FIRST_READ_COMPLETE) != 0;
        const bool networkConnected = EthernetManager::isConnected();

        switch (OtaValidationPolicy::decide(now, sensorsRead, networkConnected)) {
            case OtaValidationPolicy::Action::WAIT:
                break;

            case OtaValidationPolicy::Action::MARK_VALID:
                if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
                    LOG_WARN(TAG, "Updated firmware confirmed after %lu s - rollback cancelled",
                             static_cast<unsigned long>(now / 1000));
                } else {
                    LOG_ERROR(TAG, "Failed to confirm the updated firmware");
                }
                resolved = true;
                break;

            case OtaValidationPolicy::Action::ROLLBACK:
                LOG_ERROR(TAG, "Updated firmware not healthy after %lu s (sensors %s, network %s)",
                          static_cast<unsigned long>(now / 1000),
                          sensorsRead ? "ok" : "missing", networkConnected ? "ok" : "down");
                if (esp_ota_check_rollback_is_possible()) {
                    LOG_ERROR(TAG, "Rolling back to the previous firmware and rebooting");
                    vTaskDelay(pdMS_TO_TICKS(500));  // let the log line out
                    esp_ota_mark_app_invalid_rollback_and_reboot();
                }
                // No valid previous image: keep this one instead of rebooting repeatedly
                LOG_ERROR(TAG, "No previous firmware to roll back to - keeping this one");
                (void)esp_ota_mark_app_valid_cancel_rollback();
                resolved = true;
                break;
        }
    }

}  // namespace OtaRollbackGuard
