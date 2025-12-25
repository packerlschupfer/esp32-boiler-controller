// src/monitoring/HealthMonitor.cpp
#include "HealthMonitor.h"
#include <esp_system.h>
#include <ArduinoJson.h>
#include <SemaphoreGuard.h>
#include "EthernetManager.h"
#include "config/ProjectConfig.h"
#include "config/SystemConstants.h"
#include "events/SystemEventsGenerated.h"
#include "core/SystemResourceProvider.h"

// Global instance
HealthMonitor* gHealthMonitor = nullptr;

HealthMonitor::HealthMonitor() 
    : healthMutex_(nullptr),
      memoryWarningThreshold_(SystemConstants::System::MIN_FREE_HEAP_WARNING),
      memoryCriticalThreshold_(SystemConstants::System::MIN_FREE_HEAP_CRITICAL),
      networkStartTime_(0),
      taskCount_(0),
      overallHealth_(HealthStatus::GOOD),
      lastHealthCheckTime_(0) {
}

HealthMonitor::~HealthMonitor() {
    if (healthMutex_ != nullptr) {
        vSemaphoreDelete(healthMutex_);
    }
}

Result<void> HealthMonitor::initialize() {
    // Create mutex
    healthMutex_ = xSemaphoreCreateMutex();
    if (healthMutex_ == nullptr) {
        return Result<void>(SystemError::MUTEX_CREATE_FAILED, "Failed to create health monitor mutex");
    }
    
    // Initialize metrics
    memoryMetrics_ = MemoryMetrics();
    networkMetrics_ = NetworkMetrics();
    networkStartTime_ = millis();
    
    // Initialize subsystem health
    for (auto& health : subsystemHealth_) {
        health = SubsystemHealth();
    }
    
    // Perform initial metric update
    updateMetrics();
    
    LOG_INFO("HealthMonitor", "Health monitoring initialized");
    return Result<void>();
}

void HealthMonitor::updateMetrics() {
    SemaphoreGuard guard(healthMutex_, pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        return;
    }
    
    uint32_t now = millis();
    
    // Update memory metrics
    updateMemoryMetrics();
    
    // Update network metrics
    updateNetworkMetrics();
    
    // Update subsystem health
    updateSubsystemHealth();
    
    // Calculate error rates
    calculateErrorRates();
    
    // Check task health
    checkTaskHealth();
    
    // Calculate overall health
    overallHealth_ = calculateHealthStatus();
    
    lastHealthCheckTime_ = now;
}

void HealthMonitor::recordSuccess(Subsystem subsystem) {
    SemaphoreGuard guard(healthMutex_, pdMS_TO_TICKS(50));
    if (!guard.hasLock()) {
        return;
    }
    
    size_t index = static_cast<size_t>(subsystem);
    if (index < subsystemHealth_.size()) {
        subsystemHealth_[index].successCount++;
    }
}

void HealthMonitor::recordError(Subsystem subsystem, SystemError error) {
    SemaphoreGuard guard(healthMutex_, pdMS_TO_TICKS(50));
    if (!guard.hasLock()) {
        return;
    }
    
    size_t index = static_cast<size_t>(subsystem);
    if (index < subsystemHealth_.size()) {
        auto& health = subsystemHealth_[index];
        health.errorCount++;
        health.lastError = error;
        health.lastErrorTime = millis();
        
        // Log significant errors
        if (health.errorCount % 10 == 0) {  // Every 10th error
            LOG_WARN("HealthMonitor", "%s subsystem: %lu errors, last: %s",
                    subsystemToString(subsystem),
                    health.errorCount,
                    ErrorHandler::errorToString(error));
        }
    }
}

HealthMonitor::HealthStatus HealthMonitor::getOverallHealth() const {
    SemaphoreGuard guard(healthMutex_, pdMS_TO_TICKS(50));
    if (!guard.hasLock()) {
        return HealthStatus::WARNING;  // Can't determine, assume warning
    }
    
    return overallHealth_;
}

