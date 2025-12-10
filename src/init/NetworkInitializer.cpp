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

    // Create a task to monitor network connection
    xTaskCreate(
        networkMonitorTask,
        "NetworkMonitor",
        2048,
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

    // Wait for network connection in background
    const uint32_t QUICK_CHECK_MS = 5000;  // Quick check first
    if (EthernetManager::waitForConnection(QUICK_CHECK_MS)) {
        LOG_INFO("NetworkMonitor", "Network connected successfully");
        EthernetManager::logEthernetStatus();
        SRP::setGeneralSystemEventBits(SystemEvents::GeneralSystem::NETWORK_READY);
    } else {
        // Continue waiting for full timeout
        if (EthernetManager::waitForConnection(ETH_CONNECTION_TIMEOUT_MS - QUICK_CHECK_MS)) {
            LOG_INFO("NetworkMonitor", "Network connected after extended wait");
            EthernetManager::logEthernetStatus();
            SRP::setGeneralSystemEventBits(SystemEvents::GeneralSystem::NETWORK_READY);
        } else {
            LOG_WARN("NetworkMonitor", "Network connection timeout - system will operate offline");
        }
    }
    vTaskDelete(NULL);  // Delete this task
}
