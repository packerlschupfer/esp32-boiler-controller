/**
 * @file test_error_recovery_manager.cpp
 * @brief Unit tests for ErrorRecoveryManager recovery logic
 *
 * Tests the recovery strategies, backoff calculations, error history,
 * and escalation logic using a simplified mock implementation.
 */

#include <unity.h>
#include <cstdint>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <vector>
#include <string>
#include <algorithm>
#include "mocks/MockTime.h"

// ============================================================================
// Mock SystemError enum (matches src/utils/ErrorHandler.h)
// ============================================================================
enum class TestSystemError : uint32_t {
    SUCCESS = 0,
    SENSOR_FAILURE = 504,
    RELAY_FAULT = 603,
    NETWORK_ERROR = 203,
    MODBUS_TIMEOUT = 400,
    EMERGENCY_STOP = 707
};

// Hash function for TestSystemError enum
namespace std {
    template<>
    struct hash<TestSystemError> {
        size_t operator()(const TestSystemError& e) const {
            return hash<uint32_t>()(static_cast<uint32_t>(e));
        }
    };
}

// ============================================================================
// Mock FreeRTOS primitives
// ============================================================================
static bool g_degradedModeSet = false;
static bool g_emergencyStopSet = false;
static bool g_sensorErrorCleared = false;
static bool g_networkErrorCleared = false;

void mock_resetEventGroupFlags() {
    g_degradedModeSet = false;
    g_emergencyStopSet = false;
    g_sensorErrorCleared = false;
    g_networkErrorCleared = false;
}

// ============================================================================
// Simplified ErrorRecoveryManager for testing
// ============================================================================
class TestErrorRecoveryManager {
public:
    enum class RecoveryStrategy {
        NONE,
        RETRY,
        RETRY_WITH_BACKOFF,
        RESET_COMPONENT,
        RESTART_TASK,
        DEGRADE_SERVICE,
        FAILOVER,
        EMERGENCY_STOP,
        SYSTEM_RESET
    };

    enum class RecoveryResult {
        SUCCESS,
        FAILED,
        IN_PROGRESS,
        ESCALATED,
        ABANDONED
    };

    struct ErrorContext {
        TestSystemError error;
        const char* component;
        uint32_t timestamp;
        uint8_t occurrenceCount;
        uint8_t recoveryAttempts;
        void* customData;
    };

    using RecoveryAction = std::function<RecoveryResult(const ErrorContext&)>;

    struct RecoveryPolicy {
        RecoveryStrategy strategy;
        uint8_t maxAttempts;
        uint32_t initialDelayMs;
        uint32_t maxDelayMs;
        float backoffMultiplier;
        RecoveryAction customAction;
        RecoveryStrategy escalationStrategy;
    };

    struct RecoveryStats {
        uint32_t totalErrors;
        uint32_t successfulRecoveries;
        uint32_t failedRecoveries;
        uint32_t escalations;
        std::unordered_map<TestSystemError, uint32_t> errorCounts;
    };

private:
    std::unordered_map<TestSystemError, RecoveryPolicy> policies;
    std::unordered_map<std::string, std::vector<ErrorContext>> errorHistory;
    std::unordered_map<std::string, bool> activeRecoveries;
    RecoveryStats stats;
    bool recoveryEnabled;

    static constexpr uint32_t ERROR_HISTORY_WINDOW_MS = 300000;  // 5 minutes
    static constexpr uint8_t MAX_ERRORS_PER_WINDOW = 10;

public:
    TestErrorRecoveryManager() : recoveryEnabled(true) {
        stats = {0, 0, 0, 0, {}};
        registerDefaultPolicies();
    }

