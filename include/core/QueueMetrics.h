#ifndef QUEUE_METRICS_H
#define QUEUE_METRICS_H

#include <cstdint>
#include <algorithm>
#include <array>
#include "config/SystemConstants.h"

/**
 * @brief Queue performance metrics
 *
 * Tracks queue usage statistics for monitoring and optimization.
 * Uses fixed-point arithmetic (scale 10000 = 100%) to avoid float operations.
 */
class QueueMetrics {
public:
    // Fixed-point scale: 10000 = 100%, 8000 = 80%, 100 = 1%
    static constexpr uint16_t FP_SCALE = 10000;

    // Drop reasons
    enum class DropReason {
        QUEUE_FULL,
        PRIORITY,
        TIMEOUT,
        MEMORY_POOL_EXHAUSTED,
        EMERGENCY_MODE,
        OTHER
    };
    
    QueueMetrics() { reset(); }
    
    // Update methods
    void recordSend(bool success, size_t queueDepth);
    void recordReceive(size_t queueDepth);
    void recordDrop(DropReason reason);
    void recordOverflow();
    void updateUtilization(size_t current, size_t max);
    
    // Reset all metrics
    void reset();
    
    // Basic metrics
    uint32_t getTotalSent() const { return totalSent_; }
    uint32_t getTotalReceived() const { return totalReceived_; }
    uint32_t getTotalDropped() const { return totalDropped_; }
    uint32_t getOverflowCount() const { return overflowCount_; }
    
    // High water mark
    size_t getHighWaterMark() const { return highWaterMark_; }
    uint32_t getTimeAtHighWater() const { return timeAtHighWater_; }
    
    // Drop statistics
    uint32_t getDropsByReason(DropReason reason) const;
    uint16_t getDropRateFP() const;  // Fixed-point (0-10000 = 0-100%)

    // Utilization (fixed-point: 0-10000 = 0-100%)
    uint16_t getAverageUtilizationFP() const { return averageUtilizationFP_; }
    uint16_t getCurrentUtilizationFP() const { return currentUtilizationFP_; }
    
    // Performance
    uint32_t getMaxSendTime() const { return maxSendTime_; }
    uint32_t getAverageSendTime() const;
    
    // Time-based metrics
    uint32_t getTimeSinceLastSend() const;
    uint32_t getTimeSinceLastReceive() const;
    uint32_t getTimeSinceLastDrop() const;
    
    // Health indicators
    bool isHealthy() const;
    bool hasRecentDrops() const;
    bool isNearCapacity() const;
    
    // JSON representation for diagnostics
    void toJSON(char* buffer, size_t bufferSize) const;
    
private:
    // Basic counters
    uint32_t totalSent_;
    uint32_t totalReceived_;
    uint32_t totalDropped_;
    uint32_t overflowCount_;
    
    // Drop reasons tracking
    std::array<uint32_t, 6> dropsByReason_;
    
    // Utilization tracking (fixed-point: 0-10000 = 0-100%)
    size_t highWaterMark_;
    uint32_t timeAtHighWater_;
    uint16_t averageUtilizationFP_;
    uint16_t currentUtilizationFP_;
    uint32_t utilizationSamples_;
    
    // Performance metrics
    uint32_t maxSendTime_;
    uint32_t totalSendTime_;
    uint32_t sendCount_;
    
    // Timestamps
    uint32_t lastSendTime_;
    uint32_t lastReceiveTime_;
    uint32_t lastDropTime_;
    uint32_t startTime_;
    
    // Rolling window for drop rate calculation
    static constexpr size_t WINDOW_SIZE = SystemConstants::QueueManagement::METRICS_WINDOW_SIZE;
    std::array<uint8_t, WINDOW_SIZE> dropWindow_;
    size_t windowIndex_;
    
    // Health thresholds (fixed-point: 100 = 1%, 8000 = 80%)
    static constexpr uint16_t HEALTHY_DROP_RATE_FP = 100;   // 1% drop rate
    static constexpr uint16_t WARNING_UTILIZATION_FP = 8000; // 80% utilization
    static constexpr uint32_t RECENT_TIME_MS = SystemConstants::QueueManagement::RECENT_TIME_MS;
};

#endif // QUEUE_METRICS_H