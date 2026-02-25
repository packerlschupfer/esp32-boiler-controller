// src/modules/control/BurnerRuntimeTracker.cpp
#include "BurnerRuntimeTracker.h"
#include "core/SystemResourceProvider.h"
#include "events/SystemEventsGenerated.h"
#include "utils/CriticalDataStorage.h"
#include "utils/Utils.h"
#include <RuntimeStorage.h>
#include <esp_log.h>
#include <atomic>

static const char* TAG = "BurnerRuntime";

// Static member definition (M4 fix: atomic, was plain uint32_t in Round 21 extraction)
std::atomic<uint32_t> BurnerRuntimeTracker::burnerStartTime{0};

void BurnerRuntimeTracker::recordStartTime() {
    // Record start time for runtime tracking (if transitioning from non-running state)
    if (burnerStartTime.load() == 0) {
        burnerStartTime.store(millis());
    }
}

void BurnerRuntimeTracker::updateRuntimeCounters() {
    LOG_INFO(TAG, "Updating runtime counters");

    // Round 15 Issue #1 fix: Calculate and update runtime hours atomically
    // Use Utils::elapsedMs() for safe elapsed time (handles millis() wraparound)
    uint32_t startTime = burnerStartTime.load();
    if (startTime > 0) {
        uint32_t runTimeMs = Utils::elapsedMs(startTime);
        float runTimeHours = runTimeMs / 3600000.0f;  // Convert ms to hours

        rtstorage::RuntimeStorage* storage = SRP::getRuntimeStorage();
        if (storage) {
            // Update total runtime - ACCUMULATE, don't overwrite
            float totalHours = storage->getRuntimeHours(rtstorage::RUNTIME_TOTAL) + runTimeHours;
            if (storage->updateRuntimeHours(rtstorage::RUNTIME_TOTAL, totalHours)) {
                // Use integer arithmetic for precise HH:MM:SS display
                uint32_t totalSeconds = runTimeMs / 1000;
                int runHours = totalSeconds / 3600;
                int runMinutes = (totalSeconds % 3600) / 60;
                int runSeconds = totalSeconds % 60;
                LOG_INFO(TAG, "Runtime: %d:%02d:%02d (Total: %d.%d hours)",
                        runHours, runMinutes, runSeconds,
                        (int)totalHours, (int)(totalHours * 10) % 10);
            }

            // Update burner runtime - ACCUMULATE
            float burnerHours = storage->getRuntimeHours(rtstorage::RUNTIME_BURNER) + runTimeHours;
            (void)storage->updateRuntimeHours(rtstorage::RUNTIME_BURNER, burnerHours);

            // Also update critical data storage counters
            uint32_t runSecs = (uint32_t)(runTimeHours * 3600);
            CriticalDataStorage::incrementRuntimeCounter(runSecs, true);

            // Update heating or water runtime based on current mode - ACCUMULATE
            EventBits_t systemBits = xEventGroupGetBits(SRP::getSystemStateEventGroup());
            if (systemBits & SystemEvents::SystemState::WATER_ON) {
                float waterHours = storage->getRuntimeHours(rtstorage::RUNTIME_WATER) + runTimeHours;
                (void)storage->updateRuntimeHours(rtstorage::RUNTIME_WATER, waterHours);
                CriticalDataStorage::incrementCycleCounter(false);  // Water cycle
            } else {
                float heatingHours = storage->getRuntimeHours(rtstorage::RUNTIME_HEATING) + runTimeHours;
                (void)storage->updateRuntimeHours(rtstorage::RUNTIME_HEATING, heatingHours);
            }
        }

        burnerStartTime.store(0);  // Reset for next run
    }
}

uint32_t BurnerRuntimeTracker::getStartTime() {
    return burnerStartTime.load();
}
