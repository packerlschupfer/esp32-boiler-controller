// src/core/QueueManager.cpp
#include "core/QueueManager.h"
#include "LoggingMacros.h"
#include "diagnostics/MQTTDiagnostics.h"
#include "utils/MutexGuard.h"
#include "modules/tasks/MQTTTask.h"  // For MQTTMessage and MQTTPriority
#include <algorithm>
#include <cstring>

// Static member initialization
constexpr uint32_t QueueManager::METRICS_PUBLISH_INTERVAL_MS;

// ManagedQueue implementation
QueueManager::ManagedQueue::ManagedQueue(const std::string& name, const QueueConfig& config)
    : name_(name)
    , config_(config)
    , handle_(nullptr)
    , memoryPool_(nullptr) {
    
    // Create the FreeRTOS queue
    handle_ = xQueueCreate(config.length, config.itemSize);
    if (!handle_) {
        LOG_ERROR("QueueManager", "Failed to create queue: %s", name.c_str());
        return;
    }
    
    // Create memory pool if requested
    if (config.useMemoryPool) {
        // Memory pools are created per-queue-item-type externally
        // This is just a placeholder for now
        memoryPool_ = nullptr;
    }
    
    LOG_INFO("QueueManager", "Created queue '%s' (length=%u, itemSize=%u)", 
             name.c_str(), config.length, config.itemSize);
}

QueueManager::ManagedQueue::~ManagedQueue() {
    if (handle_) {
        vQueueDelete(handle_);
        handle_ = nullptr;
    }
    
    // Memory pool is managed externally - don't delete void*
    // if (memoryPool_) {
    //     delete memoryPool_;
    //     memoryPool_ = nullptr;
    // }
    
    LOG_INFO("QueueManager", "Destroyed queue: %s", name_.c_str());
}

bool QueueManager::ManagedQueue::send(const void* item, TickType_t timeout) {
    if (!handle_) {
        LOG_ERROR("QueueManager", "send() called with null handle for queue: %s", name_.c_str());
        return false;
    }
    if (!item) {
        LOG_ERROR("QueueManager", "send() called with null item for queue: %s", name_.c_str());
        return false;
    }
    
    // Check if queue is full
    if (uxQueueSpacesAvailable(handle_) == 0) {
        if (!handleOverflow(item)) {
            metrics_.recordDrop(QueueMetrics::DropReason::QUEUE_FULL);
            return false;
        }
    }
    
    // Try to send
    bool success = xQueueSend(handle_, item, timeout) == pdTRUE;
    
    // Update metrics
    updateMetrics(success, !success);
    
    if (success) {
        metrics_.recordSend(true, uxQueueMessagesWaiting(handle_));
    }
    
    return success;
}

bool QueueManager::ManagedQueue::sendFromISR(const void* item, BaseType_t* higherPriorityTaskWoken) {
    if (!handle_ || !item) return false;
    
    // Note: FreeRTOS doesn't have uxQueueSpacesAvailableFromISR
    // We'll try to send and handle failure
    
    // Try to send from ISR
    bool success = xQueueSendFromISR(handle_, item, higherPriorityTaskWoken) == pdTRUE;
    
    if (success) {
        metrics_.recordSend(true, uxQueueMessagesWaitingFromISR(handle_));
    } else {
        metrics_.recordDrop(QueueMetrics::DropReason::QUEUE_FULL);
    }
    
    return success;
}

bool QueueManager::ManagedQueue::receive(void* item, TickType_t timeout) {
    if (!handle_ || !item) return false;
    
    bool success = xQueueReceive(handle_, item, timeout) == pdTRUE;
    
    if (success) {
        metrics_.recordReceive(uxQueueMessagesWaiting(handle_));
    }
    
    return success;
}

bool QueueManager::ManagedQueue::peek(void* item, TickType_t timeout) {
    if (!handle_ || !item) return false;
    
    return xQueuePeek(handle_, item, timeout) == pdTRUE;
}

UBaseType_t QueueManager::ManagedQueue::getMessagesWaiting() const {
    return handle_ ? uxQueueMessagesWaiting(handle_) : 0;
}

UBaseType_t QueueManager::ManagedQueue::getSpacesAvailable() const {
    return handle_ ? uxQueueSpacesAvailable(handle_) : 0;
}