    void registerDefaultPolicies() {
        // Sensor failures
        policies[TestSystemError::SENSOR_FAILURE] = {
            RecoveryStrategy::RETRY_WITH_BACKOFF,
            3,      // maxAttempts
            1000,   // initialDelayMs
            10000,  // maxDelayMs
            2.0f,   // backoffMultiplier
            nullptr,
            RecoveryStrategy::DEGRADE_SERVICE
        };

        // Relay faults
        policies[TestSystemError::RELAY_FAULT] = {
            RecoveryStrategy::RESET_COMPONENT,
            2,      // maxAttempts
            100,    // initialDelayMs
            1000,   // maxDelayMs
            1.0f,   // backoffMultiplier
            nullptr,
            RecoveryStrategy::EMERGENCY_STOP
        };

        // Network errors
        policies[TestSystemError::NETWORK_ERROR] = {
            RecoveryStrategy::RETRY_WITH_BACKOFF,
            5,      // maxAttempts
            2000,   // initialDelayMs
            30000,  // maxDelayMs
            1.5f,   // backoffMultiplier
            nullptr,
            RecoveryStrategy::RESET_COMPONENT
        };

        // Modbus timeout
        policies[TestSystemError::MODBUS_TIMEOUT] = {
            RecoveryStrategy::RETRY_WITH_BACKOFF,
            3,      // maxAttempts
            500,    // initialDelayMs
            5000,   // maxDelayMs
            2.0f,   // backoffMultiplier
            nullptr,
            RecoveryStrategy::RESTART_TASK
        };
    }

    void registerRecoveryPolicy(TestSystemError error, const RecoveryPolicy& policy) {
        policies[error] = policy;
    }

    void setRecoveryEnabled(bool enabled) {
        recoveryEnabled = enabled;
    }

    bool isRecovering(const char* component) {
        std::string compStr(component);
        auto it = activeRecoveries.find(compStr);
        return (it != activeRecoveries.end()) ? it->second : false;
    }

    void clearErrorHistory(const char* component) {
        std::string compStr(component);
        errorHistory[compStr].clear();
    }

    RecoveryStats getStats() const {
        return stats;
    }

    RecoveryResult handleError(
        TestSystemError error,
        const char* component,
        void* customData = nullptr) {

        if (!recoveryEnabled) {
            return RecoveryResult::ABANDONED;
        }

        ErrorContext context = {
            error,
            component,
            millis(),
            0,
            0,
            customData
        };

        std::string compKey(component);

        // Check if already recovering
        if (activeRecoveries[compKey]) {
            return RecoveryResult::IN_PROGRESS;
        }

        // Mark as in recovery
        activeRecoveries[compKey] = true;

        // Get policy
        auto it = policies.find(error);
        if (it == policies.end()) {
            activeRecoveries[compKey] = false;
            return RecoveryResult::FAILED;
        }

        RecoveryPolicy policy = it->second;

        // Update error history
        updateErrorHistory(context);

        // Count errors in window
        auto& history = errorHistory[compKey];
        context.occurrenceCount = std::count_if(history.begin(), history.end(),
            [error, &context](const ErrorContext& e) {
                return e.error == error &&
                       (context.timestamp - e.timestamp) < ERROR_HISTORY_WINDOW_MS;
            });

        // Check for escalation
        if (shouldEscalate(context)) {
            policy.strategy = policy.escalationStrategy;
        }

        // Execute recovery
        RecoveryResult result = executeRecovery(context, policy);

        // Update stats
        stats.totalErrors++;
        if (result == RecoveryResult::SUCCESS) {
            stats.successfulRecoveries++;
        } else if (result == RecoveryResult::FAILED) {
            stats.failedRecoveries++;
        } else if (result == RecoveryResult::ESCALATED) {
            stats.escalations++;
        }
        stats.errorCounts[error]++;

        // Clear active recovery flag
        activeRecoveries[compKey] = false;

        return result;
    }

