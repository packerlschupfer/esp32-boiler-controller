// src/modules/scheduler/SchedulerResponseFormatter.h
#pragma once

#include <ArduinoJson.h>
#include "TimerSchedule.h"
#include <cstdio>
#include <map>
#include <vector>

/**
 * @brief Response formatter for scheduler MQTT messages
 *
 * Formats into a caller-provided buffer (no heap, see docs/MEMORY_OPTIMIZATION.md) and
 * returns it. The former version formatted into a StringPool ScopedBuffer and returned
 * buffer.c_str(): the ScopedBuffer released and cleared the pool buffer when the function
 * returned, so every list/add/remove/status reply was published empty (2026-09-15).
 */
class SchedulerResponseFormatter {
public:
    /**
     * @brief Format a simple status response
     */
    static const char* formatStatusResponse(char* out, size_t size, bool success, uint8_t id = 0) {
        if (out == nullptr || size == 0) return PreformattedResponses::ERROR_BUFFER;
        snprintf(out, size, "{\"status\":\"%s\",\"id\":%d}", success ? "ok" : "error", id);
        return out;
    }

    /**
     * @brief Format an error response
     */
    static const char* formatErrorResponse(char* out, size_t size, const char* error, uint8_t id = 0) {
        if (out == nullptr || size == 0) return PreformattedResponses::ERROR_BUFFER;
        snprintf(out, size, "{\"status\":\"error\",\"msg\":\"%s\",\"id\":%d}",
                 error ? error : "unknown", id);
        return out;
    }

    /**
     * @brief Format the schedule list
     *
     * Lists as many schedules as fit into the buffer (the MQTT payload holds 320 bytes, about
     * two to three schedules); "count" is the number listed, "total" the number stored.
     */
    static const char* formatScheduleList(char* out, size_t size, const std::vector<TimerSchedule>& schedules) {
        if (out == nullptr || size == 0) return PreformattedResponses::ERROR_BUFFER;

        JsonDocument doc;  // ArduinoJson v7
        JsonArray array = doc["schedules"].to<JsonArray>();
        doc["count"] = 0;
        doc["total"] = schedules.size();

        size_t count = 0;
        for (const auto& schedule : schedules) {
            JsonObject obj = array.add<JsonObject>();
            obj["id"] = schedule.id;
            obj["name"] = schedule.name.c_str();
            obj["enabled"] = schedule.enabled;
            obj["type"] = (schedule.type == ScheduleType::WATER_HEATING) ? "water" : "space";
            obj["days"] = schedule.dayMask;
            obj["start"] = (schedule.startHour << 8) | schedule.startMinute;
            obj["end"] = (schedule.endHour << 8) | schedule.endMinute;
            doc["count"] = count + 1;

            if (measureJson(doc) >= size) {
                // Does not fit: drop this entry and stop
                array.remove(count);
                doc["count"] = count;
                break;
            }
            count++;
        }

        serializeJson(doc, out, size);
        return out;
    }

    /**
     * @brief Format schedule status
     */
    static const char* formatScheduleStatus(
        char* out, size_t size,
        const std::vector<TimerSchedule>& schedules,
        const std::map<uint8_t, bool>& activeSchedules,
        bool anyActive) {

        if (out == nullptr || size < 48) return PreformattedResponses::ERROR_BUFFER;

        int written = snprintf(out, size, "{\"active\":%s,\"count\":%u,\"activeIds\":[",
                               anyActive ? "true" : "false", static_cast<unsigned>(schedules.size()));
        if (written < 0 || static_cast<size_t>(written) >= size) {
            return PreformattedResponses::ERROR_BUFFER;
        }
        size_t len = static_cast<size_t>(written);

        bool first = true;
        for (const auto& entry : activeSchedules) {
            if (!entry.second) continue;
            // Keep room for "]}" and the terminator
            int w = snprintf(out + len, size - len, "%s%u", first ? "" : ",", static_cast<unsigned>(entry.first));
            if (w < 0 || static_cast<size_t>(w) + 3 > size - len) {
                out[len] = '\0';
                break;
            }
            len += static_cast<size_t>(w);
            first = false;
        }

        snprintf(out + len, size - len, "]}");
        return out;
    }

    /**
     * @brief Pre-format common responses for reuse
     */
    struct PreformattedResponses {
        static constexpr const char* OK = "{\"status\":\"ok\"}";
        static constexpr const char* ERROR_PARSE = "{\"status\":\"error\",\"msg\":\"parse_error\"}";
        static constexpr const char* ERROR_NOT_FOUND = "{\"status\":\"error\",\"msg\":\"not_found\"}";
        static constexpr const char* ERROR_FULL = "{\"status\":\"error\",\"msg\":\"schedules_full\"}";
        static constexpr const char* ERROR_INVALID_TYPE = "{\"status\":\"error\",\"msg\":\"invalid_type\"}";
        static constexpr const char* ERROR_BUFFER = "{\"status\":\"error\",\"msg\":\"no_buffer\"}";
    };
};
