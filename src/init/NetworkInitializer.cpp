// src/init/NetworkInitializer.cpp
#include "NetworkInitializer.h"

#include <Arduino.h>
#include "LoggingMacros.h"
#include "config/ProjectConfig.h"
#include "EthernetManager.h"
#include "core/SystemResourceProvider.h"
#include "events/SystemEventsGenerated.h"


static const char* TAG = "NetworkInitializer";

Result<void> NetworkInitializer::initializeAsync() {
    LOG_INFO(TAG, "Starting network initialization (async)...");

    unsigned long ethStartTime = millis();

    // Configure Ethernet using fluent builder
    EthernetConfig ethConfig;
    ethConfig.withHostname(DEVICE_HOSTNAME)
             .withPHYAddress(ETH_PHY_ADDR)
             .withMDCPin(ETH_PHY_MDC_PIN)
             .withMDIOPin(ETH_PHY_MDIO_PIN)
             .withPowerPin(ETH_PHY_POWER_PIN)
             .withClockMode(ETH_CLOCK_MODE);

#ifdef USE_STATIC_IP
    ethConfig.withStaticIP(
        IPAddress(ETH_STATIC_IP),
        IPAddress(ETH_GATEWAY),
        IPAddress(ETH_SUBNET),
        IPAddress(ETH_DNS1),
        IPAddress(ETH_DNS2)
    );
    LOG_INFO(TAG, "Using static IP: %d.%d.%d.%d", ETH_STATIC_IP);
#else
    LOG_INFO(TAG, "Using DHCP");
#endif

    if (!EthernetManager::initializeAsync(ethConfig)) {
        return Result<void>(SystemError::NETWORK_INIT_FAILED, "Failed to start Ethernet");
    }

    LOG_INFO(TAG, "Ethernet PHY initialization started (async) in %lu ms", millis() - ethStartTime);
    LOG_INFO(TAG, "Network will connect in background...");

    // Create network monitor task (lightweight - just monitors connection)
    xTaskCreate(
        networkMonitorTask,
        "NetworkMonitor",
        2048,  // Minimal stack - only monitors connection
        nullptr,
        1,  // Low priority
        nullptr
    );

    return Result<void>();  // Always succeed - network is non-critical
}

Result<void> NetworkInitializer::initializeBlocking() {
    LOG_INFO(TAG, "Initializing network (blocking)...");

#ifdef ETH_MAC_ADDRESS
    uint8_t mac[] = ETH_MAC_ADDRESS;
    EthernetManager::setMacAddress(mac);
    LOG_INFO(TAG, "Using custom MAC address");
#endif

    unsigned long ethStartTime = millis();

    LOG_INFO(TAG, "Starting Ethernet PHY early initialization");
    EthernetManager::earlyInit();

    delay(10);

    if (!EthernetManager::initializeAsync(DEVICE_HOSTNAME, ETH_PHY_ADDR, ETH_PHY_MDC_PIN,
                                          ETH_PHY_MDIO_PIN, ETH_PHY_POWER_PIN, ETH_CLOCK_MODE)) {
        return Result<void>(SystemError::NETWORK_INIT_FAILED, "Failed to start Ethernet");
    }

    LOG_INFO(TAG, "Ethernet initialization started in %lu ms", millis() - ethStartTime);

    // Wait for connection
    if (!EthernetManager::waitForConnection(ETH_CONNECTION_TIMEOUT_MS)) {
        return Result<void>(SystemError::NETWORK_TIMEOUT, "Ethernet connection timeout");
    }

    LOG_INFO(TAG, "Network initialized successfully");
    EthernetManager::logEthernetStatus();

    // Set network ready bit for other tasks
    SRP::setGeneralSystemEventBits(SystemEvents::GeneralSystem::NETWORK_READY);

    return Result<void>();
}

void NetworkInitializer::networkMonitorTask(void* param) {
    (void)param;

    // F39: keep checking until the Ethernet link comes up, rather than giving up
    // after ETH_CONNECTION_TIMEOUT_MS and self-deleting. After a power outage the
    // ESP32 boots in ~5s while the switch can take 1-2 min to bring the port up;
    // the old one-shot 15s window meant GeneralSystem::NETWORK_READY was never
    // set, so NTP, Syslog and OTA (all gated on that bit) stayed dead for the
    // entire uptime even though MQTT (which polls isConnected directly) recovered.
    // waitForConnection() blocks up to CHECK_INTERVAL_MS, so the loop self-paces.
    const uint32_t CHECK_INTERVAL_MS = 5000;
    const uint32_t start = millis();
    bool loggedStillDown = false;

    while (true) {
        if (EthernetManager::waitForConnection(CHECK_INTERVAL_MS)) {
            LOG_INFO("NetworkMonitor", "Network connected (after %lu ms)",
                     (unsigned long)(millis() - start));
            EthernetManager::logEthernetStatus();
            SRP::setGeneralSystemEventBits(SystemEvents::GeneralSystem::NETWORK_READY);
            break;
        }
        if (!loggedStillDown && (millis() - start) >= ETH_CONNECTION_TIMEOUT_MS) {
            LOG_WARN("NetworkMonitor",
                     "Network still down after %lu ms - continuing to wait for link-up",
                     (unsigned long)ETH_CONNECTION_TIMEOUT_MS);
            loggedStillDown = true;
        }

        // review-fix: guaranteed yield floor. This loop relies on
        // waitForConnection() blocking for the interval, but its early-return
        // paths (phy not started, null event group) could return immediately -
        // without this delay the prio-1 loop would spin tightly, starve the IDLE
        // task and trip the task watchdog. A short unconditional delay lets IDLE
        // run (which feeds the TWDT) and barely affects the ~5s poll cadence.
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    vTaskDelete(NULL);  // Delete this task once the link is up
}
