// include/LoggingMacros.h
#ifndef LOGGING_MACROS_H
#define LOGGING_MACROS_H

// Add a compile-time check to verify this header is included
#define LOGGING_MACROS_INCLUDED 1

// Use the new LogInterface for zero-overhead logging
#include <LogInterface.h>  // Use angle brackets for library include

// Check if we're using no-logger build
#ifdef LOG_NO_CUSTOM_LOGGER
    // Force disable USE_CUSTOM_LOGGER if LOG_NO_CUSTOM_LOGGER is set
    #ifdef USE_CUSTOM_LOGGER
        #undef USE_CUSTOM_LOGGER
    #endif
#endif

// All logging macros are now provided by LogInterface.h
// This file just adds build-mode specific filtering

// Conditional logging based on build mode
#ifdef LOG_MODE_RELEASE
    // In release mode, debug and verbose are no-ops
    #undef LOG_DEBUG
    #undef LOG_VERBOSE
    #define LOG_DEBUG(tag, fmt, ...) ((void)0)
    #define LOG_VERBOSE(tag, fmt, ...) ((void)0)
#endif

#ifdef LOG_MODE_DEBUG_SELECTIVE
    // In selective debug, verbose is no-op
    #undef LOG_VERBOSE
    #define LOG_VERBOSE(tag, fmt, ...) ((void)0)
#endif

// Helper macros for common patterns
#define LOG_FUNC_ENTER(tag) LOG_DEBUG(tag, "%s: enter", __func__)
#define LOG_FUNC_EXIT(tag) LOG_DEBUG(tag, "%s: exit", __func__)
#define LOG_FUNC_ERROR(tag, msg) LOG_ERROR(tag, "%s: %s", __func__, msg)

#endif // LOGGING_MACROS_H