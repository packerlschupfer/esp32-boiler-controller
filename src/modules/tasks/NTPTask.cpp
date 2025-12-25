// src/modules/tasks/NTPTaskEventDriven.cpp
// Event-driven version of NTP time synchronization task

#include "modules/tasks/NTPTask.h"
#include "modules/tasks/EventDrivenPatterns.h"
#include "config/SystemConstants.h"
#include "LoggingMacros.h"
#include "core/SystemResourceProvider.h"
#include <Arduino.h>
#include <NTPClient.h>
#include <TaskManager.h>
#include <stdlib.h>
#include <EthernetManager.h>
#include "events/SystemEventsGenerated.h"
#include <freertos/timers.h>
#include "hal/HardwareAbstractionLayer.h"

static const char* TAG = "NTPTask";

// NTP Client instance
static NTPClient ntpClient;

// Event-driven components
static TimerHandle_t autoSyncTimer = nullptr;
static TimerHandle_t statusTimer = nullptr;
static EventAggregator ntpEvents;

// State variables
static NTPSyncCallback rtcUpdateCallback = nullptr;
static bool ntpInitialized = false;
static SemaphoreHandle_t ntpMutex = nullptr;
static time_t lastSyncTime = 0;

// Timer periods - use centralized constants
namespace NTPConstants = SystemConstants::Tasks::NTP;

// Event bits for NTP operations
enum NTPEventBits : uint32_t {
    NTP_EVENT_SYNC_NOW = (1 << 0),
    NTP_EVENT_NETWORK_READY = (1 << 1),
    NTP_EVENT_LOG_STATUS = (1 << 2),
    NTP_EVENT_UPDATE_CONFIG = (1 << 3),
    NTP_EVENT_SYNC_FAILED = (1 << 4)
};

// Time zone configuration - DISABLED
// ESP32 handles timezone via TZ environment variable set in SystemInitializer
// Using both NTPClient timezone and system TZ causes double offset application
static NTPClient::TimeZoneConfig timeZoneUTC = {
    0,            // UTC+0 (no offset - let system handle it)
    "UTC",        // Time zone name
    false,        // No DST (system handles it)
    0, 0, 0, 0,   // No DST start
    0, 0, 0, 0,   // No DST end
    0             // No DST offset
};

/**
 * @brief Timer callback for auto-sync
 */
static void autoSyncTimerCallback(TimerHandle_t xTimer) {
    ntpEvents.setEvent(NTP_EVENT_SYNC_NOW);
}

/**
 * @brief Timer callback for status logging
 */
static void statusTimerCallback(TimerHandle_t xTimer) {
    ntpEvents.setEvent(NTP_EVENT_LOG_STATUS);
}

/**
 * @brief Network state change handler
 * Currently unused but kept for potential future network event integration
 */
// static void onNetworkStateChange(bool connected) {
//     if (connected) {
//         LOG_INFO(TAG, "Network connected - triggering sync");
//         ntpEvents.setEvent(NTP_EVENT_NETWORK_READY | NTP_EVENT_SYNC_NOW);
//     }
// }

// Drift tracking variables
static struct {
    time_t lastSyncTime = 0;
    uint32_t lastSyncMillis = 0;
    int32_t accumulatedDriftMs = 0;
    uint32_t driftSamples = 0;
} driftTracker;

// RTC fallback tracking
static constexpr uint32_t RTC_FALLBACK_THRESHOLD = 5;  // Fall back after 5 NTP failures
static bool rtcFallbackUsed = false;

/**
 * @brief Try to set system time from DS3231 RTC (fallback when NTP unavailable)
 * @return true if RTC time was valid and system time was updated
 */
static bool tryRTCFallback() {
    auto& hal = HAL::HardwareAbstractionLayer::getInstance();
    const auto& config = hal.getConfig();

    if (!config.rtc) {
        LOG_WARN(TAG, "RTC not available for fallback");
        return false;
    }

    if (config.rtc->hasLostPower()) {
        LOG_WARN(TAG, "RTC has lost power - time may be invalid");
        return false;
    }

    auto dt = config.rtc->getDateTime();

    // Validate year (should be > 2020)
    if (dt.year < 2020 || dt.year > 2100) {
        LOG_WARN(TAG, "RTC time invalid (year=%d)", dt.year);
        return false;
    }

    // Convert to time_t
    struct tm timeinfo = {0};
    timeinfo.tm_year = dt.year - 1900;
    timeinfo.tm_mon = dt.month - 1;
    timeinfo.tm_mday = dt.day;
    timeinfo.tm_hour = dt.hour;
    timeinfo.tm_min = dt.minute;
    timeinfo.tm_sec = dt.second;
    timeinfo.tm_isdst = -1;  // Let system determine DST

    time_t rtcTime = mktime(&timeinfo);

    if (rtcTime < 1577836800) {  // Jan 1, 2020
        LOG_WARN(TAG, "RTC epoch invalid: %ld", rtcTime);
        return false;
    }

    // Set system time
    struct timeval tv = { .tv_sec = rtcTime, .tv_usec = 0 };
    settimeofday(&tv, nullptr);

    LOG_INFO(TAG, "System time set from RTC fallback: %04d-%02d-%02d %02d:%02d:%02d",
             dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);

    return true;
}

