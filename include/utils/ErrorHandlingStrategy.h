#pragma once

#include "utils/ErrorHandler.h"
#include "events/SystemEventsGenerated.h"
#include "core/SystemResourceProvider.h"

namespace ErrorHandling {

template<typename T>
Result<T> executeWithErrorHandling(
    std::function<Result<T>()> operation,
    const char* context,
    const char* tag
) {
    try {
        auto result = operation();
        if (result.isError()) {
            ErrorHandler::logError(tag, result.errorCode(), 
                String(context) + ": " + result.errorMessage());
        }
        return result;
    } catch (const std::exception& e) {
        SystemError error = SystemError::UNKNOWN_ERROR;
        String message = String(context) + " - Exception: " + e.what();
        ErrorHandler::logError(tag, error, message.c_str());
        return Result<T>::error(error, message);
    } catch (...) {
        SystemError error = SystemError::UNKNOWN_ERROR;
        String message = String(context) + " - Unknown exception";
        ErrorHandler::logError(tag, error, message.c_str());
        return Result<T>::error(error, message);
    }
}

// Recovery functionality removed - ErrorRecovery system not available

inline void signalError(EventBits_t errorBit, EventBits_t resolvedBit) {
    // Error event group not available in SRP
    // This functionality would need to be implemented elsewhere
}

inline void signalErrorResolved(EventBits_t errorBit, EventBits_t resolvedBit) {
    // Error event group not available in SRP
    // This functionality would need to be implemented elsewhere
}

inline Result<void> wrapLegacyBool(bool success, const char* errorMsg, SystemError errorCode) {
    if (success) {
        return Result<void>();  // Default constructor is success
    }
    return Result<void>(errorCode, errorMsg);
}

class TaskErrorHandler {
public:
    static void handleTaskError(
        const char* taskName,
        SystemError error,
        const char* message,
        EventBits_t errorBit = 0,
        EventBits_t resolvedBit = 0
    ) {
        ErrorHandler::logError(taskName, error, message);
        
        if (errorBit != 0) {
            signalError(errorBit, resolvedBit);
        }
        
        // Check if error is critical (>= 700)
        if (static_cast<int>(error) >= 700) {
            ErrorHandler::handleCriticalError(error);
        }
    }
    
    static void handleTaskRecovery(
        const char* taskName,
        const char* message,
        EventBits_t errorBit = 0,
        EventBits_t resolvedBit = 0
    ) {
        LOG_INFO(taskName, "Recovered: %s", message);
        
        if (resolvedBit != 0) {
            signalErrorResolved(errorBit, resolvedBit);
        }
    }
};

} // namespace ErrorHandling