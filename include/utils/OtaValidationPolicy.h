// include/utils/OtaValidationPolicy.h
#ifndef OTA_VALIDATION_POLICY_H
#define OTA_VALIDATION_POLICY_H

#include <cstdint>

/**
 * @brief When an OTA-updated image is confirmed or rolled back (header-only, native-testable)
 *
 * Used by OtaRollbackGuard while the running image is ESP_OTA_IMG_PENDING_VERIFY. A reset
 * before the image is confirmed boots the previous image (bootloader rollback), so a firmware
 * that crashes early is replaced automatically; one that runs but never manages to read its
 * sensors is rolled back after MAX_WAIT_MS.
 *
 * WHY NETWORK CONNECTIVITY IS NOT A HEALTH SIGNAL (changed 2026-10-25)
 *
 * Confirmation used to require networkConnected as well, so an Ethernet link that was down
 * between uptime 60 s and 600 s rolled a PERFECTLY HEALTHY image back and rebooted the
 * controller - unattended, with no post-purge and no pump overrun if it happened mid-burn.
 * This controller is required to work with no internet and no Home Assistant, so "no network"
 * cannot be evidence of a bad image; it is the configuration the boiler has to survive. And
 * the rollback achieved nothing even on its own terms: the previous image, booting into the
 * same dead LAN, is no more reachable than this one.
 *
 * Reading the sensors is the health signal that means something. It proves the RS485 bus, the
 * Modbus coordinator and the MB8ART driver all work - i.e. that the boiler can be controlled,
 * which is this firmware's whole job and the only part of it that fails dangerously.
 *
 * KNOWN COST, accepted deliberately: an image that boots and reads sensors but breaks
 * NETWORKING is now kept, and recovering from it needs the USB/usbip path rather than another
 * OTA. That is the right way round - a controller that reboots itself whenever the LAN blinks
 * is a worse failure than one that needs a cable to update.
 */
namespace OtaValidationPolicy {

    enum class Action : uint8_t {
        WAIT,
        MARK_VALID,
        ROLLBACK
    };

    constexpr uint32_t MIN_UPTIME_MS = 60000;   // confirm only after one minute without a reset
    constexpr uint32_t MAX_WAIT_MS = 600000;    // roll back if still unhealthy after 10 minutes

    /**
     * @param networkConnected kept in the signature, and deliberately unused in the decision:
     *        the caller logs it, and a parameter that disappears would hide that this used to
     *        be a rollback trigger. See the note above.
     */
    inline Action decide(uint32_t uptimeMs, bool sensorsRead, bool networkConnected) {
        (void)networkConnected;
        if (uptimeMs >= MIN_UPTIME_MS && sensorsRead) {
            return Action::MARK_VALID;
        }
        if (uptimeMs >= MAX_WAIT_MS) {
            // Still no sensor data after 10 minutes: the image boots but cannot control the
            // boiler, which is the one failure worth rebooting for.
            return Action::ROLLBACK;
        }
        return Action::WAIT;
    }

}  // namespace OtaValidationPolicy

#endif  // OTA_VALIDATION_POLICY_H
