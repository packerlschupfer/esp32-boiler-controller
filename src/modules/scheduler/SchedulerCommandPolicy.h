// src/modules/scheduler/SchedulerCommandPolicy.h
#ifndef SCHEDULER_COMMAND_POLICY_H
#define SCHEDULER_COMMAND_POLICY_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "config/SystemConstants.h"

/**
 * @brief Scheduler MQTT command rules (header-only, native-testable).
 *
 * No ArduinoJson/Arduino/FreeRTOS: TimerSchedulerTask extracts the JSON fields and
 * passes plain values; replies are formatted into caller buffers.
 */
namespace SchedulerCommandPolicy {

    // Space heating schedule modes (HeatingMode in SpaceHeatingScheduleAction.cpp)
    constexpr uint8_t SPACE_MODE_COMFORT = 0;
    constexpr uint8_t SPACE_MODE_ECO = 1;
    constexpr uint8_t SPACE_MODE_FROST = 2;

    inline bool spaceModeValid(int mode) {
        return mode >= SPACE_MODE_COMFORT && mode <= SPACE_MODE_FROST;
    }

    /**
     * @brief Target (whole °C) of a space heating schedule added without target_temp.
     *
     * The add handler used 21 for every mode, so an ECO or FROST schedule without
     * target_temp heated to the comfort temperature (2026-09-15).
     */
    inline int spaceDefaultTargetC(uint8_t mode) {
        using namespace SystemConstants::Temperature::SpaceHeating;
        switch (mode) {
            case SPACE_MODE_ECO:   return DEFAULT_ECO_TEMP / 10;
            case SPACE_MODE_FROST: return DEFAULT_FROST_TEMP / 10;
            default:               return DEFAULT_COMFORT_TEMP / 10;
        }
    }

    // Fields of a boiler/cmd/scheduler/enable payload {"id":N,"enabled":true|false}
    struct EnablePayload {
        bool hasId;
        bool idIsInt;
        long id;
        bool hasEnabled;
        bool enabledIsBool;
    };

    /**
     * @return nullptr if valid, otherwise the error code for the reply
     */
    inline const char* validateEnable(const EnablePayload& p) {
        if (!p.hasId) return "missing_id";
        if (!p.idIsInt) return "invalid_id_type";
        if (p.id < 0 || p.id > 255) return "id_out_of_range";
        if (!p.hasEnabled) return "missing_enabled";
        if (!p.enabledIsBool) return "invalid_enabled_type";
        return nullptr;
    }

    struct EnableDecision {
        bool changed;       // enabled flag changes: save to FRAM
        bool endActiveRun;  // running schedule is disabled: end it now (releases its request)
    };

    inline EnableDecision decideEnable(bool currentlyEnabled, bool requestedEnabled, bool active) {
        EnableDecision d;
        d.changed = currentlyEnabled != requestedEnabled;
        d.endActiveRun = !requestedEnabled && active;
        return d;
    }

    inline bool equalsToken(const char* value, const char* token) {
        return value != nullptr && strcmp(value, token) == 0;
    }

    /**
     * @brief boiler/cmd/scheduler/disable is an alias of enable with "enabled":false
     *
     * The state comes from the topic, so {"id":N} is a complete payload. A payload asking for
     * the opposite state is a mistake and is rejected instead of disabling anyway.
     * @return nullptr if the payload is acceptable, otherwise the error code for the reply
     */
    inline const char* validateDisableAlias(bool hasEnabled, bool enabledIsBool, bool enabled) {
        if (hasEnabled && (!enabledIsBool || enabled)) return "enabled_conflicts_with_topic";
        return nullptr;
    }

    // boiler/cmd/scheduler/clear erases every schedule, so it takes an explicit payload like
    // the FRAM format command ("format_confirm") instead of acting on an empty message
    constexpr char CLEAR_CONFIRM_PAYLOAD[] = "confirm";

    inline bool clearConfirmed(const char* payload) {
        return equalsToken(payload, CLEAR_CONFIRM_PAYLOAD);
    }

