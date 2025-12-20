// include/utils/MQTTValidator.h
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "shared/Temperature.h"

/**
 * @brief MQTT command input validation utilities
 * 
 * Provides validation for MQTT payloads to prevent crashes and
 * ensure data integrity before processing.
 */
class MQTTValidator {
public:
    // Validation result
    struct ValidationResult {
        bool valid;
        const char* error;
        
        ValidationResult(bool v = true, const char* e = nullptr) 
            : valid(v), error(e) {}
            
        operator bool() const { return valid; }
    };
    
    // Common limits
    static constexpr size_t MAX_STRING_LENGTH = 32;
    static constexpr size_t MAX_PAYLOAD_SIZE = 1024;
    static constexpr int MIN_TEMPERATURE_C = -50;
    static constexpr int MAX_TEMPERATURE_C = 150;
    static constexpr int MIN_HOUR = 0;
    static constexpr int MAX_HOUR = 23;
    static constexpr int MIN_MINUTE = 0;
    static constexpr int MAX_MINUTE = 59;
    
    /**
     * @brief Validate a schedule add command
     */
    static ValidationResult validateScheduleAdd(const JsonDocument& doc) {
        // Required fields - use isNull() instead of containsKey() for ArduinoJson v7
        if (doc["name"].isNull()) {
            return ValidationResult(false, "missing_name");
        }

        // Validate name
        const char* name = doc["name"];
        if (!name || strlen(name) == 0) {
            return ValidationResult(false, "empty_name");
        }
        if (strlen(name) > MAX_STRING_LENGTH) {
            return ValidationResult(false, "name_too_long");
        }

        // Validate type if present
        if (!doc["type"].isNull()) {
            const char* type = doc["type"];
            if (!type) {
                return ValidationResult(false, "invalid_type");
            }
            if (strcmp(type, "water_heating") != 0 &&
                strcmp(type, "space_heating") != 0) {
                return ValidationResult(false, "unknown_type");
            }
        }

        // Validate time fields
        if (!doc["start_hour"].isNull()) {
            int hour = doc["start_hour"];
            if (hour < MIN_HOUR || hour > MAX_HOUR) {
                return ValidationResult(false, "invalid_start_hour");
            }
        }

        if (!doc["start_minute"].isNull()) {
            int minute = doc["start_minute"];
            if (minute < MIN_MINUTE || minute > MAX_MINUTE) {
                return ValidationResult(false, "invalid_start_minute");
            }
        }

        if (!doc["end_hour"].isNull()) {
            int hour = doc["end_hour"];
            if (hour < MIN_HOUR || hour > MAX_HOUR) {
                return ValidationResult(false, "invalid_end_hour");
            }
        }

        if (!doc["end_minute"].isNull()) {
            int minute = doc["end_minute"];
            if (minute < MIN_MINUTE || minute > MAX_MINUTE) {
                return ValidationResult(false, "invalid_end_minute");
            }
        }

        // Validate temperature if present
        if (!doc["target_temp"].isNull()) {
            int temp = doc["target_temp"];
            if (temp < MIN_TEMPERATURE_C || temp > MAX_TEMPERATURE_C) {
                return ValidationResult(false, "invalid_temperature");
            }
        }

        // Validate days
        if (!doc["days"].isNull()) {
            if (doc["days"].is<JsonArray>()) {
                JsonArrayConst days = doc["days"].as<JsonArrayConst>();
                if (days.size() == 0) {
                    return ValidationResult(false, "empty_days_array");
                }
                if (days.size() > 7) {
                    return ValidationResult(false, "too_many_days");
                }
                for (JsonVariantConst day : days) {
                    if (!day.is<int>()) {
                        return ValidationResult(false, "invalid_day_type");
                    }
                    int dayNum = day.as<int>();
                    if (dayNum < 0 || dayNum > 6) {
                        return ValidationResult(false, "invalid_day_number");
                    }
                }
            } else if (doc["days"].is<int>()) {
                int dayMask = doc["days"];
                if (dayMask < 0 || dayMask > 0x7F) {
                    return ValidationResult(false, "invalid_day_mask");
                }
            } else {
                return ValidationResult(false, "invalid_days_format");
            }
        }
        
        return ValidationResult(true);
    }
    
    /**
     * @brief Validate a schedule remove command
     */
    static ValidationResult validateScheduleRemove(const JsonDocument& doc) {
        // Use isNull() instead of containsKey() for ArduinoJson v7
        if (doc["id"].isNull()) {
            return ValidationResult(false, "missing_id");
        }
        
        if (!doc["id"].is<int>()) {
            return ValidationResult(false, "invalid_id_type");
        }
        
        int id = doc["id"];
        if (id < 0 || id > 255) {
            return ValidationResult(false, "id_out_of_range");
        }
        
        return ValidationResult(true);
    }
    
    /**
     * @brief Validate a control command payload
     */
    static ValidationResult validateControlCommand(const char* command, const char* payload) {
        if (!command || !payload) {
            return ValidationResult(false, "null_input");
        }
        
        // Check payload size
        if (strlen(payload) > MAX_PAYLOAD_SIZE) {
            return ValidationResult(false, "payload_too_large");
        }
        
        // Command-specific validation
        if (strcmp(command, "mode") == 0) {
            // Validate mode values
            if (strcmp(payload, "off") != 0 && 
                strcmp(payload, "manual") != 0 && 
                strcmp(payload, "auto") != 0) {
                return ValidationResult(false, "invalid_mode");
            }
        } else if (strcmp(command, "target_temp") == 0) {
            // Parse temperature
            char* endptr;
            long temp = strtol(payload, &endptr, 10);
            if (endptr == payload || *endptr != '\0') {
                return ValidationResult(false, "invalid_number");
            }
            if (temp < MIN_TEMPERATURE_C || temp > MAX_TEMPERATURE_C) {
                return ValidationResult(false, "temperature_out_of_range");
            }
        }
        
        return ValidationResult(true);
    }
    
    /**
     * @brief Sanitize a string for safe use
     * Removes control characters and limits length
     */
    static void sanitizeString(char* str, size_t maxLen) {
        if (!str) return;
        
        size_t len = 0;
        char* read = str;
        char* write = str;
        
        while (*read && len < maxLen - 1) {
            if (isprint(*read) || *read == ' ') {
                *write++ = *read;
                len++;
            }
            read++;
        }
        *write = '\0';
    }
    
    /**
     * @brief Check if a JSON document is within size limits
     */
    static bool isJsonSizeValid(const JsonDocument& doc, size_t maxSize = 1024) {
        return measureJson(doc) <= maxSize;
    }
};