bool QueueManager::ManagedQueue::isFull() const {
    return handle_ ? (uxQueueSpacesAvailable(handle_) == 0) : true;
}

bool QueueManager::ManagedQueue::isEmpty() const {
    return handle_ ? (uxQueueMessagesWaiting(handle_) == 0) : true;
}

bool QueueManager::ManagedQueue::handleOverflow(const void* item) {
    switch (config_.overflowStrategy) {
        case OverflowStrategy::DROP_OLDEST: {
            // Remove oldest item to make space
            // Use fixed buffer instead of VLA for safety
            uint8_t dummy[QueueManager::MAX_QUEUE_ITEM_SIZE];
            if (config_.itemSize > sizeof(dummy)) {
                LOG_ERROR("QueueManager", "Item size %u exceeds max %zu", config_.itemSize, sizeof(dummy));
                return false;
            }
            if (xQueueReceive(handle_, dummy, 0) == pdTRUE) {
                metrics_.recordDrop(QueueMetrics::DropReason::QUEUE_FULL);
                return true;  // Space now available
            }
            break;
        }
        
        case OverflowStrategy::DROP_NEWEST:
            // Just drop the new item
            return false;
            
        case OverflowStrategy::DROP_LOWEST_PRIORITY: {
            // Implement priority-based dropping for MQTT messages
            // This assumes the queue contains messages with a priority field

            // Circuit breaker: if too many restore failures, fall back to DROP_NEWEST
            if (circuitBreakerTripped_) {
                LOG_WARN("QueueManager", "Circuit breaker active - using DROP_NEWEST fallback");
                return false;  // Drop the new message instead of complex priority drop
            }

            // For MQTT messages, we need to scan the queue and find lowest priority
            // Note: This is a simplified implementation that works with MQTTPublishRequest

            // Check if this is an MQTT queue (by checking item size)
            if (config_.itemSize == sizeof(MQTTPublishRequest)) {
                // Two-pass algorithm to minimize stack usage:
                // Pass 1: Scan queue to find lowest priority message index
                // Pass 2: Re-read queue, dropping only the target message

                const int MAX_SCAN = 8;  // Scan up to 8 messages for better priority handling
                int queueDepth = uxQueueMessagesWaiting(handle_);
                int scanDepth = (queueDepth < MAX_SCAN) ? queueDepth : MAX_SCAN;

                if (scanDepth == 0) {
                    return false;  // Queue empty, nothing to drop
                }

                // Pass 1: Find lowest priority (highest enum value = lowest priority)
                MQTTPublishRequest tempMsg;
                MQTTPriority lowestPriority = MQTTPriority::PRIORITY_HIGH;
                int lowestPriorityIndex = 0;

                for (int i = 0; i < scanDepth; i++) {
                    if (xQueueReceive(handle_, &tempMsg, 0) == pdTRUE) {
                        if (tempMsg.priority > lowestPriority) {
                            lowestPriority = tempMsg.priority;
                            lowestPriorityIndex = i;
                        }
                        // Put it back at the end to maintain scan order
                        xQueueSendToBack(handle_, &tempMsg, 0);
                    }
                }

                // Pass 2: Remove only the lowest priority message
                // Messages are now rotated, so we need to cycle through again
                bool dropped = false;
                for (int i = 0; i < scanDepth; i++) {
                    if (xQueueReceive(handle_, &tempMsg, 0) == pdTRUE) {
                        if (i == lowestPriorityIndex && !dropped) {
                            // This is the one we're dropping
                            LOG_DEBUG("QueueManager", "Dropped low priority message (priority=%d)",
                                     (int)tempMsg.priority);
                            dropped = true;
                            metrics_.recordDrop(QueueMetrics::DropReason::QUEUE_FULL);
                            // Don't put this one back
                        } else {
                            // Put it back - H12: Retry with emergency drop if queue full
                            if (xQueueSendToBack(handle_, &tempMsg, pdMS_TO_TICKS(10)) != pdTRUE) {
                                // H12: Queue unexpectedly full - another producer added during rotation
                                // Emergency recovery: drop oldest message to make room
                                MQTTPublishRequest oldestMsg;
                                if (xQueueReceive(handle_, &oldestMsg, 0) == pdTRUE) {
                                    LOG_WARN("QueueManager", "Emergency drop of oldest msg to restore rotated msg");
                                    metrics_.recordDrop(QueueMetrics::DropReason::QUEUE_FULL);

                                    // Retry restoring current message
                                    if (xQueueSendToBack(handle_, &tempMsg, pdMS_TO_TICKS(10)) != pdTRUE) {
                                        // Still failed - this shouldn't happen, record loss
                                        LOG_ERROR("QueueManager", "CRITICAL: Lost message even after emergency drop (priority=%d)",
                                                 (int)tempMsg.priority);
                                        metrics_.recordDrop(QueueMetrics::DropReason::QUEUE_FULL);
                                        consecutiveRestoreFailures_++;
                                    } else {
                                        consecutiveRestoreFailures_ = 0;
                                    }
                                } else {
                                    // Can't receive from queue - something very wrong
                                    LOG_ERROR("QueueManager", "CRITICAL: Lost message during priority drop (priority=%d)",
                                             (int)tempMsg.priority);
                                    metrics_.recordDrop(QueueMetrics::DropReason::QUEUE_FULL);
                                    consecutiveRestoreFailures_++;
                                }

                                // Track consecutive failures for circuit breaker
                                if (consecutiveRestoreFailures_ >= CIRCUIT_BREAKER_THRESHOLD) {
                                    circuitBreakerTripped_ = true;
                                    circuitBreakerTripTime_ = millis();
                                    LOG_ERROR("QueueManager", "CIRCUIT BREAKER TRIPPED after %d restore failures! "
                                             "Falling back to DROP_NEWEST strategy",
                                             consecutiveRestoreFailures_);
                                }
                            } else {
                                // Successful restore - reset failure counter
                                consecutiveRestoreFailures_ = 0;
                            }
                        }
                    }
                }

                return dropped;
            }
            
            // For non-MQTT queues, fall back to dropping oldest
            LOG_WARN("QueueManager", "Priority dropping not supported for this queue type, using DROP_OLDEST");
            uint8_t dummy[QueueManager::MAX_QUEUE_ITEM_SIZE];
            if (config_.itemSize > sizeof(dummy)) {
                LOG_ERROR("QueueManager", "Item size %u exceeds max %zu", config_.itemSize, sizeof(dummy));
                return false;
            }
            if (xQueueReceive(handle_, dummy, 0) == pdTRUE) {
                metrics_.recordDrop(QueueMetrics::DropReason::QUEUE_FULL);
                return true;
            }
            return false;
        }
            
        case OverflowStrategy::BLOCK:
            // Let the caller block
            return true;
            
        case OverflowStrategy::CALLBACK:
            if (config_.overflowCallback) {
                config_.overflowCallback(const_cast<void*>(item));
            }
            return false;
    }
    
    return false;
}