    // Reply messages for scheduler sub-topics that no branch handles. MQTTSubscriptionManager
    // subscribes boiler/cmd/scheduler/+, so every sub-topic reaches processMQTTCommand(); an
    // unhandled one used to return without a reply, which looks like a hung command to the
    // caller (2026-09-16).
    constexpr char REPLY_NOT_IMPLEMENTED[] = "not_implemented";
    constexpr char REPLY_UNKNOWN_COMMAND[] = "unknown_command";

    inline bool isHandledCommand(const char* command) {
        return equalsToken(command, "add") || equalsToken(command, "remove") ||
               equalsToken(command, "enable") || equalsToken(command, "disable") ||
               equalsToken(command, "clear") || equalsToken(command, "list") ||
               equalsToken(command, "status");
    }

    /**
     * @brief Reply message for a scheduler command without a handler
     *
     * "not_implemented" for the sub-topics MQTTTopics.h defined without an implementation
     * (update, vacation, pump_exercise, command), "unknown_command" for anything else.
     * @return nullptr if the command has a handler
     */
    inline const char* unhandledReply(const char* command) {
        if (isHandledCommand(command)) return nullptr;
        if (equalsToken(command, "update") || equalsToken(command, "vacation") ||
            equalsToken(command, "pump_exercise") || equalsToken(command, "command")) {
            return REPLY_NOT_IMPLEMENTED;
        }
        return REPLY_UNKNOWN_COMMAND;
    }

    // Status reply {"active":..,"count":..,"activeIds":[..],"disabledIds":[..]}
    constexpr size_t STATUS_MIN_BUFFER = 80;
    constexpr size_t MAX_STATUS_IDS = 20;  // schedules::MAX_SCHEDULES

    namespace detail {
        constexpr char STATUS_MID[] = "],\"disabledIds\":[";
        constexpr char STATUS_END[] = "]}";

        // Appends "a,b,c" while `reserve` bytes stay free behind it; returns the new length
        inline size_t appendIds(char* out, size_t size, size_t len,
                                const uint8_t* ids, size_t n, size_t reserve) {
            for (size_t i = 0; i < n; i++) {
                char item[12];
                int w = snprintf(item, sizeof(item), "%s%u", (i == 0) ? "" : ",",
                                 static_cast<unsigned>(ids[i]));
                if (w < 0 || len + static_cast<size_t>(w) + reserve > size) break;
                memcpy(out + len, item, static_cast<size_t>(w));
                len += static_cast<size_t>(w);
            }
            out[len] = '\0';
            return len;
        }
    } // namespace detail

    /**
     * @brief Format the scheduler status into out
     *
     * IDs that do not fit are left out; the reply stays valid JSON.
     * @return out, or nullptr if out is smaller than STATUS_MIN_BUFFER
     */
    inline const char* formatStatus(char* out, size_t size, bool anyActive, unsigned count,
                                    const uint8_t* activeIds, size_t activeCount,
                                    const uint8_t* disabledIds, size_t disabledCount) {
        const size_t midLen = sizeof(detail::STATUS_MID) - 1;
        const size_t endLen = sizeof(detail::STATUS_END) - 1;
        if (out == nullptr || size < STATUS_MIN_BUFFER) return nullptr;

        int written = snprintf(out, size, "{\"active\":%s,\"count\":%u,\"activeIds\":[",
                               anyActive ? "true" : "false", count);
        if (written < 0 || static_cast<size_t>(written) + midLen + endLen + 1 > size) return nullptr;
        size_t len = static_cast<size_t>(written);

        len = detail::appendIds(out, size, len, activeIds, activeCount, midLen + endLen + 1);
        memcpy(out + len, detail::STATUS_MID, midLen);
        len += midLen;
        len = detail::appendIds(out, size, len, disabledIds, disabledCount, endLen + 1);
        memcpy(out + len, detail::STATUS_END, endLen + 1);  // with terminator
        return out;
    }

} // namespace SchedulerCommandPolicy

#endif // SCHEDULER_COMMAND_POLICY_H