    // Public for testing
    uint32_t calculateBackoffDelay(uint32_t baseDelay, uint8_t attempt, float multiplier, uint32_t maxDelay) {
        uint32_t delay = baseDelay;
        for (uint8_t i = 0; i < attempt; i++) {
            delay = static_cast<uint32_t>(delay * multiplier);
            if (delay > maxDelay) {
                delay = maxDelay;
                break;
            }
        }
        return delay;
    }

    uint8_t getErrorCountForComponent(const char* component, TestSystemError error) {
        std::string compStr(component);
        auto it = errorHistory.find(compStr);
        if (it == errorHistory.end()) {
            return 0;
        }
        auto& history = it->second;
        uint32_t now = millis();
        return std::count_if(history.begin(), history.end(),
            [error, now](const ErrorContext& e) {
                return e.error == error &&
                       (now - e.timestamp) < ERROR_HISTORY_WINDOW_MS;
            });
    }

private:
    void updateErrorHistory(const ErrorContext& context) {
        std::string compStr(context.component);
        auto& history = errorHistory[compStr];
        history.push_back(context);

        // Clean old entries - prevent underflow
        if (context.timestamp > ERROR_HISTORY_WINDOW_MS) {
            uint32_t cutoffTime = context.timestamp - ERROR_HISTORY_WINDOW_MS;
            history.erase(
                std::remove_if(history.begin(), history.end(),
                    [cutoffTime](const ErrorContext& e) {
                        return e.timestamp < cutoffTime;
                    }),
                history.end()
            );
        }
    }

    bool shouldEscalate(const ErrorContext& context) {
        return context.occurrenceCount >= MAX_ERRORS_PER_WINDOW;
    }

    RecoveryResult executeRecovery(const ErrorContext& context, const RecoveryPolicy& policy) {
        switch (policy.strategy) {
            case RecoveryStrategy::RETRY:
            case RecoveryStrategy::RETRY_WITH_BACKOFF:
                return retryWithBackoff(context, policy);

            case RecoveryStrategy::RESET_COMPONENT:
                if (policy.customAction) {
                    return policy.customAction(context);
                }
                return RecoveryResult::FAILED;

            case RecoveryStrategy::DEGRADE_SERVICE:
                g_degradedModeSet = true;
                return RecoveryResult::SUCCESS;

            case RecoveryStrategy::EMERGENCY_STOP:
                g_emergencyStopSet = true;
                return RecoveryResult::ESCALATED;

            case RecoveryStrategy::SYSTEM_RESET:
                return RecoveryResult::ESCALATED;

            default:
                return RecoveryResult::FAILED;
        }
    }

    RecoveryResult retryWithBackoff(const ErrorContext& context, const RecoveryPolicy& policy) {
        for (uint8_t attempt = 0; attempt < policy.maxAttempts; attempt++) {
            // Calculate delay (no actual delay in tests)
            uint32_t delay = calculateBackoffDelay(
                policy.initialDelayMs,
                attempt,
                policy.backoffMultiplier,
                policy.maxDelayMs
            );

            // Advance mock time
            advanceMockMillis(delay);

            // Execute custom action if provided
            if (policy.customAction) {
                RecoveryResult result = policy.customAction(context);
                if (result == RecoveryResult::SUCCESS) {
                    return RecoveryResult::SUCCESS;
                }
            } else {
                // No custom action - simulate success on last attempt for testing
                if (attempt == policy.maxAttempts - 1) {
                    return RecoveryResult::FAILED;
                }
            }
        }

        return RecoveryResult::FAILED;
    }
};

// ============================================================================
// Test instances
// ============================================================================
static TestErrorRecoveryManager* erm = nullptr;

// ============================================================================
// Test setup/teardown helpers
// ============================================================================
static void erm_setup() {
    setMockMillis(0);
    mock_resetEventGroupFlags();
    if (erm) {
        delete erm;
    }
    erm = new TestErrorRecoveryManager();
}

static void erm_teardown() {
    if (erm) {
        delete erm;
        erm = nullptr;
    }
}

