// src/monitoring/HealthMonitor.cpp
#include "HealthMonitor.h"
#include <esp_system.h>
#include <esp_heap_caps.h>
#include "modules/tasks/OTATask.h"  // suppress DRAM checks during an OTA
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
      // Both are plain structs with no default member initialisers, so without this
      // they hold indeterminate values until the first updateMetrics(). In practice
      // initialize() runs a metric pass before gHealthMonitor is published, but a
      // getMemoryMetrics() on a constructed-but-uninitialised instance would read
      // garbage - and the health report publishes these fields verbatim.
      memoryMetrics_{},
      memoryWarningThreshold_(SystemConstants::System::MIN_FREE_HEAP_WARNING),
      memoryCriticalThreshold_(SystemConstants::System::MIN_FREE_HEAP_CRITICAL),
      networkMetrics_{},
      networkStartTime_(0),
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
    
    // Calculate error rates BEFORE judging subsystem health: updateSubsystemHealth()
    // now marks a subsystem unhealthy from errorRateFP, and running it first made that
    // decision act on the previous cycle's rate - one tick stale.
    calculateErrorRates();

    // Update subsystem health
    updateSubsystemHealth();

    // Calculate overall health
    overallHealth_ = calculateHealthStatus();
    
    lastHealthCheckTime_ = now;
}

void HealthMonitor::recordSuccessLocked(Subsystem subsystem) {
    // Caller must already hold healthMutex_.
    size_t index = static_cast<size_t>(subsystem);
    if (index < subsystemHealth_.size()) {
        subsystemHealth_[index].successCount++;
    }
}

