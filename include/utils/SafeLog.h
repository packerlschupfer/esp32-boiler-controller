// include/utils/SafeLog.h
#ifndef SAFE_LOG_H
#define SAFE_LOG_H

#include <esp_log.h>
#include <cstdio>

/**
 * @brief Safe logging utilities for float values
 *
 * Prevents stack overflow when logging multiple float values by using
 * a stack-allocated buffer for formatting before passing to ESP_LOG.
 *
 * Usage:
 *   SafeLog::logFloatPair(TAG, "Temp: %.1f, Pressure: %.2f", temp, pressure);
 *   SafeLog::logFloatTriple(TAG, "P=%.2f I=%.2f D=%.2f", p, i, d);
 */
class SafeLog {
public:
    /**
     * @brief Log with one float value (INFO level)
     * @param tag Log tag
     * @param format Format string with one %f
     * @param value Float value
     */
    static void logFloat(const char* tag, const char* format, float value) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), format, value);
        ESP_LOGI(tag, "%s", buffer);
    }

    /**
     * @brief Log with two float values (INFO level)
     * @param tag Log tag
     * @param format Format string with two %f
     * @param value1 First float value
     * @param value2 Second float value
     */
    static void logFloatPair(const char* tag, const char* format, float value1, float value2) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), format, value1, value2);
        ESP_LOGI(tag, "%s", buffer);
    }

    /**
     * @brief Log with three float values (INFO level)
     * @param tag Log tag
     * @param format Format string with three %f
     * @param value1 First float value
     * @param value2 Second float value
     * @param value3 Third float value
     */
    static void logFloatTriple(const char* tag, const char* format, float value1, float value2, float value3) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), format, value1, value2, value3);
        ESP_LOGI(tag, "%s", buffer);
    }

    /**
     * @brief Log with four float values (INFO level)
     * @param tag Log tag
     * @param format Format string with four %f
     */
    static void logFloatQuad(const char* tag, const char* format, float v1, float v2, float v3, float v4) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), format, v1, v2, v3, v4);
        ESP_LOGI(tag, "%s", buffer);
    }

    // DEBUG level variants
    static void logFloatDebug(const char* tag, const char* format, float value) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), format, value);
        ESP_LOGD(tag, "%s", buffer);
    }

    static void logFloatPairDebug(const char* tag, const char* format, float value1, float value2) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), format, value1, value2);
        ESP_LOGD(tag, "%s", buffer);
    }

    static void logFloatTripleDebug(const char* tag, const char* format, float value1, float value2, float value3) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), format, value1, value2, value3);
        ESP_LOGD(tag, "%s", buffer);
    }

    // WARN level variants
    static void logFloatWarn(const char* tag, const char* format, float value) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), format, value);
        ESP_LOGW(tag, "%s", buffer);
    }

    static void logFloatPairWarn(const char* tag, const char* format, float value1, float value2) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), format, value1, value2);
        ESP_LOGW(tag, "%s", buffer);
    }
};

#endif // SAFE_LOG_H
