// src/modules/scheduler/SchedulerResponseFormatter.h
#pragma once

#include <ArduinoJson.h>
#include "utils/StringPool.h"
#include "TimerSchedule.h"
#include <vector>
#include <map>

/**
 * @brief Optimized response formatter for scheduler MQTT messages
 * 
 * Uses pre-allocated buffers and avoids dynamic string allocations
 */
class SchedulerResponseFormatter {
public:
    /**
     * @brief Format a simple status response
     */
    static const char* formatStatusResponse(bool success, uint8_t id = 0) {
        auto buffer = StringPool::getSmallBuffer();
        if (!buffer) return "{\"status\":\"error\",\"msg\":\"no_buffer\"}";
        
        snprintf(buffer.data(), buffer.size(), 
                "{\"status\":\"%s\",\"id\":%d}", 
                success ? "ok" : "error", id);
        
        return buffer.c_str();
    }
    
    /**
     * @brief Format an error response
     */
    static const char* formatErrorResponse(const char* error, uint8_t id = 0) {
        auto buffer = StringPool::getMediumBuffer();
        if (!buffer) return "{\"status\":\"error\",\"msg\":\"no_buffer\"}";
        
        snprintf(buffer.data(), buffer.size(), 
                "{\"status\":\"error\",\"msg\":\"%s\",\"id\":%d}", 
                error, id);
        
        return buffer.c_str();
    }
    
    /**
     * @brief Format schedule list response using JsonDocument
     */
    static const char* formatScheduleList(const std::vector<TimerSchedule>& schedules) {
        auto buffer = StringPool::getLargeBuffer();
        if (!buffer) return "{\"status\":\"error\",\"msg\":\"no_buffer\"}";

        // Use JSON document (ArduinoJson v7)
        JsonDocument doc;
        JsonArray array = doc["schedules"].to<JsonArray>();

        // Limit schedules to prevent overflow
        size_t count = 0;
        for (const auto& schedule : schedules) {
            if (count >= 5) break; // Max 5 schedules in list

            JsonObject obj = array.add<JsonObject>();
            obj["id"] = schedule.id;
            obj["name"] = schedule.name.c_str();
            obj["enabled"] = schedule.enabled;
            obj["type"] = (schedule.type == ScheduleType::WATER_HEATING) ? "water" : "space";
            obj["days"] = schedule.dayMask;
            obj["start"] = (schedule.startHour << 8) | schedule.startMinute;
            obj["end"] = (schedule.endHour << 8) | schedule.endMinute;

            count++;
        }

        doc["count"] = count;
        doc["total"] = schedules.size();

        serializeJson(doc, buffer.data(), buffer.size());
        return buffer.c_str();
    }
    
    /**
     * @brief Format schedule status
     */
    static const char* formatScheduleStatus(
        const std::vector<TimerSchedule>& schedules,
        const std::map<uint8_t, bool>& activeSchedules,
        bool anyActive) {
        
        auto buffer = StringPool::getLargeBuffer();
        if (!buffer) return "{\"status\":\"error\",\"msg\":\"no_buffer\"}";
        
        // Compact format
        snprintf(buffer.data(), buffer.size(),
                "{\"active\":%s,\"count\":%zu,\"activeIds\":[",
                anyActive ? "true" : "false",
                schedules.size());
        
        size_t len = strlen(buffer.data());
        char* ptr = buffer.data() + len;
        size_t remaining = buffer.size() - len;
        
        // Add active IDs
        bool first = true;
        for (const auto& [id, active] : activeSchedules) {
            if (active && remaining > 10) {
                int written = snprintf(ptr, remaining, "%s%d", 
                                     first ? "" : ",", id);
                if (written > 0) {
                    ptr += written;
                    remaining -= written;
                    first = false;
                }
            }
        }
        
        // Close JSON
        if (remaining > 2) {
            snprintf(ptr, remaining, "%s", "]}");
        }
        
        return buffer.c_str();
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
    };
};