#ifndef SAFETY_INTERLOCKS_H
#define SAFETY_INTERLOCKS_H

#include <cstdint>
#include <string>
#include "shared/Temperature.h"
#include "config/SystemConstants.h"

/**
 * @brief Safety interlock system for burner control
 * 
 * Provides comprehensive safety checks before allowing burner operation.
 * All interlocks must pass before the burner can be activated.
 */
class SafetyInterlocks {
public:
    // Individual interlock status flags
    struct InterlockStatus {
        // Round 19 Issue #3: pumpRunning field REMOVED
        // Pumps are now started atomically with burner via BurnerSystemController batch command.
        // Relay verification happens at batch command level (setMultipleRelayStatesVerified).
        // Physical pump failure is detected via temperature sensors (no heat transfer = pump failed).
        bool temperatureValid = false;      // Temperature sensors are valid
        bool temperatureInRange = false;    // Temperatures within safe limits
        bool noEmergencyStop = true;        // No emergency stop active
        bool communicationOk = false;       // Communication with sensors/relays OK
        bool waterFlowDetected = false;     // Water flow is detected (if available)
        bool noSystemErrors = true;         // No critical system errors
        bool minimumSensorsValid = false;   // Minimum required sensors are valid
        bool pressureInRange = true;        // System pressure within safe limits (default true for compatibility)

        // Timestamp of last check
        uint32_t lastCheckTime = 0;

        // Overall status
        bool allInterlocksPassed() const {
            return temperatureValid &&
                   temperatureInRange &&
                   noEmergencyStop &&
                   communicationOk &&
                   noSystemErrors &&
                   minimumSensorsValid &&
                   pressureInRange;
        }
        
        // Get human-readable status (returns pointer to static buffer)
        const char* getFailureReason() const;
    };
    
    // Water flow verification (uses temperature differential as proxy - no flow sensor)
    static bool verifyWaterFlow(uint32_t timeoutMs = 3000);
    
    // Temperature safety checks
    static bool verifyTemperatureSensors(uint8_t minRequiredSensors = 2);
    static bool checkTemperatureLimits(Temperature_t maxAllowedTemp = 850);  // 85.0°C
    static bool checkThermalShock(Temperature_t maxDifferential = 350);  // 35.0°C
    
    // System status checks
    static bool checkEmergencyStop();
    static bool checkSystemErrors();
    static bool checkCommunicationStatus();
    
    // Comprehensive safety check
    static InterlockStatus performFullSafetyCheck(bool isWaterMode);
    
    // Continuous monitoring (to be called periodically during operation)
    static bool continuousSafetyMonitor();
    
    // Emergency response
    static void triggerEmergencyShutdown(const char* reason);
    
private:
    static InterlockStatus lastStatus;
    static uint32_t lastFullCheckTime;
    
    // Use safety timing constants from SystemConstants
    static constexpr uint32_t FULL_CHECK_INTERVAL_MS = SystemConstants::Safety::FULL_CHECK_INTERVAL_MS;
    static constexpr uint32_t CRITICAL_CHECK_INTERVAL_MS = SystemConstants::Safety::CRITICAL_CHECK_INTERVAL_MS;
};

#endif // SAFETY_INTERLOCKS_H