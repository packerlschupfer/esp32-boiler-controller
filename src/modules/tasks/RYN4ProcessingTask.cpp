// src/modules/tasks/RYN4ProcessingTask.cpp
// Handles scheduled Modbus operations for RYN4 relay module via ModbusCoordinator
#include "modules/tasks/RYN4ProcessingTask.h"
#include "RYN4.h"
#include "LoggingMacros.h"
#include "core/SystemResourceProvider.h"
#include "core/ModbusCoordinator.h"
#include "config/SystemConstants.h"
#include "shared/RelayState.h"
#include "shared/SharedRelayReadings.h"
#include "shared/RelayBindings.h"
#include "events/SystemEventsGenerated.h"
#include <TaskManager.h>
#include <IDeviceInstance.h>
#include <climits>  // For ULONG_MAX
#include <array>

static const char* TAG = "RYN4Processing";

// Task handle for coordinator registration
static TaskHandle_t ryn4ProcessingTaskHandle = nullptr;

// DELAY watchdog: Track SET tick count
static uint32_t g_setTickCounter = 0;

// Helper: Send DELAY commands for state changes
static void sendDelayCommands(RYN4* ryn4, uint8_t desired) {
    std::array<RYN4::RelayCommandSpec, 8> commands;

    for (int i = 0; i < 8; i++) {
        bool shouldBeOn = (desired >> i) & 0x01;
        if (shouldBeOn) {
            // Relay ON: Use DELAY watchdog (auto-OFF in 10s if not renewed)
            uint8_t delaySeconds = SystemConstants::Relay::DELAY_WATCHDOG_SECONDS;
            commands[i] = {ryn4::RelayAction::DELAY, delaySeconds};
            // Track DELAY - don't verify until timer would expire
            g_relayState.setDelayCommand(i, delaySeconds);
        } else {
            // Relay OFF: Use DELAY 0 to cancel any active timer
            commands[i] = {ryn4::RelayAction::DELAY, 0};

            // Track DELAY 0 - verify immediately on next READ tick
            // Hardware responds <100ms, READ tick at 1000ms provides sufficient time
            g_relayState.setDelayCommand(i, 0);
        }
    }

    ryn4::RelayErrorCode result = ryn4->setMultipleRelayCommands(commands);

    if (result == ryn4::RelayErrorCode::SUCCESS) {
        // Update 'sent' state - this is what we verify against in READ tick
        g_relayState.sent.store(desired, std::memory_order_release);
        LOG_INFO(TAG, "Relay DELAY commands sent: 0x%02X", desired);
    } else {
        LOG_ERROR(TAG, "Failed to send DELAY commands: %d", static_cast<int>(result));
        g_relayState.pendingWrite.store(true, std::memory_order_release);
    }
}

// Helper: Renew DELAY for contiguous ON relays (minimal Modbus traffic)
static void sendCompactDelayRenewal(RYN4* ryn4, uint8_t desired) {
    // Find contiguous blocks of ON relays and renew them efficiently
    // E.g., if R1,R2,R5 are ON: send R1-R2 (4 bytes), then R5 (2 bytes)
    // Much more efficient than sending all 8 relays (16 bytes)

    uint8_t start = 0xFF;
    uint8_t count = 0;

    for (int i = 0; i <= 8; i++) {  // Loop to 8 to flush last block
        bool isOn = (i < 8) && ((desired >> i) & 0x01);

        if (isOn && start == 0xFF) {
            // Start of contiguous block
            start = i;
            count = 1;
        } else if (isOn && start != 0xFF) {
            // Continue block
            count++;
        } else if (!isOn && start != 0xFF) {
            // End of block - send renewal for start..start+count-1
            std::vector<uint16_t> data(count);
            for (int j = 0; j < count; j++) {
                data[j] = 0x0600 | SystemConstants::Relay::DELAY_WATCHDOG_SECONDS;  // DELAY 10s
            }

            auto result = ryn4->writeMultipleRegisters(start, data);
            if (result.isOk()) {
                // Update tracking
                for (int j = start; j < start + count; j++) {
                    g_relayState.setDelayCommand(j, SystemConstants::Relay::DELAY_WATCHDOG_SECONDS);
                }
                LOG_DEBUG(TAG, "DELAY renewed: R%d-%d (%d bytes)", start + 1, start + count, count * 2);
            } else {
                LOG_ERROR(TAG, "Failed DELAY renewal: R%d-%d", start + 1, start + count);
            }

            start = 0xFF;
            count = 0;
        }
    }
}

