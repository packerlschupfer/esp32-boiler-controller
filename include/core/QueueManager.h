#ifndef QUEUE_MANAGER_H
#define QUEUE_MANAGER_H

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <unordered_map>
#include <string>
#include <functional>
#include <memory>
#include "utils/MemoryPool.h"
#include "core/QueueMetrics.h"
#include "config/SystemConstants.h"

/**
 * @brief Centralized queue management system
 *
 * Provides lifecycle management, metrics, overflow handling, and
 * memory pool integration for all system queues.
 */
class QueueManager {
public:
    // Maximum supported item size for queue operations (MQTTPublishRequest is ~400 bytes)
    static constexpr size_t MAX_QUEUE_ITEM_SIZE = 512;
    // Overflow handling strategies
    enum class OverflowStrategy {
        DROP_OLDEST,      // Drop oldest message (default)
        DROP_NEWEST,      // Drop new message
        DROP_LOWEST_PRIORITY,  // Drop based on priority
        BLOCK,            // Block until space available
        CALLBACK         // Call custom handler
    };
    
    // Queue configuration
    struct QueueConfig {
        UBaseType_t length;
        UBaseType_t itemSize;
        OverflowStrategy overflowStrategy = OverflowStrategy::DROP_OLDEST;
        bool useMemoryPool = false;
        std::function<void(void*)> overflowCallback = nullptr;
        uint32_t warningThreshold = 80;  // Percentage full to trigger warning
    };
    
    // Managed queue wrapper
    class ManagedQueue {
    public:
        ManagedQueue(const std::string& name, const QueueConfig& config);
        ~ManagedQueue();
        
        // Queue operations
        bool send(const void* item, TickType_t timeout = 0);
        bool sendFromISR(const void* item, BaseType_t* higherPriorityTaskWoken);
        bool receive(void* item, TickType_t timeout = portMAX_DELAY);
        bool peek(void* item, TickType_t timeout = 0);
        
        // Status
        UBaseType_t getMessagesWaiting() const;
        UBaseType_t getSpacesAvailable() const;
        bool isFull() const;
        bool isEmpty() const;
        bool isValid() const { return handle_ != nullptr; }  // Check if queue was created successfully
        
        // Metrics
        const QueueMetrics& getMetrics() const { return metrics_; }
        void resetMetrics() { metrics_.reset(); }
        
        // Configuration
        const std::string& getName() const { return name_; }
        const QueueConfig& getConfig() const { return config_; }
        
    private:
        std::string name_;
        QueueConfig config_;
        QueueHandle_t handle_;
        QueueMetrics metrics_;
        void* memoryPool_;  // Optional memory pool - placeholder for now

        // Circuit breaker for priority drop - prevents cascading failures
        // M16: Increased from 3 to 10 - transient issues during reconnect shouldn't trip breaker
        static constexpr uint8_t CIRCUIT_BREAKER_THRESHOLD = 10;
        static constexpr uint32_t CIRCUIT_BREAKER_RECOVERY_MS = 60000;  // Round 14 Issue #15: 1 minute recovery
        uint8_t consecutiveRestoreFailures_ = 0;
        bool circuitBreakerTripped_ = false;
        uint32_t circuitBreakerTripTime_ = 0;  // Round 14 Issue #15: Track when tripped

        bool handleOverflow(const void* item);
        void updateMetrics(bool sent, bool dropped);
        void checkCircuitBreakerRecovery();  // Round 14 Issue #15
    };
    
    // Get singleton instance
    static QueueManager& getInstance();

    #ifdef UNIT_TEST
    /**
     * @brief Reset singleton for testing - NOT IMPLEMENTED
     *
     * DESIGN NOTE: QueueManager manages FreeRTOS queues used throughout the system
     * for inter-task communication. Resetting this would be extremely dangerous:
     *
     * 1. Managed queues contain active FreeRTOS queue handles
     * 2. Tasks may be blocked on queue receive/send operations
     * 3. Queues may contain messages that haven't been consumed
     * 4. Circuit breakers and metrics track time-sensitive state
     * 5. Emergency mode affects system-wide queue behavior
     *
     * RECOMMENDED APPROACH: Use mocks/test doubles in unit tests:
     *   - Create mock queues for testing
     *   - Test components independently without real FreeRTOS queues
     *   - Use fake/spy patterns to verify queue operations
     *
     * For integration tests that need real queues, restart the test process.
     */
    static void resetForTesting() {
        // Intentionally not implemented - see comment above
        // Use mocks in tests instead
    }
    #endif

    // Queue management
    std::shared_ptr<ManagedQueue> createQueue(const std::string& name, const QueueConfig& config);
    std::shared_ptr<ManagedQueue> getQueue(const std::string& name);
    bool deleteQueue(const std::string& name);
    
    // Task association (for automatic cleanup)
    void associateQueueWithTask(const std::string& queueName, TaskHandle_t task);
    void cleanupTaskQueues(TaskHandle_t task);
    
    // Global metrics
    void getGlobalMetrics(size_t& totalQueues, size_t& totalMessages, size_t& totalDropped);
    void publishMetrics();  // Publish to MQTT diagnostics
    
    // Health monitoring
    bool isHealthy() const;
    uint16_t getAverageUtilizationFP() const;  // Fixed-point: 0-10000 = 0-100%
    size_t getCriticalQueueCount() const;
    
    // Emergency operations
    void flushAllQueues();
    void enterEmergencyMode();  // Bypass normal queue operations
    void exitEmergencyMode();
    
private:
    QueueManager();
    ~QueueManager() = default;
    QueueManager(const QueueManager&) = delete;
    QueueManager& operator=(const QueueManager&) = delete;
    
    std::unordered_map<std::string, std::shared_ptr<ManagedQueue>> queues_;
    std::unordered_map<TaskHandle_t, std::vector<std::string>> taskQueues_;
    SemaphoreHandle_t mutex_;
    bool emergencyMode_;
    
    static constexpr uint32_t METRICS_PUBLISH_INTERVAL_MS = SystemConstants::QueueManagement::METRICS_PUBLISH_INTERVAL_MS;
    uint32_t lastMetricsPublish_;
};

// Convenience macro for getting a queue
#define GET_MANAGED_QUEUE(name) \
    (QueueManager::getInstance().getQueue(name))

#endif // QUEUE_MANAGER_H