HealthMonitor::HealthStatus HealthMonitor::getSubsystemHealth(Subsystem subsystem) const {
    SemaphoreGuard guard(healthMutex_, pdMS_TO_TICKS(50));
    if (!guard.hasLock()) {
        return HealthStatus::WARNING;
    }
    
    size_t index = static_cast<size_t>(subsystem);
    if (index >= subsystemHealth_.size()) {
        return HealthStatus::FAILED;
    }
    
    const auto& health = subsystemHealth_[index];

    // Determine health based on error rate (fixed-point: 100 = 1%)
    if (!health.isHealthy) {
        return HealthStatus::FAILED;
    } else if (health.errorRateFP > 1000) {  // > 10%
        return HealthStatus::CRITICAL;
    } else if (health.errorRateFP > 500) {   // > 5%
        return HealthStatus::WARNING;
    } else if (health.errorRateFP > 100) {   // > 1%
        return HealthStatus::GOOD;
    } else {
        return HealthStatus::EXCELLENT;
    }
}

HealthMonitor::MemoryMetrics HealthMonitor::getMemoryMetrics() const {
    SemaphoreGuard guard(healthMutex_, pdMS_TO_TICKS(50));
    if (!guard.hasLock()) {
        return MemoryMetrics();
    }
    
    return memoryMetrics_;
}

HealthMonitor::NetworkMetrics HealthMonitor::getNetworkMetrics() const {
    SemaphoreGuard guard(healthMutex_, pdMS_TO_TICKS(50));
    if (!guard.hasLock()) {
        return NetworkMetrics();
    }
    
    return networkMetrics_;
}

bool HealthMonitor::shouldEnterFailsafe() const {
    SemaphoreGuard guard(healthMutex_, pdMS_TO_TICKS(50));
    if (!guard.hasLock()) {
        return false;  // Don't trigger failsafe if we can't check
    }
    
    // Check critical conditions
    if (overallHealth_ == HealthStatus::FAILED) {
        return true;
    }
    
    // Check memory
    if (memoryMetrics_.currentFreeHeap < memoryCriticalThreshold_) {
        return true;
    }
    
    // Check critical subsystems
    if (getSubsystemHealth(Subsystem::CONTROL) == HealthStatus::FAILED ||
        getSubsystemHealth(Subsystem::SENSORS) == HealthStatus::FAILED ||
        getSubsystemHealth(Subsystem::RELAYS) == HealthStatus::FAILED) {
        return true;
    }
    
    return false;
}

std::string HealthMonitor::generateHealthReport() const {
    SemaphoreGuard guard(healthMutex_, pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        return "{}";
    }
    
    JsonDocument doc;  // ArduinoJson v7 - elastic capacity

    // Overall health
    doc["status"] = healthStatusToString(overallHealth_);
    doc["timestamp"] = millis();

    // Memory metrics
    JsonObject memory = doc["memory"].to<JsonObject>();
    memory["free"] = memoryMetrics_.currentFreeHeap;
    memory["min_free"] = memoryMetrics_.minFreeHeap;
    memory["max_alloc"] = memoryMetrics_.maxAllocHeap;
    memory["alloc_failures"] = memoryMetrics_.allocationFailures;

    // Network metrics
    JsonObject network = doc["network"].to<JsonObject>();
    network["connected"] = networkMetrics_.isConnected;
    network["disconnects"] = networkMetrics_.disconnectCount;
    network["availability"] = networkMetrics_.availabilityFP / 100.0f;  // Convert FP to percentage

    // Subsystem health
    JsonObject subsystems = doc["subsystems"].to<JsonObject>();
    for (size_t i = 0; i < static_cast<size_t>(Subsystem::NUM_SUBSYSTEMS); i++) {
        const auto& health = subsystemHealth_[i];
        JsonObject subsys = subsystems[subsystemToString(static_cast<Subsystem>(i))].to<JsonObject>();
        subsys["success"] = health.successCount;
        subsys["errors"] = health.errorCount;
        subsys["error_rate"] = health.errorRateFP / 100.0f;  // Convert FP to percentage
        subsys["healthy"] = health.isHealthy;
    }

    // Task health
    JsonArray tasks = doc["tasks"].to<JsonArray>();
    for (size_t i = 0; i < taskCount_; i++) {
        const auto& task = taskMetrics_[i];
        JsonObject taskObj = tasks.add<JsonObject>();
        taskObj["name"] = task.name;
        taskObj["stack_free"] = task.stackHighWaterMark;
        taskObj["healthy"] = task.isHealthy;
    }
    
    std::string output;
    serializeJson(doc, output);
    return output;
}

