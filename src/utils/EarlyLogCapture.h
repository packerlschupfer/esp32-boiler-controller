// src/utils/EarlyLogCapture.h
#ifndef EARLY_LOG_CAPTURE_H
#define EARLY_LOG_CAPTURE_H

#include <vector>
#include <string>
#include <cstdarg>
#include <cstdio>

/**
 * @brief Captures early boot logs before Logger is initialized or serial monitor connects
 * 
 * This class stores early boot messages in a circular buffer and can replay them
 * once the Logger is fully initialized and serial monitor is connected.
 */
class EarlyLogCapture {
private:
    static constexpr size_t MAX_EARLY_LOGS = 100;
    static constexpr size_t MAX_LOG_LENGTH = 256;
    
    static std::vector<std::string> earlyLogs;
    static bool capturing;
    static unsigned long startTime;
    
public:
    /**
     * @brief Start capturing early logs
     */
    static void begin() {
        capturing = true;
        startTime = millis();
        earlyLogs.reserve(MAX_EARLY_LOGS);
    }
    
    /**
     * @brief Capture a log message with timestamp
     * @param format Printf-style format string
     * @param ... Variable arguments
     */
    static void capture(const char* format, ...) {
        if (!capturing || earlyLogs.size() >= MAX_EARLY_LOGS) {
            return;
        }
        
        char buffer[MAX_LOG_LENGTH];
        unsigned long timestamp = millis() - startTime;
        
        // Format the message
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        
        // Create timestamped message
        char timestampedMsg[MAX_LOG_LENGTH + 32];
        snprintf(timestampedMsg, sizeof(timestampedMsg), "[%lu ms] %s", timestamp, buffer);
        
        earlyLogs.push_back(std::string(timestampedMsg));
        
        // Also print to Serial immediately if available
        if (Serial) {
            Serial.println(timestampedMsg);
            Serial.flush();
        }
    }
    
    /**
     * @brief Stop capturing and dump all captured logs
     * @param logFunc Function to use for logging (e.g., Logger function)
     */
    template<typename LogFunc>
    static void dumpAndStop(LogFunc logFunc) {
        capturing = false;
        
        if (!earlyLogs.empty()) {
            logFunc("EarlyBoot", "=== EARLY BOOT LOG REPLAY (%zu messages) ===", earlyLogs.size());
            
            for (const auto& log : earlyLogs) {
                logFunc("EarlyBoot", "%s", log.c_str());
            }
            
            logFunc("EarlyBoot", "=== END EARLY BOOT LOG REPLAY ===");
            logFunc("EarlyBoot", "Total early boot time: %lu ms", millis() - startTime);
        }
        
        earlyLogs.clear();
        earlyLogs.shrink_to_fit();
    }
    
    /**
     * @brief Check if still capturing
     */
    static bool isCapturing() {
        return capturing;
    }
    
    /**
     * @brief Get number of captured logs
     */
    static size_t getLogCount() {
        return earlyLogs.size();
    }
};

// Static member definitions
inline std::vector<std::string> EarlyLogCapture::earlyLogs;
inline bool EarlyLogCapture::capturing = false;
inline unsigned long EarlyLogCapture::startTime = 0;

// Convenience macro for early logging
#define EARLY_LOG(format, ...) EarlyLogCapture::capture(format, ##__VA_ARGS__)

#endif // EARLY_LOG_CAPTURE_H