// ============================================================================
// Test functions
// ============================================================================

void test_erm_initial_state() {
    erm_setup();

    auto stats = erm->getStats();
    TEST_ASSERT_EQUAL_UINT32(0, stats.totalErrors);
    TEST_ASSERT_EQUAL_UINT32(0, stats.successfulRecoveries);
    TEST_ASSERT_EQUAL_UINT32(0, stats.failedRecoveries);
    TEST_ASSERT_EQUAL_UINT32(0, stats.escalations);

    erm_teardown();
}

void test_erm_recovery_disabled() {
    erm_setup();

    erm->setRecoveryEnabled(false);
    auto result = erm->handleError(TestSystemError::SENSOR_FAILURE, "TestSensor");

    TEST_ASSERT_EQUAL(TestErrorRecoveryManager::RecoveryResult::ABANDONED, result);

    // Stats should not be updated when disabled
    auto stats = erm->getStats();
    TEST_ASSERT_EQUAL_UINT32(0, stats.totalErrors);

    erm_teardown();
}

void test_erm_unknown_error_fails() {
    erm_setup();

    // Emergency stop has no policy registered by default in our test
    auto result = erm->handleError(TestSystemError::EMERGENCY_STOP, "TestComponent");

    TEST_ASSERT_EQUAL(TestErrorRecoveryManager::RecoveryResult::FAILED, result);

    erm_teardown();
}

void test_erm_in_progress_detection() {
    erm_setup();

    // Register a policy that takes time (uses custom action that always succeeds)
    TestErrorRecoveryManager::RecoveryPolicy policy = {
        TestErrorRecoveryManager::RecoveryStrategy::RETRY,
        1,
        100,
        1000,
        1.0f,
        [](const TestErrorRecoveryManager::ErrorContext&) {
            return TestErrorRecoveryManager::RecoveryResult::SUCCESS;
        },
        TestErrorRecoveryManager::RecoveryStrategy::DEGRADE_SERVICE
    };
    erm->registerRecoveryPolicy(TestSystemError::SENSOR_FAILURE, policy);

    // Component should not be recovering initially
    TEST_ASSERT_FALSE(erm->isRecovering("TestSensor"));

    // After successful recovery, should no longer be recovering
    erm->handleError(TestSystemError::SENSOR_FAILURE, "TestSensor");
    TEST_ASSERT_FALSE(erm->isRecovering("TestSensor"));

    erm_teardown();
}

void test_erm_backoff_calculation() {
    erm_setup();

    // Test exponential backoff
    uint32_t delay0 = erm->calculateBackoffDelay(1000, 0, 2.0f, 30000);
    uint32_t delay1 = erm->calculateBackoffDelay(1000, 1, 2.0f, 30000);
    uint32_t delay2 = erm->calculateBackoffDelay(1000, 2, 2.0f, 30000);
    uint32_t delay3 = erm->calculateBackoffDelay(1000, 3, 2.0f, 30000);

    TEST_ASSERT_EQUAL_UINT32(1000, delay0);  // No backoff on first attempt
    TEST_ASSERT_EQUAL_UINT32(2000, delay1);  // 1000 * 2
    TEST_ASSERT_EQUAL_UINT32(4000, delay2);  // 2000 * 2
    TEST_ASSERT_EQUAL_UINT32(8000, delay3);  // 4000 * 2

    erm_teardown();
}

void test_erm_backoff_max_delay_cap() {
    erm_setup();

    // Test that backoff is capped at max delay
    uint32_t delay = erm->calculateBackoffDelay(1000, 10, 2.0f, 5000);

    TEST_ASSERT_EQUAL_UINT32(5000, delay);  // Should be capped at maxDelay

    erm_teardown();
}