void HealthMonitor::registerTask(TaskHandle_t handle, const char* name) {
    SemaphoreGuard guard(healthMutex_, pdMS_TO_TICKS(50));
    if (!guard.hasLock()) {
        return;
    }
    
    if (taskCount_ < MAX_MONITORED_TASKS && handle != nullptr) {
        taskMetrics_[taskCount_].name = name;
        taskMetrics_[taskCount_].stackHighWaterMark = uxTaskGetStackHighWaterMark(handle);
        taskMetrics_[taskCount_].lastCheckTime = millis();
        taskMetrics_[taskCount_].isHealthy = true;
        taskCount_++;
    }
}

void HealthMonitor::checkTaskHealth() {
    // Note: This is called with mutex already held
    for (size_t i = 0; i < taskCount_; i++) {
        auto& task = taskMetrics_[i];
        
        // Get task handle by name (would need TaskManager integration)
        // For now, just check if stack is critically low
        if (task.stackHighWaterMark < 256) {
            task.isHealthy = false;
            LOG_WARN("HealthMonitor", "Task %s has low stack: %d bytes",
                    task.name, task.stackHighWaterMark);
        }
        
        task.lastCheckTime = millis();
    }
}

void HealthMonitor::updateMemoryMetrics() {
    memoryMetrics_.currentFreeHeap = ESP.getFreeHeap();
    memoryMetrics_.minFreeHeap = ESP.getMinFreeHeap();
    memoryMetrics_.maxAllocHeap = ESP.getMaxAllocHeap();
    memoryMetrics_.largestFreeBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    memoryMetrics_.lastUpdateTime = millis();
    
    // Check for critically low memory
    if (memoryMetrics_.currentFreeHeap < memoryCriticalThreshold_) {
        LOG_ERROR("HealthMonitor", "Critical memory level: %d bytes free", memoryMetrics_.currentFreeHeap);
        recordError(Subsystem::MEMORY, SystemError::SYSTEM_LOW_MEMORY);
    } else if (memoryMetrics_.currentFreeHeap < memoryWarningThreshold_) {
        LOG_WARN("HealthMonitor", "Low memory warning: %d bytes free", memoryMetrics_.currentFreeHeap);
    }
}

void HealthMonitor::updateNetworkMetrics() {
    bool wasConnected = networkMetrics_.isConnected;
    networkMetrics_.isConnected = EthernetManager::isConnected();
    
    uint32_t now = millis();
    
    // Track disconnections - set event bit to notify other tasks immediately
    if (wasConnected && !networkMetrics_.isConnected) {
        networkMetrics_.disconnectCount++;
        networkMetrics_.lastDisconnectTime = now;
        recordError(Subsystem::NETWORK, SystemError::NETWORK_NOT_CONNECTED);

        // Proactively notify other tasks via event bits
        xEventGroupSetBits(SRP::getErrorNotificationEventGroup(),
                          SystemEvents::Error::NETWORK);
        LOG_WARN("HealthMonitor", "Ethernet link DOWN - notifying tasks");
    }

    // Track reconnections - clear error bit when link is restored
    if (!wasConnected && networkMetrics_.isConnected) {
        networkMetrics_.reconnectCount++;
        if (networkMetrics_.lastDisconnectTime > 0) {
            networkMetrics_.totalDowntime += (now - networkMetrics_.lastDisconnectTime);
        }
        recordSuccess(Subsystem::NETWORK);

        // Clear network error bit when link is restored
        xEventGroupClearBits(SRP::getErrorNotificationEventGroup(),
                            SystemEvents::Error::NETWORK);
        LOG_INFO("HealthMonitor", "Ethernet link UP - connection restored");
    }
    
    // Calculate availability using fixed-point percentage scale
    uint32_t totalTime = now - networkStartTime_;
    if (totalTime > 0) {
        constexpr auto SCALE = SystemConstants::FixedPoint::PERCENTAGE_SCALE;
        // availabilityFP = SCALE * (1 - downtime/total) = SCALE - (SCALE * downtime / total)
        networkMetrics_.availabilityFP = static_cast<uint16_t>(
            SCALE - (static_cast<uint32_t>(SCALE) * networkMetrics_.totalDowntime / totalTime));
    }
}

void HealthMonitor::updateSubsystemHealth() {
    uint32_t now = millis();
    
    // Check for stale subsystems (no activity in 5 minutes)
    for (auto& health : subsystemHealth_) {
        uint32_t totalOps = health.successCount + health.errorCount;
        if (totalOps > 0 && (now - health.lastErrorTime) > 300000) {
            // No errors in 5 minutes, mark as healthy
            health.isHealthy = true;
        }
    }
}