void HealthMonitor::recordErrorLocked(Subsystem subsystem, SystemError error) {
    // Caller must already hold healthMutex_.
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

void HealthMonitor::recordSuccess(Subsystem subsystem) {
    SemaphoreGuard guard(healthMutex_, pdMS_TO_TICKS(50));
    if (!guard.hasLock()) {
        return;
    }
    recordSuccessLocked(subsystem);
}

void HealthMonitor::recordError(Subsystem subsystem, SystemError error) {
    SemaphoreGuard guard(healthMutex_, pdMS_TO_TICKS(50));
    if (!guard.hasLock()) {
        return;
    }
    recordErrorLocked(subsystem, error);
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

size_t HealthMonitor::generateHealthReport(char* buffer, size_t size) const {
    if (!buffer || size < 2) {
        return 0;
    }
    buffer[0] = '\0';

    SemaphoreGuard guard(healthMutex_, pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        // The previous version returned "{}" here, which is indistinguishable from a
        // clean report with nothing to say. A report nobody could produce is news.
        const int n = snprintf(buffer, size, "{\"st\":\"unknown\",\"err\":\"busy\"}");
        return (n > 0 && static_cast<size_t>(n) < size) ? static_cast<size_t>(n) : 0;
    }

    JsonDocument doc;  // ArduinoJson v7 - trimmed to `size` below, never emitted raw

    doc["st"] = healthStatusToString(overallHealth_);
    doc["up"] = millis() / 1000;

    // Memory: the DRAM figures, deliberately not currentFreeHeap. The getFreeHeap()
    // family counts IRAM (~57 kB) and stays reassuring while the DRAM that actually
    // serves allocations is down to a few hundred bytes - measured at 576 B free
    // under ordinary HA polling on 2026-09-16.
    doc["blk"] = memoryMetrics_.largestFreeBlock;
    doc["min"] = memoryMetrics_.minFreeDram;
    if (memoryMetrics_.allocationFailures > 0) {
        doc["af"] = memoryMetrics_.allocationFailures;
    }

    doc["nc"] = networkMetrics_.isConnected ? 1 : 0;
    doc["nd"] = networkMetrics_.disconnectCount;
    doc["na"] = networkMetrics_.availabilityFP;  // fixed point, 10000 = 100%

    // No task metrics: the whole task-monitoring apparatus was removed on 2026-09-16
    // because registerTask() had no callers, so taskCount_ was permanently 0.

    constexpr size_t N = static_cast<size_t>(Subsystem::NUM_SUBSYSTEMS);

    // Only subsystems worth reading about: eight all-zero entries would crowd out the
    // one that matters. "nt" is how many were notable, "n" how many fit.
    size_t notable = 0;
    for (size_t i = 0; i < N; i++) {
        if (!subsystemHealth_[i].isHealthy || subsystemHealth_[i].errorCount > 0) {
            notable++;
        }
    }
    doc["nt"] = notable;

    JsonObject subs = doc["sub"].to<JsonObject>();
    size_t shown = 0;
    bool full = false;

    // Unhealthy first, then merely-erroring, so truncation sheds the least important.
    for (int pass = 0; pass < 2 && !full; pass++) {
        for (size_t i = 0; i < N && !full; i++) {
            const auto& health = subsystemHealth_[i];
            const bool include = (pass == 0) ? !health.isHealthy
                                            : (health.isHealthy && health.errorCount > 0);
            if (!include) continue;

            const char* name = subsystemToString(static_cast<Subsystem>(i));
            JsonObject entry = subs[name].to<JsonObject>();
            entry["e"] = health.errorCount;
            entry["r"] = health.errorRateFP;  // fixed point, 10000 = 100%
            entry["h"] = health.isHealthy ? 1 : 0;

            doc["n"] = shown + 1;
            if (measureJson(doc) >= size) {  // >= keeps room for the NUL
                subs.remove(name);
                full = true;
                break;
            }
            shown++;
        }
    }
    doc["n"] = shown;  // <= 8, so never wider than the value measured above

    if (measureJson(doc) >= size) {
        // Base fields alone overflow a short buffer: emit valid JSON rather than a
        // truncated fragment. Cannot happen at the 320-byte MQTT payload size.
        const int n = snprintf(buffer, size, "{\"st\":\"%s\",\"err\":\"nofit\"}",
                               healthStatusToString(overallHealth_));
        return (n > 0 && static_cast<size_t>(n) < size) ? static_cast<size_t>(n) : 0;
    }

    return serializeJson(doc, buffer, size);
}



void HealthMonitor::updateMemoryMetrics() {
    memoryMetrics_.currentFreeHeap = ESP.getFreeHeap();
    memoryMetrics_.minFreeHeap = ESP.getMinFreeHeap();
    memoryMetrics_.maxAllocHeap = ESP.getMaxAllocHeap();
    memoryMetrics_.largestFreeBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    // Same source as heap_dram_min on boiler/status/health (MQTTPublisher.cpp) and the
    // diagnostics memory dump, so the three agree. minFreeHeap above is kept for the
    // existing struct API but is ESP.getMinFreeHeap() - MALLOC_CAP_INTERNAL, ~56 kB of
    // which most is IRAM malloc() cannot hand out - and must not be reported as DRAM.
    memoryMetrics_.minFreeDram = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    memoryMetrics_.lastUpdateTime = millis();
    
    // Judge memory on what malloc() can actually serve. The previous check compared
    // currentFreeHeap (ESP.getFreeHeap(), i.e. MALLOC_CAP_INTERNAL, ~58 kB on this
    // device) against memoryCriticalThreshold_/memoryWarningThreshold_ (10/20 kB), so
    // it could never fire - most of that "free heap" is IRAM-only and malloc() cannot
    // return it. Measured 2026-09-16: 57 kB "free" with a 48 kB "largest block" while
    // byte-addressable DRAM was 7 kB with a 4.2 kB ceiling, and ordinary Home Assistant
    // polling took free DRAM down to 576 B without anything noticing.
    //
    // Gate on the largest free block, not total free: allocations fail on contiguity,
    // and total free DRAM legitimately dips to a few hundred bytes during a poll burst.
    // Skipped during an OTA, where Update.begin()'s 4096 B sector buffer drops the
    // largest block to ~1.5 kB by design until onOTAStart frees memory.
    //
    // Detection only: recordError() just counts and logs, so this cannot degrade the
    // burner. The DEGRADED failsafe path runs through
    // ErrorHandler::handleCriticalError -> attemptMemoryRecovery(), which is
    // deliberately left alone until thresholds are confirmed over a cold-day cycle.
    if (OTATask::isOtaInProgress()) {
        return;
    }

    const size_t dramFree = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t dramBlock = memoryMetrics_.largestFreeBlock;

    if (dramBlock < SystemConstants::System::MIN_DRAM_BLOCK_CRITICAL) {
        LOG_ERROR("HealthMonitor", "Critical DRAM: largest block %u B, %u B free (internal heap %u B)",
                  static_cast<unsigned>(dramBlock), static_cast<unsigned>(dramFree),
                  static_cast<unsigned>(memoryMetrics_.currentFreeHeap));
        recordErrorLocked(Subsystem::MEMORY, SystemError::SYSTEM_LOW_MEMORY);
    } else if (dramBlock < SystemConstants::System::MIN_DRAM_BLOCK_WARNING) {
        LOG_WARN("HealthMonitor", "Low DRAM: largest block %u B, %u B free",
                 static_cast<unsigned>(dramBlock), static_cast<unsigned>(dramFree));
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
        recordErrorLocked(Subsystem::NETWORK, SystemError::NETWORK_NOT_CONNECTED);

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
        recordSuccessLocked(Subsystem::NETWORK);

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
    constexpr uint32_t QUIET_PERIOD_MS = 300000;  // 5 min without an error = recovered

    // isHealthy previously had no way to become false: it was initialised true and
    // only ever re-set to true here, so calculateHealthStatus()'s
    // `if (!health.isHealthy) return FAILED;` could never fire and the report's
    // per-subsystem "healthy" field was a constant. This adds the missing half.
    //
    // Recovery is tested FIRST and deliberately stays time-based. errorRateFP is
    // cumulative since boot and never decays, and some subsystems only ever call
    // recordError() and never recordSuccess() - MQTT is one - so a single error pins
    // them at 100% permanently. Rate-based recovery would latch those unhealthy
    // forever; a quiet period cannot. Testing recovery first also gives hysteresis:
    // a subsystem goes unhealthy on a sustained rate and clears only after it has
    // genuinely stopped erroring, not merely when the average dips.
    for (auto& health : subsystemHealth_) {
        const uint32_t totalOps = health.successCount + health.errorCount;
        const bool quiet = (totalOps > 0) && ((now - health.lastErrorTime) > QUIET_PERIOD_MS);

        if (quiet) {
            health.isHealthy = true;
        } else if (health.errorRateFP > SystemConstants::Diagnostics::SUBSYSTEM_UNHEALTHY_RATE_FP) {
            // errorRateFP is 0 below MIN_SAMPLES_FOR_STATISTICS (calculateErrorRates),
            // so a couple of early errors cannot trip this.
            health.isHealthy = false;
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
    
    // Check memory against the largest free DRAM block, not currentFreeHeap.
    //
    // This was the last surviving inert comparison of the set: currentFreeHeap is
    // ESP.getFreeHeap() (MALLOC_CAP_INTERNAL, ~57 kB here because it counts IRAM the
    // allocator cannot hand out), tested against memoryCriticalThreshold_/
    // memoryWarningThreshold_ of 10/20 kB - so neither branch could ever be taken,
    // even with 192 bytes of real free DRAM. Same reasoning and same limits as
    // updateMemoryMetrics(): allocations fail on contiguity, and the largest block is
    // the stable signal, whereas total free DRAM dips to a few hundred bytes during a
    // normal Home Assistant poll burst.
    //
    // Skipped during an OTA for the same reason as the gate above: Update.begin()
    // takes a 4096 B sector buffer and drops the largest block to ~1.5 kB by design
    // until onOTAStart frees memory - that is expected, not ill health.
    if (!OTATask::isOtaInProgress()) {
        if (memoryMetrics_.largestFreeBlock < SystemConstants::System::MIN_DRAM_BLOCK_CRITICAL) {
            return HealthStatus::CRITICAL;
        } else if (memoryMetrics_.largestFreeBlock < SystemConstants::System::MIN_DRAM_BLOCK_WARNING) {
            worstStatus = HealthStatus::WARNING;
        }
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

    // Reset overall health
    instance.overallHealth_ = HealthStatus::UNKNOWN;
    instance.lastHealthCheckTime_ = 0;

    // Note: healthMutex_ is NOT reset - it persists across test runs
    // Test framework should handle FreeRTOS cleanup if needed
}
#endif