void test_erm_error_history_tracking() {
    erm_setup();

    // Register policy with custom action that always succeeds
    TestErrorRecoveryManager::RecoveryPolicy policy = {
        TestErrorRecoveryManager::RecoveryStrategy::RETRY,
        1,
        100,
        1000,
        1.0f,
        [](const TestErrorRecoveryManager::ErrorContext&) {
            return TestErrorRecoveryManager::RecoveryResult::SUCCESS;
        },
        TestErrorRecoveryManager::RecoveryStrategy::DEGRADE_SERVICE
    };
    erm->registerRecoveryPolicy(TestSystemError::SENSOR_FAILURE, policy);

    // Generate multiple errors
    erm->handleError(TestSystemError::SENSOR_FAILURE, "SensorA");
    advanceMockMillis(1000);
    erm->handleError(TestSystemError::SENSOR_FAILURE, "SensorA");
    advanceMockMillis(1000);
    erm->handleError(TestSystemError::SENSOR_FAILURE, "SensorA");

    uint8_t count = erm->getErrorCountForComponent("SensorA", TestSystemError::SENSOR_FAILURE);
    TEST_ASSERT_EQUAL_UINT8(3, count);

    erm_teardown();
}

void test_erm_error_history_expiration() {
    erm_setup();

    // Register policy with custom action that always succeeds
    TestErrorRecoveryManager::RecoveryPolicy policy = {
        TestErrorRecoveryManager::RecoveryStrategy::RETRY,
        1,
        100,
        1000,
        1.0f,
        [](const TestErrorRecoveryManager::ErrorContext&) {
            return TestErrorRecoveryManager::RecoveryResult::SUCCESS;
        },
        TestErrorRecoveryManager::RecoveryStrategy::DEGRADE_SERVICE
    };
    erm->registerRecoveryPolicy(TestSystemError::SENSOR_FAILURE, policy);

    // Generate error
    erm->handleError(TestSystemError::SENSOR_FAILURE, "SensorA");

    // Check error count
    uint8_t count = erm->getErrorCountForComponent("SensorA", TestSystemError::SENSOR_FAILURE);
    TEST_ASSERT_EQUAL_UINT8(1, count);

    // Advance time past window (5 minutes)
    advanceMockMillis(300001);

    // Generate new error (this will clean up old entries)
    erm->handleError(TestSystemError::SENSOR_FAILURE, "SensorA");

    // Old error should be expired, only new one counted
    count = erm->getErrorCountForComponent("SensorA", TestSystemError::SENSOR_FAILURE);
    TEST_ASSERT_EQUAL_UINT8(1, count);

    erm_teardown();
}

void test_erm_escalation_trigger() {
    erm_setup();

    // Register policy that tracks escalation through degraded mode flag
    // Note: escalation happens after MAX_ERRORS_PER_WINDOW (10) errors
    TestErrorRecoveryManager::RecoveryPolicy policy = {
        TestErrorRecoveryManager::RecoveryStrategy::RETRY,
        1,
        100,
        1000,
        1.0f,
        [](const TestErrorRecoveryManager::ErrorContext&) {
            return TestErrorRecoveryManager::RecoveryResult::SUCCESS;
        },
        TestErrorRecoveryManager::RecoveryStrategy::DEGRADE_SERVICE
    };
    erm->registerRecoveryPolicy(TestSystemError::SENSOR_FAILURE, policy);

    // Generate 10 errors (escalation threshold)
    for (int i = 0; i < 10; i++) {
        erm->handleError(TestSystemError::SENSOR_FAILURE, "SensorA");
        advanceMockMillis(100);
    }

    // The 11th error should trigger escalation to DEGRADE_SERVICE
    g_degradedModeSet = false;  // Reset flag
    erm->handleError(TestSystemError::SENSOR_FAILURE, "SensorA");

    // Degraded mode should be set due to escalation
    TEST_ASSERT_TRUE(g_degradedModeSet);

    erm_teardown();
}

