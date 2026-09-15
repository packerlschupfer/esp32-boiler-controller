// include/utils/OtaValidationPolicy.h
#ifndef OTA_VALIDATION_POLICY_H
#define OTA_VALIDATION_POLICY_H

#include <cstdint>

/**
 * @brief When an OTA-updated image is confirmed or rolled back (header-only, native-testable)
 *
 * Used by OtaRollbackGuard while the running image is ESP_OTA_IMG_PENDING_VERIFY. A reset
 * before the image is confirmed boots the previous image (bootloader rollback), so a firmware
 * that crashes early is replaced automatically; one that runs but never gets sensor data or a
 * network connection is rolled back after MAX_WAIT_MS.
 */
namespace OtaValidationPolicy {

    enum class Action : uint8_t {
        WAIT,
        MARK_VALID,
        ROLLBACK
    };

    constexpr uint32_t MIN_UPTIME_MS = 60000;   // confirm only after one minute without a reset
    constexpr uint32_t MAX_WAIT_MS = 600000;    // roll back if still unhealthy after 10 minutes

    inline Action decide(uint32_t uptimeMs, bool sensorsRead, bool networkConnected) {
        if (uptimeMs >= MIN_UPTIME_MS && sensorsRead && networkConnected) {
            return Action::MARK_VALID;
        }
        if (uptimeMs >= MAX_WAIT_MS) {
            return Action::ROLLBACK;
        }
        return Action::WAIT;
    }

}  // namespace OtaValidationPolicy

#endif  // OTA_VALIDATION_POLICY_H