void HealthMonitor::calculateErrorRates() {
    constexpr auto SCALE = SystemConstants::FixedPoint::PERCENTAGE_SCALE;
    constexpr auto MIN_SAMPLES = SystemConstants::Diagnostics::MIN_SAMPLES_FOR_STATISTICS;

    for (auto& health : subsystemHealth_) {
        uint32_t totalOps = health.successCount + health.errorCount;
        if (totalOps >= MIN_SAMPLES) {
            // Fixed-point percentage: errorRateFP = (errorCount * SCALE) / totalOps
            health.errorRateFP = static_cast<uint16_t>((health.errorCount * static_cast<uint32_t>(SCALE)) / totalOps);
        } else {
            health.errorRateFP = 0;  // Not enough data
        }
    }
}

HealthMonitor::HealthStatus HealthMonitor::calculateHealthStatus() const {
    // Start with excellent
    HealthStatus worstStatus = HealthStatus::EXCELLENT;
    
    // Check memory
    if (memoryMetrics_.currentFreeHeap < memoryCriticalThreshold_) {
        return HealthStatus::CRITICAL;
    } else if (memoryMetrics_.currentFreeHeap < memoryWarningThreshold_) {
        worstStatus = HealthStatus::WARNING;
    }
    
    // Check network
    if (!networkMetrics_.isConnected) {
        worstStatus = std::max(worstStatus, HealthStatus::WARNING);
    }
    if (networkMetrics_.availabilityFP < 9000) {  // < 90%
        worstStatus = std::max(worstStatus, HealthStatus::WARNING);
    }

    // Check subsystems (fixed-point: 100 = 1%)
    for (const auto& health : subsystemHealth_) {
        if (!health.isHealthy) {
            return HealthStatus::FAILED;
        }
        if (health.errorRateFP > 1000) {  // > 10%
            worstStatus = std::max(worstStatus, HealthStatus::CRITICAL);
        } else if (health.errorRateFP > 500) {  // > 5%
            worstStatus = std::max(worstStatus, HealthStatus::WARNING);
        }
    }
    
    // Check tasks
    for (size_t i = 0; i < taskCount_; i++) {
        if (!taskMetrics_[i].isHealthy) {
            worstStatus = std::max(worstStatus, HealthStatus::WARNING);
        }
    }
    
    return worstStatus;
}

const char* HealthMonitor::healthStatusToString(HealthStatus status) const {
    switch (status) {
        case HealthStatus::EXCELLENT: return "excellent";
        case HealthStatus::GOOD: return "good";
        case HealthStatus::WARNING: return "warning";
        case HealthStatus::CRITICAL: return "critical";
        case HealthStatus::FAILED: return "failed";
        default: return "unknown";
    }
}

const char* HealthMonitor::subsystemToString(Subsystem subsystem) const {
    switch (subsystem) {
        case Subsystem::MEMORY: return "memory";
        case Subsystem::NETWORK: return "network";
        case Subsystem::MODBUS: return "modbus";
        case Subsystem::SENSORS: return "sensors";
        case Subsystem::RELAYS: return "relays";
        case Subsystem::CONTROL: return "control";
        case Subsystem::MQTT: return "mqtt";
        case Subsystem::BLE: return "ble";
        default: return "unknown";
    }
}

HealthMonitor& HealthMonitor::getInstance() {
    static HealthMonitor instance;
    return instance;
}

#ifdef UNIT_TEST
void HealthMonitor::resetForTesting() {
    auto& instance = getInstance();

    // Reset all subsystem health
    instance.subsystemHealth_.fill(SubsystemHealth{});

    // Reset memory metrics
    instance.memoryMetrics_ = MemoryMetrics{};
    instance.memoryWarningThreshold_ = 10240;  // Default 10KB
    instance.memoryCriticalThreshold_ = 5120;  // Default 5KB

    // Reset network metrics
    instance.networkMetrics_ = NetworkMetrics{};
    instance.networkStartTime_ = 0;

    // Reset task metrics
    instance.taskMetrics_.fill(TaskMetrics{});
    instance.taskCount_ = 0;

    // Reset overall health
    instance.overallHealth_ = HealthStatus::UNKNOWN;
    instance.lastHealthCheckTime_ = 0;

    // Note: healthMutex_ is NOT reset - it persists across test runs
    // Test framework should handle FreeRTOS cleanup if needed
}
#endif