void test_erm_emergency_stop_escalation() {
    erm_setup();

    // Register relay fault policy that escalates to emergency stop
    TestErrorRecoveryManager::RecoveryPolicy policy = {
        TestErrorRecoveryManager::RecoveryStrategy::RESET_COMPONENT,
        2,
        100,
        1000,
        1.0f,
        nullptr,  // No custom action - will fail
        TestErrorRecoveryManager::RecoveryStrategy::EMERGENCY_STOP
    };
    erm->registerRecoveryPolicy(TestSystemError::RELAY_FAULT, policy);

    // Generate 10 errors to trigger escalation
    for (int i = 0; i < 10; i++) {
        erm->handleError(TestSystemError::RELAY_FAULT, "RelayModule");
        advanceMockMillis(100);
    }

    // The 11th error should escalate to emergency stop
    g_emergencyStopSet = false;
    auto result = erm->handleError(TestSystemError::RELAY_FAULT, "RelayModule");

    TEST_ASSERT_TRUE(g_emergencyStopSet);
    TEST_ASSERT_EQUAL(TestErrorRecoveryManager::RecoveryResult::ESCALATED, result);

    erm_teardown();
}

void test_erm_stats_tracking() {
    erm_setup();

    // Register policy that succeeds
    TestErrorRecoveryManager::RecoveryPolicy successPolicy = {
        TestErrorRecoveryManager::RecoveryStrategy::RETRY,
        1,
        100,
        1000,
        1.0f,
        [](const TestErrorRecoveryManager::ErrorContext&) {
            return TestErrorRecoveryManager::RecoveryResult::SUCCESS;
        },
        TestErrorRecoveryManager::RecoveryStrategy::DEGRADE_SERVICE
    };
    erm->registerRecoveryPolicy(TestSystemError::SENSOR_FAILURE, successPolicy);

    // Register policy that fails
    TestErrorRecoveryManager::RecoveryPolicy failPolicy = {
        TestErrorRecoveryManager::RecoveryStrategy::RESET_COMPONENT,
        1,
        100,
        1000,
        1.0f,
        nullptr,  // No action - will fail
        TestErrorRecoveryManager::RecoveryStrategy::NONE
    };
    erm->registerRecoveryPolicy(TestSystemError::NETWORK_ERROR, failPolicy);

    // Generate successes
    erm->handleError(TestSystemError::SENSOR_FAILURE, "Sensor1");
    erm->handleError(TestSystemError::SENSOR_FAILURE, "Sensor2");

    // Generate failures
    erm->handleError(TestSystemError::NETWORK_ERROR, "Network");

    auto stats = erm->getStats();
    TEST_ASSERT_EQUAL_UINT32(3, stats.totalErrors);
    TEST_ASSERT_EQUAL_UINT32(2, stats.successfulRecoveries);
    TEST_ASSERT_EQUAL_UINT32(1, stats.failedRecoveries);
    TEST_ASSERT_EQUAL_UINT32(2, stats.errorCounts[TestSystemError::SENSOR_FAILURE]);
    TEST_ASSERT_EQUAL_UINT32(1, stats.errorCounts[TestSystemError::NETWORK_ERROR]);

    erm_teardown();
}

void test_erm_clear_error_history() {
    erm_setup();

    // Register policy
    TestErrorRecoveryManager::RecoveryPolicy policy = {
        TestErrorRecoveryManager::RecoveryStrategy::RETRY,
        1,
        100,
        1000,
        1.0f,
        [](const TestErrorRecoveryManager::ErrorContext&) {
            return TestErrorRecoveryManager::RecoveryResult::SUCCESS;
        },
        TestErrorRecoveryManager::RecoveryStrategy::DEGRADE_SERVICE
    };
    erm->registerRecoveryPolicy(TestSystemError::SENSOR_FAILURE, policy);

    // Generate errors
    erm->handleError(TestSystemError::SENSOR_FAILURE, "SensorA");
    erm->handleError(TestSystemError::SENSOR_FAILURE, "SensorA");

    // Verify errors recorded
    uint8_t count = erm->getErrorCountForComponent("SensorA", TestSystemError::SENSOR_FAILURE);
    TEST_ASSERT_EQUAL_UINT8(2, count);

    // Clear history
    erm->clearErrorHistory("SensorA");

    // Verify cleared
    count = erm->getErrorCountForComponent("SensorA", TestSystemError::SENSOR_FAILURE);
    TEST_ASSERT_EQUAL_UINT8(0, count);

    erm_teardown();
}

