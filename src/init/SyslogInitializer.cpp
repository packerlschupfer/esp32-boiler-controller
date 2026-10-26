// src/init/SyslogInitializer.cpp
#include "SyslogInitializer.h"

#include <Arduino.h>
#include "LoggingMacros.h"
#include "config/SystemSettingsStruct.h"
#include "core/SystemResourceProvider.h"
#include "events/SystemEventsGenerated.h"
#include <Syslog.h>

#if defined(USE_CUSTOM_LOGGER) && !defined(LOG_NO_CUSTOM_LOGGER)
#include <Logger.h>
#endif

static const char* TAG = "SyslogInit";

// Static syslog instance (persists for lifetime of application)
static Syslog* sSyslog = nullptr;

#if defined(USE_CUSTOM_LOGGER) && !defined(LOG_NO_CUSTOM_LOGGER)
// Syslog callback for Logger subscriber mechanism
// Runs on dedicated subscriber task pinned to Core 1 (same as Ethernet)
static void syslogCallback(esp_log_level_t level, const char* tag, const char* message) {
    if (auto* syslog = SRP::getSyslog()) {
        syslog->send(level, tag, message);
    }
}
#endif

Result<void> SyslogInitializer::initialize() {
    // Task creation is handled by TaskInitializer using TaskManager
    // This method is for future use if synchronous init is needed
    return Result<void>();
}

void SyslogInitializer::SyslogTask(void* param) {
    (void)param;

    // Wait for both NETWORK_READY and STORAGE_READY
    EventGroupHandle_t generalSystemEventGroup = SRP::getGeneralSystemEventGroup();
    if (!generalSystemEventGroup) {
        LOG_ERROR(TAG, "No event group - cannot initialize");
        vTaskDelete(NULL);
        return;
    }

    EventBits_t requiredBits = SystemEvents::GeneralSystem::NETWORK_READY |
                               SystemEvents::GeneralSystem::STORAGE_READY;

    EventBits_t bits = xEventGroupWaitBits(
        generalSystemEventGroup,
        requiredBits,
        pdFALSE,  // Don't clear on exit
        pdTRUE,   // Wait for ALL bits
        pdMS_TO_TICKS(30000)  // 30 second timeout
    );

    if ((bits & requiredBits) != requiredBits) {
        LOG_WARN(TAG, "Timeout waiting for network/storage - syslog not initialized");
        vTaskDelete(NULL);
        return;
    }

    // Check settings
    SystemSettings& settings = SRP::getSystemSettings();

#ifdef SYSLOG_FORCE_DISABLED
    // Recovery mode: force syslog disabled and save to NVS
    if (settings.syslogEnabled) {
        LOG_WARN(TAG, "SYSLOG_FORCE_DISABLED: Overriding syslogEnabled=true from NVS");
        settings.syslogEnabled = false;
        extern void PersistentStorageTask_RequestSave();
        PersistentStorageTask_RequestSave();
        LOG_WARN(TAG, "Syslog disabled and saved - remove SYSLOG_FORCE_DISABLED flag");
    }
    vTaskDelete(NULL);
    return;
#endif

    if (!settings.syslogEnabled) {
        LOG_INFO(TAG, "Syslog disabled in settings");
        vTaskDelete(NULL);
        return;
    }

    // Create syslog instance
    sSyslog = new Syslog("esp32-boiler", "boiler");
    if (!sSyslog) {
        LOG_ERROR(TAG, "Failed to allocate Syslog instance");
        vTaskDelete(NULL);
        return;
    }

    // Convert IP array to IPAddress
    IPAddress serverIP(
        settings.syslogServerIP[0],
        settings.syslogServerIP[1],
        settings.syslogServerIP[2],
        settings.syslogServerIP[3]
    );

    // Initialize syslog with settings
    if (!sSyslog->begin(serverIP, settings.syslogPort,
                       static_cast<Syslog::Facility>(settings.syslogFacility))) {
        LOG_ERROR(TAG, "Failed to initialize syslog");
        delete sSyslog;
        sSyslog = nullptr;
        vTaskDelete(NULL);
        return;
    }

    sSyslog->setMinLevel(static_cast<esp_log_level_t>(settings.syslogMinLevel));
    SRP::setSyslog(sSyslog);

    LOG_INFO(TAG, "Syslog initialized: %d.%d.%d.%d:%u facility=%u minLevel=%u",
             settings.syslogServerIP[0], settings.syslogServerIP[1],
             settings.syslogServerIP[2], settings.syslogServerIP[3],
             settings.syslogPort, settings.syslogFacility, settings.syslogMinLevel);

    // Register syslog callback with Logger's async subscriber mechanism
    // The subscriber task is pinned to Core 1 (same as Ethernet/LAN8720 PHY)
    // to avoid cross-core EthernetUDP access issues
    //
    // IMPORTANT: Start subscriber task BEFORE registering callback to ensure
    // the async queue exists. Otherwise there's a race condition where logs
    // between addLogSubscriber() and startSubscriberTask() use synchronous
    // fallback path, causing stack overflow in calling task.
#if defined(USE_CUSTOM_LOGGER) && !defined(LOG_NO_CUSTOM_LOGGER)
    // Start subscriber task first (creates the async queue)
    if (!SRP::getLogger().startSubscriberTask(1)) {
        LOG_ERROR(TAG, "Failed to start log subscriber task on Core 1");
    } else {
        LOG_INFO(TAG, "Log subscriber task started on Core 1");

        // Now safe to register subscriber - queue exists
        if (SRP::getLogger().addLogSubscriber(syslogCallback)) {
            LOG_INFO(TAG, "Syslog subscriber registered with Logger");
        } else {
            LOG_ERROR(TAG, "Failed to register syslog subscriber");
        }
    }
#endif

    vTaskDelete(NULL);
}