// Handle SET tick - DELAY watchdog with staggered renewal
static void handleSetTick(RYN4* ryn4) {
    g_setTickCounter++;

    uint8_t desired = g_relayState.desired.load(std::memory_order_acquire);
    bool hasPendingWrite = g_relayState.pendingWrite.load(std::memory_order_acquire);

    // Prioritize state changes over renewal
    if (hasPendingWrite) {
        // Clear pending flag atomically
        g_relayState.pendingWrite.store(false, std::memory_order_release);

        LOG_INFO(TAG, "SET tick - state change: 0x%02X", desired);
        sendDelayCommands(ryn4, desired);

    } else if (desired != 0) {
        // Compact renewal: Send only contiguous ON relay blocks
        // Example: R1,R2,R5 ON → 2 transactions (R1-R2: 4 bytes, R5: 2 bytes)
        // Much more efficient than full 8-relay batch (16 bytes)
        // Renewal every other SET tick (5s interval per batch)

        bool isRenewalTick = (g_setTickCounter % 2) == 0;
        if (isRenewalTick) {
            LOG_DEBUG(TAG, "SET tick - compact DELAY renewal");
            sendCompactDelayRenewal(ryn4, desired);
        } else {
            LOG_DEBUG(TAG, "SET tick - no renewal (odd tick)");
        }

    } else {
        // All relays OFF - no renewal needed
        LOG_DEBUG(TAG, "SET tick - all relays OFF");
    }

    // Clean up expired DELAY timers
    uint8_t delayMask = g_relayState.delayMask.load(std::memory_order_acquire);
    if (delayMask != 0) {
        for (int i = 0; i < 8; i++) {
            if (delayMask & (1 << i)) {
                if (!g_relayState.isDelayActive(i)) {
                    g_relayState.clearDelay(i);
                    LOG_DEBUG(TAG, "DELAY expired for relay %d", i + 1);
                }
            }
        }
    }
}