/**
 * @brief Perform NTP sync operation
 */
static bool performNTPSync() {
    if (!EthernetManager::isConnected()) {
        LOG_WARN(TAG, "Cannot sync - network not connected");
        return false;
    }

    LOG_DEBUG(TAG, "Performing NTP sync...");
    
    auto result = ntpClient.syncTime(5000);  // 5 second timeout
    
    // Note: result.syncTime field appears to have issues on ESP32, 
    // but the actual time sync is working correctly as we use time(nullptr) instead
    
    if (result.success) {
        // Calculate actual clock drift for monitoring
        if (driftTracker.lastSyncTime > 0 && driftTracker.lastSyncMillis > 0) {
            uint32_t millisElapsed = millis() - driftTracker.lastSyncMillis;
            if (millisElapsed > 60000) {  // Only calculate drift after at least 1 minute
                time_t expectedTime = driftTracker.lastSyncTime + (millisElapsed / 1000);
                int64_t timeDiffSeconds = (int64_t)lastSyncTime - (int64_t)expectedTime;

                // Protect against overflow - clamp drift calculation to reasonable range
                // Normal drift should be < 60 seconds; larger differences indicate clock jump
                int32_t driftMs;
                if (timeDiffSeconds > 3600 || timeDiffSeconds < -3600) {
                    LOG_WARN(TAG, "Clock jumped by %lld seconds - skipping drift calc", timeDiffSeconds);
                    driftMs = 0;  // Don't accumulate bogus drift
                } else {
                    driftMs = (int32_t)(timeDiffSeconds * 1000) + (int32_t)(millisElapsed % 1000);
                }
                
                // Update drift statistics
                driftTracker.accumulatedDriftMs += driftMs;
                driftTracker.driftSamples++;
                int32_t avgDriftMs = driftTracker.accumulatedDriftMs / driftTracker.driftSamples;
                float driftPpm = (float)driftMs * 1000000.0f / (float)millisElapsed;
                
                LOG_INFO(TAG, "Time synced from %s, offset %ldms, RTT %dms", 
                         result.serverUsed, result.offsetMs, result.roundTripMs);
                LOG_INFO(TAG, "Clock drift: %ldms over %lus (%.1f ppm), avg: %ldms", 
                         driftMs, millisElapsed / 1000, driftPpm, avgDriftMs);
                
                // Warn if drift is excessive (>1000 ppm = 0.1%)
                if (abs(driftPpm) > 1000) {
                    LOG_WARN(TAG, "Excessive clock drift detected: %.1f ppm", driftPpm);
                }
            } else {
                LOG_INFO(TAG, "Time synced from %s, offset %ldms, RTT %dms", 
                         result.serverUsed, result.offsetMs, result.roundTripMs);
            }
        } else {
            LOG_INFO(TAG, "Initial time sync from %s, offset %ldms, RTT %dms", 
                     result.serverUsed, result.offsetMs, result.roundTripMs);
        }
        driftTracker.lastSyncTime = lastSyncTime;
        driftTracker.lastSyncMillis = millis();
        
        // Store the actual sync time - use the current time after sync
        lastSyncTime = time(nullptr);
        
        // Update RTC if callback is set
        if (rtcUpdateCallback) {
            // Add small delay to ensure system time is updated
            vTaskDelay(pdMS_TO_TICKS(10));
            
            // Use the current system time after NTP sync (which is now correct)
            time_t utcTime = time(nullptr);
            
            // Debug logging - show what we got from NTP
            LOG_DEBUG(TAG, "NTP provided UTC epoch: %ld", utcTime);
            
            // Check if time looks valid (should be > year 2020)
            if (utcTime < 1577836800) { // Jan 1, 2020
                LOG_ERROR(TAG, "Invalid epoch time received: %ld", utcTime);
                LOG_ERROR(TAG, "Server %s returned invalid time!", result.serverUsed);
                LOG_INFO(TAG, "Marking sync as failed - will retry with different server");
                ntpEvents.setEvent(NTP_EVENT_SYNC_FAILED);
                return false;
            }
            
            // Get system time after NTP sync (should be updated now)
            [[maybe_unused]] time_t systemTime = time(nullptr);
            LOG_DEBUG(TAG, "System time after NTP sync: %ld", systemTime);

            // Don't use NTPClient's timezone - system handles it
            // time_t localTime = ntpClient.getLocalTime();  // This applies offset twice!

            // Debug logging
            struct tm utmBuf, ltmBuf;
            [[maybe_unused]] struct tm* utm = gmtime_r(&utcTime, &utmBuf);
            [[maybe_unused]] struct tm* ltm = localtime_r(&utcTime, &ltmBuf);  // Use utcTime with localtime_r to get system TZ applied

            LOG_DEBUG(TAG, "UTC time: %04d-%02d-%02d %02d:%02d:%02d",
                    utm->tm_year + 1900, utm->tm_mon + 1, utm->tm_mday,
                    utm->tm_hour, utm->tm_min, utm->tm_sec);

            // Log timezone information
            [[maybe_unused]] char* tzEnv = getenv("TZ");
            LOG_DEBUG(TAG, "System TZ environment: %s", tzEnv ? tzEnv : "not set");
            
            LOG_DEBUG(TAG, "Local time: %04d-%02d-%02d %02d:%02d:%02d",
                    ltm->tm_year + 1900, ltm->tm_mon + 1, ltm->tm_mday,
                    ltm->tm_hour, ltm->tm_min, ltm->tm_sec);
            
            // Pass UTC time to RTC update callback (it will handle timezone)
            rtcUpdateCallback(utcTime);
        }
        
        // Set system time synced bit
        // Set time synced in general system event group
        EventGroupHandle_t generalSystemEventGroup = SRP::getGeneralSystemEventGroup();
        if (generalSystemEventGroup) {
            xEventGroupSetBits(generalSystemEventGroup, SystemEvents::GeneralSystem::TIME_SYNCED);
        }
        
        return true;
    } else {
        LOG_WARN(TAG, "Sync failed: %s", result.error);
        ntpEvents.setEvent(NTP_EVENT_SYNC_FAILED);
        return false;
    }
}

