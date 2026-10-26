// src/init/LoggingInitializer.h
#ifndef LOGGING_INITIALIZER_H
#define LOGGING_INITIALIZER_H

#include "utils/ErrorHandler.h"

/**
 * @brief Handles logging system initialization
 *
 * Configures ESP-IDF log levels and custom Logger based on build mode:
 * - LOG_MODE_DEBUG_FULL: All modules at DEBUG level
 * - LOG_MODE_DEBUG_SELECTIVE: Selected modules at DEBUG, others at INFO
 * - Release mode: Minimal logging at INFO/ERROR level
 */
class LoggingInitializer {
public:
    /**
     * @brief Initialize the logging system
     * @return Result indicating success or failure
     */
    static Result<void> initialize();

private:
    // Prevent instantiation
    LoggingInitializer() = delete;

    /**
     * @brief Configure selective debug logging for specific modules
     */
    static void configureSelectiveDebug();

    /**
     * @brief Suppress verbose HAL and system logs
     */
    static void suppressVerboseLogs();
};

#endif // LOGGING_INITIALIZER_H
