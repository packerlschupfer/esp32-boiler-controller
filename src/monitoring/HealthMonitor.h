// src/monitoring/HealthMonitor.h
#ifndef HEALTH_MONITOR_H
#define HEALTH_MONITOR_H

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdint>
#include <array>
#include <string>
#include "utils/ErrorHandler.h"
#include "config/SystemConstants.h"
#include "core/SystemResourceProvider.h"

/**
 * @brief System health monitoring and diagnostics
 * 
 * Tracks various system health metrics and provides early warning
 * of potential issues before they become critical failures.
 */
class HealthMonitor {
public:
    /**
     * @brief Health status levels
     */
    enum class HealthStatus {
        EXCELLENT,  // All metrics within ideal range
        GOOD,       // All metrics acceptable
        WARNING,    // Some metrics approaching limits
        CRITICAL,   // Some metrics at critical levels
        FAILED      // System failure detected
    };
    
    /**
     * @brief Subsystem identifiers
     */
    enum class Subsystem {
        MEMORY,
        NETWORK,
        MODBUS,
        SENSORS,
        RELAYS,
        CONTROL,
        MQTT,
        BLE,
        NUM_SUBSYSTEMS
    };
    
    /**
     * @brief Health metrics for a subsystem
     */
    struct SubsystemHealth {
        uint32_t successCount;
        uint32_t errorCount;
        uint32_t lastErrorTime;
        SystemError lastError;
        uint16_t errorRateFP;  // Fixed-point: 100 = 1%, 10000 = 100%
        bool isHealthy;

        SubsystemHealth() : successCount(0), errorCount(0), lastErrorTime(0),
                           lastError(SystemError::SUCCESS), errorRateFP(0), isHealthy(true) {}
    };
    
    /**
     * @brief Memory health metrics
     */
    struct MemoryMetrics {
        size_t currentFreeHeap;
        size_t minFreeHeap;
        size_t maxAllocHeap;
        size_t largestFreeBlock;
        uint32_t allocationFailures;
        uint32_t lastUpdateTime;
    };
    
    /**
     * @brief Task health metrics
     */
    struct TaskMetrics {
        const char* name;
        UBaseType_t stackHighWaterMark;
        uint32_t lastCheckTime;
        bool isHealthy;
    };
    
    /**
     * @brief Network health metrics
     */
    struct NetworkMetrics {
        bool isConnected;
        uint32_t disconnectCount;
        uint32_t reconnectCount;
        uint32_t lastDisconnectTime;
        uint32_t totalDowntime;
        uint16_t availabilityFP;  // Fixed-point: 100 = 1%, 10000 = 100%
    };
    
    /**
     * @brief Constructor
     */
    HealthMonitor();
    
    /**
     * @brief Destructor
     */
    ~HealthMonitor();
    
    /**
     * @brief Initialize the health monitor
     */
    Result<void> initialize();
    
    /**
     * @brief Update health metrics (call periodically)
     */
    void updateMetrics();
    
    /**
     * @brief Record a successful operation
     */
    void recordSuccess(Subsystem subsystem);
    
    /**
     * @brief Record an error
     */
    void recordError(Subsystem subsystem, SystemError error);
    
    /**
     * @brief Get overall system health status
     */
    HealthStatus getOverallHealth() const;
    
    /**
     * @brief Get health status for a specific subsystem
     */
    HealthStatus getSubsystemHealth(Subsystem subsystem) const;
    
    /**
     * @brief Get current memory metrics
     */
    MemoryMetrics getMemoryMetrics() const;
    
    /**
     * @brief Get current network metrics
     */
    NetworkMetrics getNetworkMetrics() const;
    
    /**
     * @brief Check if system should enter failsafe mode
     */
    bool shouldEnterFailsafe() const;
    
    /**
     * @brief Generate health report JSON
     */
    std::string generateHealthReport() const;
    
    /**
     * @brief Register a task for monitoring
     */
    void registerTask(TaskHandle_t handle, const char* name);
    
    /**
     * @brief Check task health
     */
    void checkTaskHealth();
    
    /**
     * @brief Set memory warning threshold
     */
    void setMemoryWarningThreshold(size_t bytes) { memoryWarningThreshold_ = bytes; }
    
    /**
     * @brief Set memory critical threshold
     */
    void setMemoryCriticalThreshold(size_t bytes) { memoryCriticalThreshold_ = bytes; }
    
    /**
     * @brief Get singleton instance
     */
    static HealthMonitor& getInstance();

    #ifdef UNIT_TEST
    /**
     * @brief Reset singleton state for testing
     *
     * TESTING ONLY: Resets all health metrics, counters, and state to initial values.
     * Only compiled when UNIT_TEST is defined (test environments).
     *
     * Usage in tests:
     *   void tearDown() {
     *       HealthMonitor::resetForTesting();
     *   }
     *
     * Note: Does NOT recreate mutex - assumes test framework handles cleanup
     */
    static void resetForTesting();
    #endif

private:
    // Mutex for thread safety
    mutable SemaphoreHandle_t healthMutex_;
    
    // Subsystem health tracking
    std::array<SubsystemHealth, static_cast<size_t>(Subsystem::NUM_SUBSYSTEMS)> subsystemHealth_;
    
    // Memory metrics
    MemoryMetrics memoryMetrics_;
    size_t memoryWarningThreshold_;
    size_t memoryCriticalThreshold_;
    
    // Network metrics
    NetworkMetrics networkMetrics_;
    uint32_t networkStartTime_;
    
    // Task monitoring
    static constexpr size_t MAX_MONITORED_TASKS = 20;
    std::array<TaskMetrics, MAX_MONITORED_TASKS> taskMetrics_;
    size_t taskCount_;
    
    // Overall health
    HealthStatus overallHealth_;
    uint32_t lastHealthCheckTime_;
    
    // Helper methods
    void updateMemoryMetrics();
    void updateNetworkMetrics();
    void updateSubsystemHealth();
    void calculateErrorRates();
    HealthStatus calculateHealthStatus() const;
    const char* healthStatusToString(HealthStatus status) const;
    const char* subsystemToString(Subsystem subsystem) const;
};

// Note: HealthMonitor is accessed via SystemResourceProvider::getHealthMonitor()

// Convenience macros
#define HEALTH_RECORD_SUCCESS(subsystem) \
    if (SystemResourceProvider::getHealthMonitor()) SystemResourceProvider::getHealthMonitor()->recordSuccess(HealthMonitor::Subsystem::subsystem)

#define HEALTH_RECORD_ERROR(subsystem, error) \
    if (SystemResourceProvider::getHealthMonitor()) SystemResourceProvider::getHealthMonitor()->recordError(HealthMonitor::Subsystem::subsystem, error)

#endif // HEALTH_MONITOR_H