/**
 * @brief Log NTP status
 */
static void logNTPStatus() {
    if (ntpClient.getLastSyncTime() > 0) {
        time_t timeSinceSync = time(nullptr) - ntpClient.getLastSyncTime();
        const char* formattedTime = ntpClient.getFormattedDateTime();
        
        if (formattedTime) {
            LOG_DEBUG(TAG, "Status: Time=%s, LastSync=%lds ago", 
                     formattedTime, timeSinceSync);
        }
        
        // Check if we need to force a sync (if last sync is too old)
        if (timeSinceSync > 43200) {  // 12 hours
            LOG_WARN(TAG, "Last sync is old - forcing sync");
            ntpEvents.setEvent(NTP_EVENT_SYNC_NOW);
        }
    } else {
        LOG_WARN(TAG, "Status: Not synced yet");
    }
}

/**
 * @brief Event-driven NTP task
 */
void NTPTaskEventDriven(void* parameter) {
    LOG_INFO(TAG, "Starting event-driven NTP task on core %d", xPortGetCoreID());

    // Enable TaskManager watchdog with generous timeout
    // Event loop has 5s timeout, NTP sync 5s max, so 60s provides ample margin
    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::enabled(60000);

    if (!SRP::getTaskManager().registerCurrentTaskWithWatchdog("NTPTask", wdtConfig)) {
        LOG_ERROR(TAG, "WDT reg failed");
    } else {
        LOG_INFO(TAG, "WDT OK 60000ms");
        (void)SRP::getTaskManager().feedWatchdog();  // Feed immediately after registration
    }

    // Create mutex for thread safety
    ntpMutex = xSemaphoreCreateMutex();
    if (!ntpMutex) {
        LOG_ERROR(TAG, "Failed to create mutex");
        vTaskDelete(NULL);
        return;
    }
    
    // Initialize NTP client
    LOG_DEBUG(TAG, "Initializing NTP client...");
    
    // Configure NTP servers
    ntpClient.clearServers();
    // Re-enable local server with enhanced debugging
    (void)ntpClient.addServer("192.168.20.1", 123);       // Local NTP server (gateway)
    (void)ntpClient.addServer("pool.ntp.org", 123);      // Primary public NTP
    (void)ntpClient.addServer("time.google.com", 123);   // Secondary
    (void)ntpClient.addServer("time.cloudflare.com", 123); // Tertiary
    
    // Use UTC in NTPClient - system TZ environment handles timezone
    // This prevents double timezone offset application
    ntpClient.setTimeZone(timeZoneUTC);
    LOG_INFO(TAG, "NTPClient set to UTC (system handles TZ via env)");
    
    // Initialize the NTP client (required by the library)
    ntpClient.begin();  // Initialize with default UDP port
    (void)SRP::getTaskManager().feedWatchdog();  // Feed after potentially slow init

    // Set up NTP callbacks
    ntpClient.onSync([](const NTPClient::SyncResult& result) {
        // This callback is already handled in performNTPSync
        // Could add additional processing here if needed
    });
    
    ntpClient.onTimeChange([](time_t oldTime, time_t newTime) {
        // Avoid logging in callback context to prevent stack overflow
        // The time adjustment is already logged in performNTPSync()
        (void)oldTime;  // Suppress unused parameter warning
        (void)newTime;
    });

    // Register network state change handler (if EthernetManager supports it)
    // EthernetManager::onStateChange(onNetworkStateChange);
    
    ntpInitialized = true;
    LOG_DEBUG(TAG, "NTP client initialized");
    
    // Create timers
    // Note: pdMS_TO_TICKS might overflow for large values
    // Calculate ticks directly to avoid overflow
    TickType_t syncIntervalTicks = (NTPConstants::SYNC_INTERVAL_MS * configTICK_RATE_HZ) / 1000;
    LOG_DEBUG(TAG, "Creating auto-sync timer with interval %lu ms (%lu minutes), %lu ticks", 
             NTPConstants::SYNC_INTERVAL_MS, NTPConstants::SYNC_INTERVAL_MS / 60000, syncIntervalTicks);
    autoSyncTimer = xTimerCreate(
        "NTPAutoSync",
        syncIntervalTicks,
        pdTRUE,  // Auto-reload
        nullptr,
        autoSyncTimerCallback
    );
    
    statusTimer = xTimerCreate(
        "NTPStatus",
        pdMS_TO_TICKS(NTPConstants::STATUS_LOG_INTERVAL_MS),
        pdTRUE,  // Auto-reload
        nullptr,
        statusTimerCallback
    );
    
    if (!autoSyncTimer || !statusTimer) {
        LOG_ERROR(TAG, "Failed to create timers");
        vTaskDelete(NULL);
        return;
    }
    
    // Start timers
    xTimerStart(statusTimer, pdMS_TO_TICKS(100));
    
    // Wait for network before starting auto-sync timer
    LOG_INFO(TAG, "Waiting for network...");

    // Main event loop
    EventGroupHandle_t eventGroup = ntpEvents.getHandle();
    LOG_INFO(TAG, "Event group handle: %p", eventGroup);
    if (!eventGroup) {
        LOG_ERROR(TAG, "Event group is NULL - cannot run event loop!");
        // Fall back to simple delay loop
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            (void)SRP::getTaskManager().feedWatchdog();
        }
    }

    const EventBits_t ALL_EVENTS = NTP_EVENT_SYNC_NOW | NTP_EVENT_NETWORK_READY |
                                   NTP_EVENT_LOG_STATUS | NTP_EVENT_UPDATE_CONFIG | NTP_EVENT_SYNC_FAILED;

    bool initialSyncDone = false;
    uint32_t syncFailureCount = 0;
    static uint32_t nextRetryTime = 0;  // For retry scheduling
    static uint32_t loopCount = 0;

    LOG_INFO(TAG, "Entering main event loop");

    while (true) {
        loopCount++;

        // WORKAROUND: Use short delay instead of blocking event wait
        // Bug discovered: blocking calls (xEventGroupWaitBits, ulTaskNotifyTake)
        // with timeouts >= ~5000ms hang on the second iteration in this task.
        // Short vTaskDelay works reliably, so we use that + non-blocking event check.
        // Skip delay on first iteration to check network immediately.
        static bool firstIteration = true;
        if (!firstIteration) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        firstIteration = false;

        // Non-blocking check for events
        EventBits_t events = xEventGroupWaitBits(
            eventGroup,
            ALL_EVENTS,
            pdTRUE,   // Clear bits on exit
            pdFALSE,  // Wait for any bit
            0         // Don't block - just check
        );

        // Feed watchdog
        (void)SRP::getTaskManager().feedWatchdog();

        // Log only when events occur (reduces spam)
        if (events != 0) {
            LOG_DEBUG(TAG, "Events: 0x%lx", (unsigned long)events);
        }
        
        // Check for scheduled retry
        if (nextRetryTime > 0 && millis() >= nextRetryTime) {
            nextRetryTime = 0;
            events |= NTP_EVENT_SYNC_NOW;
        }
        
        // Check system NETWORK_READY event bit (set when Ethernet connects)
        EventBits_t systemBits = SRP::getGeneralSystemEventBits();
        if (!initialSyncDone && (systemBits & SystemEvents::GeneralSystem::NETWORK_READY)) {
            LOG_DEBUG(TAG, "Network available - attempting initial sync");
            events |= NTP_EVENT_NETWORK_READY | NTP_EVENT_SYNC_NOW;
        }
        
        // Handle network ready event
        if (events & NTP_EVENT_NETWORK_READY) {
            if (!initialSyncDone) {
                LOG_DEBUG(TAG, "Network ready - starting sync operations");
                // Start auto-sync timer after network is ready
                xTimerStart(autoSyncTimer, pdMS_TO_TICKS(100));
            }
        }
        
        // Handle sync request
        if (events & NTP_EVENT_SYNC_NOW) {
            if (xSemaphoreTake(ntpMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                bool success = performNTPSync();

                // Feed TaskManager watchdog after NTP sync (which can take up to 5 seconds)
                (void)SRP::getTaskManager().feedWatchdog();

                if (success) {
                    initialSyncDone = true;
                    syncFailureCount = 0;
                    rtcFallbackUsed = false;  // Reset RTC fallback flag on successful NTP sync
                } else {
                    syncFailureCount++;

                    // Try RTC fallback after threshold failures (only once per outage)
                    if (syncFailureCount >= RTC_FALLBACK_THRESHOLD && !rtcFallbackUsed && !initialSyncDone) {
                        LOG_WARN(TAG, "NTP failed %lu times - attempting RTC fallback", syncFailureCount);
                        if (tryRTCFallback()) {
                            rtcFallbackUsed = true;
                            initialSyncDone = true;  // Mark as synced (from RTC)
                            LOG_INFO(TAG, "Using RTC time until NTP recovers");
                        }
                        // Feed TaskManager watchdog after RTC fallback attempt
                        (void)SRP::getTaskManager().feedWatchdog();
                    }

                    // Schedule retry based on failure count
                    uint32_t retryDelay = NTPConstants::RETRY_INTERVAL_MS * (syncFailureCount > 5 ? 5 : syncFailureCount);
                    LOG_INFO(TAG, "Scheduling NTP retry in %u seconds", retryDelay / 1000);

                    // For long delays, we need a mechanism to keep feeding the watchdog
                    // Instead of a single long timer, we'll set a flag to retry later
                    nextRetryTime = millis() + retryDelay;
                }

                xSemaphoreGive(ntpMutex);
            }
        }
        
        // Handle status logging
        if (events & NTP_EVENT_LOG_STATUS) {
            logNTPStatus();
        }
        
        // Handle configuration update
        if (events & NTP_EVENT_UPDATE_CONFIG) {
            LOG_INFO(TAG, "Configuration update requested");
            // Could reload NTP servers, timezone, etc. here
        }

    }
    
    // Cleanup (should never reach here)
    if (autoSyncTimer) {
        xTimerDelete(autoSyncTimer, 0);
    }
    if (statusTimer) {
        xTimerDelete(statusTimer, 0);
    }
    vTaskDelete(NULL);
}

// FreeRTOS task wrapper for compatibility
void NTPTask(void* parameter) {
    NTPTaskEventDriven(parameter);
}

// Public API functions remain the same
void setNTPRTCCallback(NTPSyncCallback callback) {
    if (ntpMutex && xSemaphoreTake(ntpMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        rtcUpdateCallback = callback;
        xSemaphoreGive(ntpMutex);
    }
}

bool forceNTPSync() {
    if (!ntpInitialized) {
        LOG_WARN(TAG, "NTP not initialized");
        return false;
    }
    
    // Trigger sync event
    ntpEvents.setEvent(NTP_EVENT_SYNC_NOW);
    return true;
}

bool isNTPSynced() {
    return ntpInitialized && lastSyncTime > 0;
}

time_t getLastNTPSyncTime() {
    return lastSyncTime;
}