// Handle READ tick - verify relay states match desired
static void handleReadTick(RYN4* ryn4) {
    LOG_DEBUG(TAG, "READ tick - verifying relay states");

    // Read actual relay states as bitmap
    auto bitmapResult = ryn4->readBitmapStatus(true);  // true = update internal cache

    if (bitmapResult.isError()) {
        LOG_ERROR(TAG, "Failed to read relay status bitmap");
        return;
    }

    uint16_t actualBitmap = bitmapResult.value();
    uint8_t actual = static_cast<uint8_t>(actualBitmap & 0xFF);  // Only lower 8 bits for 8 relays
    uint8_t sent = g_relayState.sent.load(std::memory_order_acquire);  // Verify against what we sent

    // Update actual state
    g_relayState.actual.store(actual, std::memory_order_release);

    // Update SharedRelayReadings for MQTT publishing
    if (SRP::takeRelayReadingsMutex(pdMS_TO_TICKS(100))) {
        for (int i = 0; i < 8; i++) {
            bool* statePtr = RelayBindings::getStatePtr(i);
            if (statePtr) {
                *statePtr = (actual >> i) & 0x01;
            }
        }
        SRP::giveRelayReadingsMutex();
        SRP::setRelayEventBits(SystemEvents::RelayControl::DATA_AVAILABLE);
    }

    // Set relay status bits
    EventGroupHandle_t relayStatusEventGroup = SRP::getRelayStatusEventGroup();
    if (relayStatusEventGroup) {
        xEventGroupSetBits(relayStatusEventGroup,
            SystemEvents::RelayStatus::SYNCHRONIZED | SystemEvents::RelayStatus::COMM_OK);
    }

    // Check for mismatch - but skip DELAY relays (countdown in progress)
    // Compare against 'sent' (what we commanded), not 'desired' (what app wants)
    if (actual != sent) {
        // Check if mismatch is due to active DELAY timers
        uint8_t mismatchMask = actual ^ sent;  // XOR to find differing bits
        uint8_t delayMask = g_relayState.delayMask.load(std::memory_order_acquire);
        uint8_t realMismatch = mismatchMask & ~delayMask;  // Exclude DELAY relays

        if (realMismatch == 0) {
            // All mismatches are from active DELAY commands - expected!
            LOG_DEBUG(TAG, "Relay verification deferred (DELAY active): Sent: 0x%02X, Actual: 0x%02X, Delay mask: 0x%02X",
                      sent, actual, delayMask);

            // Don't increment mismatch counter or retry for DELAY relays
            return;
        }

        // We have real mismatches (non-DELAY relays)
        uint8_t mismatches = g_relayState.consecutiveMismatches.fetch_add(1, std::memory_order_acq_rel) + 1;

        if (mismatches == 1) {
            // First mismatch - likely timing issue
            LOG_DEBUG(TAG, "Relay verification pending (attempt 1/2): Sent: 0x%02X, Actual: 0x%02X",
                      sent, actual);
        } else {
            // Persistent mismatch - real problem!
            LOG_ERROR(TAG, "Relay verification FAILED after %d attempts! Sent: 0x%02X, Actual: 0x%02X",
                      mismatches, sent, actual);

            // Log individual mismatches (excluding DELAY relays)
            for (int i = 0; i < 8; i++) {
                if (realMismatch & (1 << i)) {
                    bool sentBit = (sent >> i) & 0x01;
                    bool actualBit = (actual >> i) & 0x01;
                    LOG_ERROR(TAG, "  Relay %d: sent=%s, actual=%s",
                              i + 1, sentBit ? "ON" : "OFF", actualBit ? "ON" : "OFF");
                }
            }

            // Set communication error event bit
            EventGroupHandle_t relayStatusEventGroup = SRP::getRelayStatusEventGroup();
            if (relayStatusEventGroup) {
                xEventGroupSetBits(relayStatusEventGroup, SystemEvents::RelayStatus::COMM_ERROR);
            }
        }

        // Queue retry
        g_relayState.pendingWrite.store(true, std::memory_order_release);

    } else {
        // States match - reset counter
        uint8_t previousMismatches = g_relayState.consecutiveMismatches.exchange(0, std::memory_order_acq_rel);

        if (previousMismatches > 0) {
            LOG_INFO(TAG, "Relay verification SUCCESS after %d attempts: 0x%02X",
                     previousMismatches + 1, actual);
        } else {
            LOG_DEBUG(TAG, "Relay states verified: 0x%02X", actual);
        }
    }
}

