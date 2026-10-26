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
        BOTH,             // Both heating modes active
        // Frost protection only: nothing but the frost latch authorised this burn.
        //
        // Needs the boiler OUTPUT and nothing else, because a frost burn heats to the flow
        // FLOOR rather than to a room target - the room reading is not an input to it. Added
        // 2026-10-26 after the review found a start/stop loop: the start check ran under mode
        // NONE (boiler output only) and passed, then BurnerControlTask applied SPACE_HEATING,
        // which requires roomTempValid, so a frost burn with a dead room sensor had its
        // request dropped ~5 s in, the mode reverted to NONE, the fallback recovered, and it
        // started again - a 10-15 s loop with the fallback flapping NORMAL/SHUTDOWN. Frost is
        // the only path that starts heating while the room is not a measurement, which is why
        // nothing else exposed this.
        FROST_PROTECTION
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
    
    /**
     * @brief May the invalid-sample counter take another step?
     *
     * Pure, so the rate limit that the 2026-10-25 fix depends on is actually testable - the
     * surrounding updateSensorStatusLocked() needs FreeRTOS mutexes and shared readings, and
     * the native MockSharedSensorReadings is too stale to stand in for them.
     *
     * @param counting  false until the first invalid observation since the last valid one
     * @param nowMs     millis()
     * @param lastMs    when the counter last stepped
     * @param intervalMs the sensor sample period (MB8ART_SENSOR_READ_INTERVAL_MS)
     *
     * The subtraction is done in uint32_t so it stays correct across the 49.7-day millis()
     * rollover, where `lastMs + intervalMs` would wrap and stall the counter for 49 days -
     * i.e. it would stop escalating a dead sensor exactly once every 49 days.
     */
    // Public because it is part of this component's observable contract - the native tests
    // pin the ~5 s shutdown debounce against it rather than against a copied literal that
    // would silently drift if the threshold changed.
    static constexpr uint8_t INVALID_COUNT_TO_SHUTDOWN = 3;     // ~5 s: 3 rate-limited steps

    static bool shouldCountInvalid(bool counting, uint32_t nowMs, uint32_t lastMs,
                                   uint32_t intervalMs) {
        return !counting || (nowMs - lastMs) >= intervalMs;
    }


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
    // Steps with a missing BOILER OUTPUT specifically, so it can escalate on its own shorter
    // threshold. Reset whenever the boiler output is readable, even if another sensor is not.
    static uint8_t boilerOutputInvalidCount;

    // Hysteresis thresholds (different for entering vs leaving NORMAL mode). Deliberately
    // asymmetric: slow to shut down, immediate to recover.
    //
    // The INVALID counter is RATE-LIMITED to one step per MB8ART_SENSOR_READ_INTERVAL_MS
    // (2500). The first invalid observation counts at once, then at most one step per 2.5 s.
    //
    // MEASURED CONSEQUENCE, 2026-10-26 review: that is NOT "about 5 s", which is what this
    // comment used to claim. The MB8ART is read on coordinator ticks {2,5} of ten, so samples
    // arrive 1.5 s then 3.5 s apart - a 2.5 s average, never a 2.5 s period. A sample landing
    // 1.5 s after a counted one is therefore always discarded, and three steps span
    // **8.5-10 s** depending on which tick the failure starts near, not 5 s.
    //
    // LEFT AS IS, deliberately. The alternative is a predicate that counts sample ARRIVALS
    // with a time-based fallback for a dead sensor, and that is more machinery on a safety
    // path to recover 3.5 s. The exposure during those seconds is a burner running without a
    // trustworthy boiler-output reading - and that is independently covered: the boiler's own
    // aquastat and manual-reset STB are outside this firmware, SafetyInterlocks' 110 C check
    // does not depend on this mode, and the 5 s full safety check sees the missing output via
    // Error::SENSOR_FAILURE. So the number is documented rather than engineered.
    //
    // Uneven sampling also means "three samples" can never be one wall-clock figure. Quoting
    // the range is the honest form.
    //
    // THIS COMMENT USED TO CLAIM THE COUNTER TICKED ONCE PER MB8ART READ, AND IT DID NOT.
    // The old text argued that evaluation is driven by BurnerControlTask's TEMP_UPDATE_BITS,
    // that the room sensor's own 5 s cycle therefore could not tick it, and - the part that
    // was actually backwards - that extra out-of-band callers "make the debounce stricter,
    // not looser, since any valid evaluation resets the invalid count". That only considered
    // VALID evaluations. An extra caller evaluating while sensors are INVALID increments the
    // counter again, so more callers made the debounce LOOSER. And there are many:
    // updateSensorStatus() is reached from canContinueOperation(), called from
    // BurnerControlTask (3 sites), WheaterControlTask (4), CentralizedFailsafe and others,
    // and its STARTUP branch calls it twice in a single call. The threshold could therefore
    // be crossed inside one 2.5 s period, and the debounce actually obtained depended on how
    // many tasks happened to ask that cycle - which varies with mode. That silently undid
    // most of the 2026-10-23 change from 1 -> 3. Fixed and re-documented 2026-10-25 after
    // the review found it; the rate limit is in updateSensorStatusLocked().
    //
    // The rate limit is on ELAPSED TIME, not on a new sample arriving: a completely dead
    // MB8ART stops advancing lastBoilerTempUpdateTimestamp, so gating on sample arrival
    // would never escalate at all - the exact failure this layer exists to catch.
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
    // SPLIT PER SENSOR CLASS, 2026-10-26. This counter used to be shared by all required
    // sensors, so a missing boiler-OUTPUT reading - the one that leaves overheat protection
    // blind and sets Error::SENSOR_FAILURE - waited as long as a room dropout. The comment
    // here called the split "the better shape" and left it undone; the boiler output is the
    // most important reading in the system, so it now has its own shorter count below.
    static constexpr uint8_t VALID_COUNT_TO_ENTER_NORMAL = 1;   // Was 3 - one good check recovers

public:
    /**
     * @brief Steps of a MISSING BOILER OUTPUT before SHUTDOWN - shorter than the shared count.
     *
     * 2 instead of 3. On the real MB8ART schedule (ticks {2,5} of ten, so samples 1.5 s / 3.5 s
     * apart) that is 3.5-5 s rather than 8.5-10 s, while still rejecting a single bad read -
     * which is what the 2026-10-23 change from 1 to 3 was for, after one failed room check
     * stopped a burn mid-cycle.
     *
     * Why this reading and not the others: without it the overheat protection in this layer is
     * blind, and it is the one that sets Error::SENSOR_FAILURE. A missing room, tank or return
     * reading is degraded operation - the mode task stops the burner normally and
     * BurnerControlTask's 10 s SensorFailureConfirm handles a demand that persists - so those
     * keep the longer count, which exists to ride out the ANDRTF3's known dropouts.
     *
     * Still not instant, and the layers that are: the boiler's own aquastat and manual-reset
     * STB are outside this firmware entirely, and SafetyInterlocks' 110 C check does not depend
     * on this mode.
     */
    static constexpr uint8_t BOILER_OUTPUT_COUNT_TO_SHUTDOWN = 2;

private:
    
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