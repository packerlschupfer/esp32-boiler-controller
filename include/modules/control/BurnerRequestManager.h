#ifndef BURNER_REQUEST_MANAGER_H
#define BURNER_REQUEST_MANAGER_H

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/event_groups.h>
#include <cstdint>
#include "events/SystemEventsGenerated.h"
#include "shared/Temperature.h"

/**
 * @brief Thread-safe manager for burner request operations
 * 
 * Provides atomic read-modify-write operations for burner request bits
 * to prevent race conditions when multiple tasks update requests.
 */
class BurnerRequestManager {
public:
    // Request source identifiers
    enum class RequestSource {
        HEATING,
        WATER,
        MANUAL,
        EMERGENCY
    };
    
    // Initialize the manager
    static void initialize();

    // Cleanup for partial init recovery (deletes mutex if created)
    static void cleanup();
    
    // Thread-safe request operations
    static bool setHeatingRequest(Temperature_t targetTemp, bool highPower = false);
    static bool setWaterRequest(Temperature_t targetTemp, bool highPower = false);
    static bool clearRequest(RequestSource source);
    static bool clearAllRequests();
    
    // Atomic read of current requests
    static EventBits_t getCurrentRequests();
    
    // Get decoded values from current requests
    static Temperature_t getCurrentTargetTemp();
    static bool isHeatingRequested();
    static bool isWaterRequested();
    static bool isWaterPriority();
    static bool isHighPowerRequested();
    
    // Update temperature without changing other bits
    static bool updateTargetTemp(Temperature_t newTemp);

    // Emergency operations
    static void emergencyClearAll();

    // Request timeout/watchdog functions
    static uint32_t getLastHeatingRequestTime();
    static uint32_t getLastWaterRequestTime();
    static bool isHeatingRequestExpired(uint32_t maxAgeMs);
    static bool isWaterRequestExpired(uint32_t maxAgeMs);
    static bool checkAndClearExpiredRequests(uint32_t maxAgeMs);

    /**
     * @brief Atomic update of event bits (clear then set under mutex)
     * @param setBits Bits to set after clearing
     * @param clearBits Bits to clear before setting
     * @return true if update succeeded
     *
     * Use this to prevent race conditions in clear-then-set patterns.
     * Round 12 Issue #3: Event group clear-then-set race.
     */
    static bool atomicUpdateBits(EventBits_t setBits, EventBits_t clearBits);

private:
    static SemaphoreHandle_t requestMutex;
    static bool initialized;
    static uint32_t lastHeatingRequestTime;  // Timestamp of last heating request
    static uint32_t lastWaterRequestTime;    // Timestamp of last water request
    static constexpr uint32_t MUTEX_TIMEOUT_MS = 100;

    // Helper to get event group handle
    static EventGroupHandle_t getBurnerRequestEventGroup();
};

#endif // BURNER_REQUEST_MANAGER_H