void RYN4ProcessingTask(void* parameter) {
    auto* ryn4 = static_cast<RYN4*>(parameter);

    if (!ryn4) {
        LOG_ERROR(TAG, "Started with null RYN4 instance");
        vTaskDelete(NULL);
        return;
    }

    ryn4ProcessingTaskHandle = xTaskGetCurrentTaskHandle();

    LOG_INFO(TAG, "Started C%d Stk:%d", xPortGetCoreID(), uxTaskGetStackHighWaterMark(NULL) * 4);

    // Register with watchdog
    TaskManager::WatchdogConfig wdtConfig = TaskManager::WatchdogConfig::enabled(
        false,  // not critical
        SystemConstants::System::WDT_SENSOR_PROCESSING_MS
    );

    if (!SRP::getTaskManager().registerCurrentTaskWithWatchdog("RYN4Processing", wdtConfig)) {
        LOG_WARN(TAG, "Failed to register with watchdog");
    } else {
        LOG_INFO(TAG, "WDT OK %lums", SystemConstants::System::WDT_SENSOR_PROCESSING_MS);
    }

    // Wait for device initialization to complete using event bits
    LOG_INFO(TAG, "Waiting for device initialization...");

    EventGroupHandle_t deviceReadyGroup = ryn4->getExternalEventGroup();
    EventBits_t readyBit = ryn4->getReadyBit();
    EventBits_t errorBit = ryn4->getErrorBit();

    if (deviceReadyGroup != nullptr && (readyBit || errorBit)) {
        EventBits_t waitBits = readyBit | errorBit;
        EventBits_t bits = xEventGroupWaitBits(
            deviceReadyGroup,
            waitBits,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(30000)
        );

        if (bits & readyBit) {
            LOG_INFO(TAG, "Device initialization complete - ready");
        } else if (bits & errorBit) {
            LOG_WARN(TAG, "Device initialization failed - will wait for background retry");
        } else {
            LOG_WARN(TAG, "Timeout waiting for device initialization");
        }
    } else {
        LOG_WARN(TAG, "No event group configured - using fallback delay");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    // Register with ModbusCoordinator for both SET and READ operations
    auto& coordinator = ModbusCoordinator::getInstance();

    if (!coordinator.registerSensor(ModbusCoordinator::SensorType::RYN4_SET, ryn4ProcessingTaskHandle)) {
        LOG_ERROR(TAG, "Failed to register for RYN4_SET");
    } else {
        LOG_INFO(TAG, "Registered for RYN4_SET notifications");
    }

    if (!coordinator.registerSensor(ModbusCoordinator::SensorType::RYN4_READ, ryn4ProcessingTaskHandle)) {
        LOG_ERROR(TAG, "Failed to register for RYN4_READ");
    } else {
        LOG_INFO(TAG, "Registered for RYN4_READ notifications");
    }

    // Let RYN4 hardware settle after init (DELAY 0 × 8 needs processing time)
    vTaskDelay(pdMS_TO_TICKS(100));

    // Read initial relay states
    LOG_INFO(TAG, "Reading initial relay states...");
    auto bitmapResult = ryn4->readBitmapStatus(true);
    if (bitmapResult.isOk()) {
        uint8_t initial = static_cast<uint8_t>(bitmapResult.value() & 0xFF);
        g_relayState.actual.store(initial, std::memory_order_release);
        g_relayState.desired.store(initial, std::memory_order_release);  // Initialize desired to match actual
        LOG_INFO(TAG, "Initial relay states: 0x%02X", initial);
    } else {
        LOG_WARN(TAG, "Failed to read initial relay states");
    }

    LOG_INFO(TAG, "Entering main processing loop - waiting for coordinator notifications");

    // Main processing loop
    constexpr TickType_t WAIT_TIMEOUT = pdMS_TO_TICKS(SystemConstants::Timing::TASK_NOTIFICATION_TIMEOUT_MS);  // 3s timeout for watchdog

    while (true) {
        // Wait for coordinator notification with SensorType value
        uint32_t notificationValue = 0;
        if (xTaskNotifyWait(0, ULONG_MAX, &notificationValue, WAIT_TIMEOUT) == pdTRUE) {
            auto sensorType = static_cast<ModbusCoordinator::SensorType>(notificationValue);

            if (sensorType == ModbusCoordinator::SensorType::RYN4_SET) {
                handleSetTick(ryn4);
            } else if (sensorType == ModbusCoordinator::SensorType::RYN4_READ) {
                handleReadTick(ryn4);
            } else {
                LOG_WARN(TAG, "Unexpected notification value: %lu", notificationValue);
            }
        }

        // Feed watchdog
        (void)SRP::getTaskManager().feedWatchdog();
    }

    // Cleanup (should never reach here)
    coordinator.unregisterSensor(ModbusCoordinator::SensorType::RYN4_SET);
    coordinator.unregisterSensor(ModbusCoordinator::SensorType::RYN4_READ);
    vTaskDelete(NULL);
}
