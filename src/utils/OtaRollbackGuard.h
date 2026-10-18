// src/utils/OtaRollbackGuard.h
#ifndef OTA_ROLLBACK_GUARD_H
#define OTA_ROLLBACK_GUARD_H

/**
 * @brief Confirms an OTA-updated firmware image or rolls it back
 *
 * The Arduino core marked every new image valid before setup(), so the bootloader rollback
 * never applied (2026-09-15). This file overrides the core hook verifyRollbackLater(); check()
 * confirms the image once OtaValidationPolicy allows it, or rolls back to the previous image.
 * Serial-flashed images have no pending state and are not affected.
 */
namespace OtaRollbackGuard {

    /**
     * @brief Evaluate a pending OTA image (MonitoringTask loop; cheap, rate-limited to 5 s)
     */
    void check();

}  // namespace OtaRollbackGuard

#endif  // OTA_ROLLBACK_GUARD_H
