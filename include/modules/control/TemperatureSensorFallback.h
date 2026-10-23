#ifndef TEMPERATURE_SENSOR_FALLBACK_H
#define TEMPERATURE_SENSOR_FALLBACK_H

#include <cstdint>
#include "shared/Temperature.h"

/**
 * @brief Temperature sensor fallback and validation system
 * 
 * Provides ultra-simple, fail-safe sensor validation with context-aware
 * requirements based on active operation mode. No partial operation allowed.
 */
class TemperatureSensorFallback {
public:
    enum class FallbackMode {
        STARTUP,          // Initial startup period - waiting for sensors
        NORMAL,           // All required sensors working
        SHUTDOWN          // Any required sensor missing - must stop
    };
    
    enum class OperationMode {
        NONE,             // No operation active
        SPACE_HEATING,    // Space heating only
        WATER_HEATING,    // Water heating only
        BOTH              // Both heating modes active
    };
    
    struct SensorStatus {
        bool boilerOutputValid = false;
        bool boilerReturnValid = false;
        bool waterTempValid = false;
        bool roomTempValid = false;
        bool outsideTempValid = false;
        
        FallbackMode currentMode = FallbackMode::STARTUP;
        OperationMode currentOperationMode = OperationMode::NONE;
        
        // Track which specific sensors are missing for clear error reporting
        bool missingBoilerOutput = false;
        bool missingBoilerReturn = false;
        bool missingWaterTemp = false;
        bool missingRoomTemp = false;
    };
    
    // Initialize the fallback system
    static void initialize();

    // Cleanup for partial init recovery
    static void cleanup();

    // Set current operation mode (called by control tasks)
    static void setOperationMode(OperationMode mode);
    
    // Update sensor status and determine fallback mode
    static FallbackMode updateSensorStatus();
    
    // Get current fallback mode
    static FallbackMode getCurrentMode() { return currentStatus.currentMode; }
    
    // Get current operation mode
    static OperationMode getOperationMode() { return currentStatus.currentOperationMode; }
    
    // Get sensor status
    static const SensorStatus& getStatus() { return currentStatus; }
    
    // Check if operation should continue
    static bool canContinueOperation();
    
    // Get safe operating parameters for current mode
    static void getSafeOperatingParams(Temperature_t& maxTemp, float& maxPower, uint32_t& runTime);
    
    // Check if all required sensors are available for current operation mode
    static bool hasRequiredSensors();
    
private:
    static SensorStatus currentStatus;
    static uint32_t initializationTime;
    // Use SENSOR_TIMEOUT_MS from SystemConstants (30 seconds)
    // Startup period must be long enough for all Modbus sensors to complete their first read
    // ANDRTF3 needs ~200-500ms per read, and may wait for Modbus bus mutex
    // Reduced from 10s to 5s - sensors typically ready within 5s, 10s was overly conservative
    static constexpr uint32_t STARTUP_PERIOD_MS = 5000; // 5 seconds startup grace period
    static constexpr Temperature_t DEFAULT_OUTSIDE_TEMP = -50; // -5.0°C in tenths

    // Hysteresis counters to prevent mode chattering on flaky sensors
    static uint8_t consecutiveValidCount;    // Count of consecutive valid sensor checks
    static uint8_t consecutiveInvalidCount;  // Count of consecutive invalid sensor checks

    // Hysteresis thresholds (different for entering vs leaving NORMAL mode). Deliberately
    // asymmetric: slow to shut down, immediate to recover.
    //
    // The counters tick once per evaluation, and evaluation is driven by BurnerControlTask's
    // TEMP_UPDATE_BITS - which is MB8ART channels ONLY (BOILER_RETURN, WATER_TANK and the
    // three error bits, set on every read). The ANDRTF3 room sensor raises
    // SensorUpdate::INSIDE / INSIDE_ERROR and neither is in that mask, so the room sensor's
    // own 5 s cycle does NOT tick this counter: a room failure is noticed on the next
    // MB8ART read like everything else. MB8ART_SENSOR_READ_INTERVAL_MS is 2500, so the tick
    // is 2.5 s and INVALID_COUNT_TO_SHUTDOWN = 3 is about 7.5 s of continuous failure.
    //
    // (Other callers can evaluate out of band - the idle refresh every 30 s, and each
    // setOperationMode() change alters WHICH sensors are required, which is part of why the
    // 2026-10-23 trace flapped within milliseconds. Those make the debounce stricter, not
    // looser, since any valid evaluation resets the invalid count.)
    //
    // It was 1, i.e. no hysteresis at all in the shutdown direction, under a comment saying
    // "sensors are stable and accurate in this installation". They are not: the room sensor
    // is a Modbus device on a shared bus and it glitches. On 2026-10-23 08:34:14 a single
    // failed check sent this straight to SHUTDOWN and stopped the burner mid-cycle, then
    // flapped SHUTDOWN -> NORMAL -> SHUTDOWN -> NORMAL within five seconds - exactly the
    // "mode oscillation from intermittent Modbus errors" the archived note below says the
    // original values existed to prevent. The burner restart cost 20 s of MIN_OFF_TIME and
    // one ignition. (Original Round 4 values: VALID=3, INVALID=2.)
    //
    // Why 3 (user decision, 2026-10-23): the room sensor's own driver already marks the
    // reading invalid only after 3 consecutive failed reads at 5 s each, so by the time
    // roomTempValid goes false the sensor has been failing ~15 s - the debounce here only
    // has to cover the dropout itself. The one on record was ~5 s of invalid room data
    // (08:34:14.9 to 08:34:19.9), i.e. 2 ticks, so 3 covers it with a tick to spare while
    // keeping the shared-counter delay below (~7.5 s) as short as it can be and still work.
    //
    // KNOWN CONSEQUENCE: this counter is shared by all required sensors, so a missing
    // boiler-OUTPUT reading - the one that leaves overheat protection blind and sets
    // Error::SENSOR_FAILURE - now also takes ~7.5 s longer to escalate. The boiler's own
    // aquastat and manual-reset STB are independent of the firmware and unaffected, and
    // SafetyInterlocks' 110 C check does not depend on this mode. Splitting the debounce
    // per sensor class (short for boiler output, long for room) is the better shape and is
    // not done here.
    static constexpr uint8_t VALID_COUNT_TO_ENTER_NORMAL = 1;   // Was 3 - one good check recovers
    static constexpr uint8_t INVALID_COUNT_TO_SHUTDOWN = 3;     // ~7.5 s at the 2.5 s tick
    
    // Validate sensor readings
    static bool validateSensorReading(Temperature_t temp, Temperature_t minValid = -500, Temperature_t maxValid = 1500);
    
    // Check sensor age
    static bool isSensorDataFresh(uint32_t lastUpdateTime, uint32_t maxAge = 30000);  // Default to SystemConstants::Timing::SENSOR_TIMEOUT_MS
    
    // updateSensorStatus() body; caller holds the fallback mutex (TemperatureSensorFallback.cpp)
    static FallbackMode updateSensorStatusLocked();

    // Get specific error message for missing sensors
    static const char* getMissingSensorMessage();
};

#endif // TEMPERATURE_SENSOR_FALLBACK_H