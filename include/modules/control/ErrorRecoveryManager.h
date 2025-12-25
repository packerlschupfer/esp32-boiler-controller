#ifndef ERROR_RECOVERY_MANAGER_H
#define ERROR_RECOVERY_MANAGER_H

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>
#include "utils/ErrorHandler.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/**
 * @brief Manages error recovery strategies and automatic system recovery
 * 
 * This class provides a framework for automatic error recovery with
 * progressive strategies, backoff mechanisms, and escalation paths.
 */
class ErrorRecoveryManager {
public:
    // Recovery strategies
    enum class RecoveryStrategy {
        NONE,
        RETRY,              // Simple retry
        RETRY_WITH_BACKOFF, // Exponential backoff
        RESET_COMPONENT,    // Reset specific component
        RESTART_TASK,       // Restart failed task
        DEGRADE_SERVICE,    // Run in degraded mode
        FAILOVER,           // Switch to backup
        EMERGENCY_STOP,     // Stop all operations
        SYSTEM_RESET        // Full system reset
    };

    // Recovery result
    enum class RecoveryResult {
        SUCCESS,
        FAILED,
        IN_PROGRESS,
        ESCALATED,
        ABANDONED
    };

    // Error context for recovery
    struct ErrorContext {
        SystemError error;
        const char* component;
        uint32_t timestamp;
        uint8_t occurrenceCount;
        uint8_t recoveryAttempts;
        void* customData;
    };

    // Recovery action callback
    using RecoveryAction = std::function<RecoveryResult(const ErrorContext&)>;

    // Recovery policy
    struct RecoveryPolicy {
        RecoveryStrategy strategy;
        uint8_t maxAttempts;
        uint32_t initialDelayMs;
        uint32_t maxDelayMs;
        float backoffMultiplier;
        RecoveryAction customAction;
        RecoveryStrategy escalationStrategy;
    };

    /**
     * @brief Get singleton instance
     */
    static ErrorRecoveryManager& getInstance();

    /**
     * @brief Register recovery policy for specific error
     * @param error Error type
     * @param policy Recovery policy
     */
    void registerRecoveryPolicy(SystemError error, const RecoveryPolicy& policy);

    /**
     * @brief Handle error with automatic recovery
     * @param error Error that occurred
     * @param component Component that failed
     * @param customData Optional context data
     * @return Recovery result
     */
    RecoveryResult handleError(
        SystemError error,
        const char* component,
        void* customData = nullptr
    );

    /**
     * @brief Register task for automatic restart
     * @param taskName Task name
     * @param taskFunction Task function
     * @param stackSize Stack size
     * @param priority Task priority
     */
    void registerTask(
        const char* taskName,
        TaskFunction_t taskFunction,
        uint32_t stackSize,
        UBaseType_t priority
    );

    /**
     * @brief Check if component is in recovery
     * @param component Component name
     * @return true if recovery in progress
     */
    bool isRecovering(const char* component);

    /**
     * @brief Get recovery statistics
     */
    struct RecoveryStats {
        uint32_t totalErrors;
        uint32_t successfulRecoveries;
        uint32_t failedRecoveries;
        uint32_t escalations;
        std::unordered_map<SystemError, uint32_t> errorCounts;
    };
    RecoveryStats getStats() const { return stats; }

    /**
     * @brief Reset error count for component
     * @param component Component name
     */
    void clearErrorHistory(const char* component);

    /**
     * @brief Set global recovery enabled/disabled
     */
    void setRecoveryEnabled(bool enabled) { recoveryEnabled = enabled; }

private:
    ErrorRecoveryManager();
    ~ErrorRecoveryManager() = default;
    ErrorRecoveryManager(const ErrorRecoveryManager&) = delete;
    ErrorRecoveryManager& operator=(const ErrorRecoveryManager&) = delete;

    // Recovery policies by error type
    std::unordered_map<SystemError, RecoveryPolicy> policies;
    
    // Error history by component
    std::unordered_map<std::string, std::vector<ErrorContext>> errorHistory;
    
    // Task registry for restart capability
    struct TaskInfo {
        TaskFunction_t function;
        uint32_t stackSize;
        UBaseType_t priority;
        TaskHandle_t handle;
        uint8_t restartCount;
        uint32_t lastRestartTime;
    };
    std::unordered_map<std::string, TaskInfo> taskRegistry;
    
    // Recovery state
    std::unordered_map<std::string, bool> activeRecoveries;
    
    // Statistics
    RecoveryStats stats;
    
    // Configuration
    bool recoveryEnabled;
    static constexpr uint32_t ERROR_HISTORY_WINDOW_MS = 300000;  // 5 minutes
    static constexpr uint8_t MAX_ERRORS_PER_WINDOW = 10;
    static constexpr uint32_t MIN_RESTART_INTERVAL_MS = 5000;
    
    // Mutex for thread safety
    SemaphoreHandle_t mutex;
    
    // Recovery task handle
    TaskHandle_t recoveryTaskHandle;
    
    /**
     * @brief Execute recovery strategy
     */
    RecoveryResult executeRecovery(
        const ErrorContext& context,
        const RecoveryPolicy& policy
    );
    
    /**
     * @brief Retry with backoff
     */
    RecoveryResult retryWithBackoff(
        const ErrorContext& context,
        const RecoveryPolicy& policy
    );
    
    /**
     * @brief Restart task
     */
    RecoveryResult restartTask(const char* taskName);
    
    /**
     * @brief Update error history
     */
    void updateErrorHistory(const ErrorContext& context);
    
    /**
     * @brief Check if escalation needed
     */
    bool shouldEscalate(const ErrorContext& context);
    
    /**
     * @brief Recovery monitoring task
     */
    static void recoveryTask(void* pvParameters);
    
    /**
     * @brief Clean old error entries
     */
    void cleanErrorHistory();
    
    /**
     * @brief Default recovery actions
     */
    static RecoveryResult defaultSensorRecovery(const ErrorContext& context);
    static RecoveryResult defaultNetworkRecovery(const ErrorContext& context);
    static RecoveryResult defaultModbusRecovery(const ErrorContext& context);
    static RecoveryResult defaultRelayRecovery(const ErrorContext& context);
};

// Convenience macros for error handling with recovery
#define HANDLE_ERROR_WITH_RECOVERY(error, component) \
    ErrorRecoveryManager::getInstance().handleError(error, component)

#define REGISTER_RECOVERY_POLICY(error, strategy, attempts) \
    ErrorRecoveryManager::getInstance().registerRecoveryPolicy( \
        error, \
        {strategy, attempts, 1000, 30000, 2.0f, nullptr, RecoveryStrategy::ESCALATE} \
    )

#endif // ERROR_RECOVERY_MANAGER_H