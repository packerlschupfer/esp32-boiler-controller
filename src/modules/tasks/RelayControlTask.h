// src/modules/tasks/RelayControlTask.h
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/event_groups.h>
#include <array>   // For std::array
#include <atomic>  // Round 20 Issue #6: For atomic relayStatesKnown
#include <RYN4.h>
#include "config/ProjectConfig.h"
// QueueManager.h removed - no longer using command queue

// Forward declarations
class RYN4;

class RelayControlTask {
public:
    
    // Task control
    static bool init(RYN4* device);
    static bool start();
    static void stop();
    static bool isRunning();
    static TaskHandle_t getTaskHandle();
    
    // Relay control methods
    static bool setRelayState(uint8_t relayIndex, bool state);
    static bool setAllRelays(bool state);
    static bool setMultipleRelays(const std::array<bool, 8>& states);  // Using array for type safety
    static bool toggleRelay(uint8_t relayIndex);
    static bool toggleAllRelays();
    
    // Status methods
    static void getStatistics(uint32_t& processed, uint32_t& failed);
    
private:
    // Task function
    static void taskFunction(void* pvParameters);
    
    // Direct relay processing methods (no queue)
    static bool processSingleRelay(uint8_t relayIndex, bool state);
    static bool processToggleRelay(uint8_t relayIndex);
    static bool processSetAllRelays(bool state);
    static bool processSetMultipleRelays(const std::array<bool, 8>& states);  // Using array for type safety
    static bool processToggleAllRelays();
    
    // Rate limiting methods (from version 2)
    static bool checkRateLimit(uint8_t relayIndex);
    static void updateRateLimitCounters();
    
    // System state monitoring for relay coordination
    static void monitorSystemState();
    static void waitForRelayRequests();
    static void processRelayRequests();
    
    // Static members
    static RYN4* ryn4Device;
    static TaskHandle_t taskHandle;
    // Command queue removed - using direct RYN4 calls
    static SemaphoreHandle_t taskMutex;
    static bool initialized;
    static bool running;
    static uint32_t commandsProcessed;
    static uint32_t commandsFailed;
    static TickType_t lastCommandTime;
    
    // Rate limiting members (from version 2)
    static uint32_t toggleCount[8];
    static TickType_t toggleTimestamps[8];
    static TickType_t rateWindowStart;
    
    // State tracking for avoiding redundant commands
    // Round 20 Issue #6: Protected by relayStateMutex_
    static bool currentRelayStates[8];
    static std::atomic<bool> relayStatesKnown;  // Made atomic for fast check without mutex
    static SemaphoreHandle_t relayStateMutex_;  // Protects currentRelayStates array

    // Consecutive failure tracking for escalation to failsafe
    // FMEA Round 6: Reduced from 5 to 3 for faster failsafe response
    static uint8_t consecutiveFailures[8];
    static constexpr uint8_t MAX_CONSECUTIVE_FAILURES = 3;

    // Pump motor protection - 30s minimum between state changes for relays 1 and 2
    // This prevents rapid on/off cycling that can damage pump motors
    static TickType_t pumpLastStateChangeTime[2];  // For relay 1 (heating) and 2 (water)
    static bool checkPumpProtection(uint8_t relayIndex, bool desiredState);
    static uint32_t getPumpProtectionTimeRemaining(uint8_t relayIndex);

    // Helper to update SharedRelayReadings immediately
    static void updateSharedRelayReadings(uint8_t relayIndex, bool state);

    // Escalate to failsafe if relay failures persist
    static void checkRelayHealthAndEscalate(uint8_t relayIndex, bool success);
};