void QueueManager::ManagedQueue::updateMetrics(bool sent, bool dropped) {
    // Update utilization
    UBaseType_t current = uxQueueMessagesWaiting(handle_);
    metrics_.updateUtilization(current, config_.length);

    // Round 16 Issue #3: Use 64-bit intermediate to prevent overflow
    // If current is near UINT32_MAX/100, (current * 100) would overflow before division
    // Example: current = 42949673 → current * 100 = 4294967300 > UINT32_MAX
    // Using uint64_t ensures safe calculation for any queue size
    uint32_t utilizationPct = 0;
    if (config_.length > 0) {
        utilizationPct = static_cast<uint32_t>((static_cast<uint64_t>(current) * 100ULL) / config_.length);
    }

    if (utilizationPct >= config_.warningThreshold) {
        LOG_WARN("QueueManager", "Queue '%s' at %lu%% capacity", name_.c_str(), (unsigned long)utilizationPct);
    }

    // Round 14 Issue #15: Check circuit breaker recovery
    checkCircuitBreakerRecovery();
}

// Round 14 Issue #15: Circuit breaker recovery check
void QueueManager::ManagedQueue::checkCircuitBreakerRecovery() {
    if (!circuitBreakerTripped_) {
        return;
    }

    uint32_t now = millis();
    uint32_t elapsed = now - circuitBreakerTripTime_;

    if (elapsed >= CIRCUIT_BREAKER_RECOVERY_MS) {
        LOG_INFO("QueueManager", "Circuit breaker recovery period elapsed (%lu ms) - resetting for '%s'",
                 elapsed, name_.c_str());
        circuitBreakerTripped_ = false;
        consecutiveRestoreFailures_ = 0;
        circuitBreakerTripTime_ = 0;
    }
}