void test_erm_custom_recovery_action() {
    erm_setup();

    static int customActionCallCount = 0;
    customActionCallCount = 0;

    TestErrorRecoveryManager::RecoveryPolicy policy = {
        TestErrorRecoveryManager::RecoveryStrategy::RETRY_WITH_BACKOFF,
        3,
        100,
        1000,
        1.0f,
        [](const TestErrorRecoveryManager::ErrorContext& ctx) {
            customActionCallCount++;
            // Succeed on third attempt
            return (customActionCallCount >= 3)
                ? TestErrorRecoveryManager::RecoveryResult::SUCCESS
                : TestErrorRecoveryManager::RecoveryResult::FAILED;
        },
        TestErrorRecoveryManager::RecoveryStrategy::DEGRADE_SERVICE
    };
    erm->registerRecoveryPolicy(TestSystemError::SENSOR_FAILURE, policy);

    auto result = erm->handleError(TestSystemError::SENSOR_FAILURE, "TestSensor");

    TEST_ASSERT_EQUAL(TestErrorRecoveryManager::RecoveryResult::SUCCESS, result);
    TEST_ASSERT_EQUAL_INT(3, customActionCallCount);  // Called 3 times

    erm_teardown();
}

void test_erm_multiple_components_isolated() {
    erm_setup();

    // Register policy
    TestErrorRecoveryManager::RecoveryPolicy policy = {
        TestErrorRecoveryManager::RecoveryStrategy::RETRY,
        1,
        100,
        1000,
        1.0f,
        [](const TestErrorRecoveryManager::ErrorContext&) {
            return TestErrorRecoveryManager::RecoveryResult::SUCCESS;
        },
        TestErrorRecoveryManager::RecoveryStrategy::DEGRADE_SERVICE
    };
    erm->registerRecoveryPolicy(TestSystemError::SENSOR_FAILURE, policy);

    // Generate errors for different components
    erm->handleError(TestSystemError::SENSOR_FAILURE, "SensorA");
    erm->handleError(TestSystemError::SENSOR_FAILURE, "SensorA");
    erm->handleError(TestSystemError::SENSOR_FAILURE, "SensorB");

    // Each component should have isolated error history
    TEST_ASSERT_EQUAL_UINT8(2, erm->getErrorCountForComponent("SensorA", TestSystemError::SENSOR_FAILURE));
    TEST_ASSERT_EQUAL_UINT8(1, erm->getErrorCountForComponent("SensorB", TestSystemError::SENSOR_FAILURE));

    erm_teardown();
}

void test_erm_degrade_service_strategy() {
    erm_setup();

    // Register policy that immediately degrades
    TestErrorRecoveryManager::RecoveryPolicy policy = {
        TestErrorRecoveryManager::RecoveryStrategy::DEGRADE_SERVICE,
        1,
        100,
        1000,
        1.0f,
        nullptr,
        TestErrorRecoveryManager::RecoveryStrategy::NONE
    };
    erm->registerRecoveryPolicy(TestSystemError::SENSOR_FAILURE, policy);

    auto result = erm->handleError(TestSystemError::SENSOR_FAILURE, "TestSensor");

    TEST_ASSERT_EQUAL(TestErrorRecoveryManager::RecoveryResult::SUCCESS, result);
    TEST_ASSERT_TRUE(g_degradedModeSet);

    erm_teardown();
}