// QueueManager implementation
QueueManager::QueueManager() 
    : emergencyMode_(false)
    , lastMetricsPublish_(0) {
    
    mutex_ = xSemaphoreCreateMutex();
    if (!mutex_) {
        LOG_ERROR("QueueManager", "Failed to create mutex");
    }
}

QueueManager& QueueManager::getInstance() {
    static QueueManager instance;
    return instance;
}

std::shared_ptr<QueueManager::ManagedQueue> QueueManager::createQueue(
    const std::string& name, const QueueConfig& config) {

    MutexGuard guard(mutex_);

    // Check if queue already exists
    auto it = queues_.find(name);
    if (it != queues_.end()) {
        LOG_WARN("QueueManager", "Queue '%s' already exists", name.c_str());
        return it->second;
    }

    // Create new managed queue
    auto queue = std::make_shared<ManagedQueue>(name, config);

    // Validate queue was created successfully before storing
    if (!queue->isValid()) {
        LOG_ERROR("QueueManager", "Queue '%s' creation failed - not storing", name.c_str());
        return nullptr;
    }

    queues_[name] = queue;

    return queue;
}

std::shared_ptr<QueueManager::ManagedQueue> QueueManager::getQueue(const std::string& name) {
    MutexGuard guard(mutex_);
    
    auto it = queues_.find(name);
    return (it != queues_.end()) ? it->second : nullptr;
}

bool QueueManager::deleteQueue(const std::string& name) {
    MutexGuard guard(mutex_);
    
    auto it = queues_.find(name);
    if (it == queues_.end()) {
        return false;
    }
    
    // Remove from task associations
    for (auto& taskEntry : taskQueues_) {
        auto& queueNames = taskEntry.second;
        queueNames.erase(
            std::remove(queueNames.begin(), queueNames.end(), name),
            queueNames.end()
        );
    }
    
    // Remove queue
    queues_.erase(it);
    return true;
}

void QueueManager::associateQueueWithTask(const std::string& queueName, TaskHandle_t task) {
    MutexGuard guard(mutex_);
    
    taskQueues_[task].push_back(queueName);
}

void QueueManager::cleanupTaskQueues(TaskHandle_t task) {
    MutexGuard guard(mutex_);
    
    auto it = taskQueues_.find(task);
    if (it == taskQueues_.end()) {
        return;
    }
    
    // Delete all queues associated with this task
    for (const auto& queueName : it->second) {
        queues_.erase(queueName);
    }
    
    taskQueues_.erase(it);
}

void QueueManager::getGlobalMetrics(size_t& totalQueues, size_t& totalMessages, size_t& totalDropped) {
    MutexGuard guard(mutex_);
    
    totalQueues = queues_.size();
    totalMessages = 0;
    totalDropped = 0;
    
    for (const auto& entry : queues_) {
        const auto& metrics = entry.second->getMetrics();
        totalMessages += entry.second->getMessagesWaiting();
        totalDropped += metrics.getTotalDropped();
    }
}

void QueueManager::publishMetrics() {
    MQTTDiagnostics* diagnostics = MQTTDiagnostics::getInstance();
    if (!diagnostics || !diagnostics->isEnabled()) {
        return;
    }
    
    // Check publish interval
    uint32_t now = millis();
    if (now - lastMetricsPublish_ < METRICS_PUBLISH_INTERVAL_MS) {
        return;
    }
    lastMetricsPublish_ = now;
    
    MutexGuard guard(mutex_);

    // Round 20 Issue #9: Static buffers reduce stack pressure (ESP32 optimization)
    // THREAD-SAFETY: ✅ Mutex-protected - MutexGuard ensures safe access
    // RATIONALE: MQTT task has only 712B free stack, 256B+128B+64B=448B would be fatal
    // See: docs/MEMORY_OPTIMIZATION.md for complete rationale
    // Build metrics JSON
    static char buffer[256];  // Reduced from 512, actual usage ~150 bytes
    uint16_t avgUtil = getAverageUtilizationFP();
    snprintf(buffer, sizeof(buffer),
        "{\"queues\":%zu,\"emergency\":%s,\"healthy\":%s,\"avgUtil\":%u.%02u,\"critical\":%zu}",
        queues_.size(),
        emergencyMode_ ? "true" : "false",
        isHealthy() ? "true" : "false",
        avgUtil / 100, avgUtil % 100,  // Fixed-point to X.XX%
        getCriticalQueueCount()
    );
    
    diagnostics->publishDiagnostics("queues", buffer, true);
    
    // Publish individual queue metrics for critical queues
    for (const auto& entry : queues_) {
        const auto& queue = entry.second;
        const auto& metrics = queue->getMetrics();
        
        if (!metrics.isHealthy() || queue->getConfig().warningThreshold > 0) {
            // Round 20 Issue #9: Static buffers reduce stack pressure (mutex-protected above)
            static char queueBuffer[128];  // Reduced from 256, actual usage ~100 bytes
            metrics.toJSON(queueBuffer, sizeof(queueBuffer));

            static char topic[64];  // Reduced from 128
            snprintf(topic, sizeof(topic), "queues/%s", entry.first.c_str());
            diagnostics->publishDiagnostics(topic, queueBuffer, true);
        }
    }
}

bool QueueManager::isHealthy() const {
    MutexGuard guard(mutex_);
    
    for (const auto& entry : queues_) {
        if (!entry.second->getMetrics().isHealthy()) {
            return false;
        }
    }
    
    return true;
}

uint16_t QueueManager::getAverageUtilizationFP() const {
    MutexGuard guard(mutex_);

    if (queues_.empty()) return 0;

    uint32_t totalUtilization = 0;
    for (const auto& entry : queues_) {
        totalUtilization += entry.second->getMetrics().getCurrentUtilizationFP();
    }

    return static_cast<uint16_t>(totalUtilization / queues_.size());
}

size_t QueueManager::getCriticalQueueCount() const {
    MutexGuard guard(mutex_);
    
    size_t count = 0;
    for (const auto& entry : queues_) {
        const auto& metrics = entry.second->getMetrics();
        if (!metrics.isHealthy() || metrics.isNearCapacity()) {
            count++;
        }
    }
    
    return count;
}

void QueueManager::flushAllQueues() {
    MutexGuard guard(mutex_);

    LOG_WARN("QueueManager", "Flushing all queues");

    // Use fixed buffer instead of VLA
    uint8_t dummy[MAX_QUEUE_ITEM_SIZE];

    for (auto& entry : queues_) {
        auto& queue = entry.second;

        if (queue->getConfig().itemSize > sizeof(dummy)) {
            LOG_ERROR("QueueManager", "Cannot flush queue '%s': item size %u exceeds max %zu",
                     entry.first.c_str(), queue->getConfig().itemSize, sizeof(dummy));
            continue;
        }

        // Flush queue by reading all items
        while (queue->receive(dummy, 0)) {
            // Item discarded
        }

        // Reset metrics
        queue->resetMetrics();
    }
}

void QueueManager::enterEmergencyMode() {
    MutexGuard guard(mutex_);

    if (!emergencyMode_) {
        LOG_ERROR("QueueManager", "Entering emergency mode");
        emergencyMode_ = true;

        // Use fixed buffer instead of VLA
        uint8_t dummy[MAX_QUEUE_ITEM_SIZE];

        // Flush non-critical queues
        for (auto& entry : queues_) {
            const std::string& name = entry.first;

            // Keep critical queues (relay control, safety)
            if (name.find("relay") == std::string::npos &&
                name.find("safety") == std::string::npos &&
                name.find("emergency") == std::string::npos) {

                if (entry.second->getConfig().itemSize > sizeof(dummy)) {
                    LOG_ERROR("QueueManager", "Cannot flush queue '%s': item size exceeds max",
                             name.c_str());
                    continue;
                }

                // Flush non-critical queue
                while (entry.second->receive(dummy, 0)) {
                    // Item discarded
                }
            }
        }
    }
}

void QueueManager::exitEmergencyMode() {
    MutexGuard guard(mutex_);
    
    if (emergencyMode_) {
        LOG_INFO("QueueManager", "Exiting emergency mode");
        emergencyMode_ = false;
    }
}