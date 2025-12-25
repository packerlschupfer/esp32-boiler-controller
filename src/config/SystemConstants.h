// src/config/SystemConstants.h
#ifndef SYSTEM_CONSTANTS_H
#define SYSTEM_CONSTANTS_H

#include <cstdint>
#include "shared/Temperature.h"

namespace SystemConstants {
    
    // ===========================
    // Timing Constants
    // ===========================
    // Sensor and MQTT Publishing Strategy:
    // - MB8ART reads every 2.5s (critical boiler temperatures)
    // - ANDRTF3 reads every 5s (room temperature changes slowly)
    // - MQTT publishes sensor data every 10s (balanced update rate)
    // - MQTT publishes health data every 60s (system monitoring)
    namespace Timing {
        // Task delays and intervals
        constexpr uint32_t TASK_STARTUP_DELAY_MS = 100;
        constexpr uint32_t EMERGENCY_STOP_DELAY_MS = 50;
        
        // Burner timing
        constexpr uint32_t BURNER_IGNITION_DELAY_MS = 500;
        constexpr uint32_t BURNER_POWER_CHANGE_DELAY_MS = 500;
        constexpr uint32_t BURNER_SAFETY_CHECK_INTERVAL_MS = 1000;
        constexpr uint32_t BURNER_FLAME_DETECTION_TIMEOUT_MS = 5000;
        constexpr uint32_t BURNER_PURGE_TIME_MS = 30000;  // 30 seconds
        constexpr uint32_t BURNER_MIN_IGNITION_TIME_MS = 3000;  // Minimum time before checking flame (real ignition takes 3-5s)
        
        // Relay timing
        constexpr uint32_t RELAY_DEBOUNCE_TIME_MS = 150;
        constexpr uint32_t RELAY_MIN_SWITCH_INTERVAL_MS = 150;
        constexpr uint32_t RELAY_OPERATION_TIMEOUT_MS = 1000;
        
        // Sensor timing
        // Note: Each sensor has its own specific interval - no generic interval used
        constexpr uint32_t MB8ART_SENSOR_READ_INTERVAL_MS = 2500;  // 2.5s - critical boiler temps
        constexpr uint32_t ANDRTF3_SENSOR_READ_INTERVAL_MS = 5000;  // 5s - room temp changes slowly
        constexpr uint32_t SENSOR_TIMEOUT_MS = 30000;  // 30 seconds
        // Note: Sensor staleness threshold moved to Safety::SENSOR_STALE_THRESHOLD_MS
        
        // Network timing
        constexpr uint32_t NETWORK_RECONNECT_DELAY_MS = 5000;
        constexpr uint32_t MQTT_KEEPALIVE_INTERVAL_MS = 60000;  // 60 seconds
        constexpr uint32_t MQTT_SENSOR_PUBLISH_INTERVAL_MS = 10000;  // 10s - balanced update rate
        constexpr uint32_t MQTT_HEALTH_PUBLISH_INTERVAL_MS = 60000;  // 60s - system health
        constexpr uint32_t MQTT_PUBLISH_TIMEOUT_MS = 5000;
        
        // Mutex timeouts - IMPORTANT: Never use portMAX_DELAY to prevent deadlocks
        // Policy:
        // - SHORT (50ms): Quick operations, sensor reads, status checks
        // - DEFAULT (100ms): Standard operations, state changes, control logic
        // - LONG (1000ms): Complex operations, initialization, service registration
        constexpr uint32_t MUTEX_SHORT_TIMEOUT_MS = 50;
        constexpr uint32_t MUTEX_DEFAULT_TIMEOUT_MS = 100;
        constexpr uint32_t MUTEX_LONG_TIMEOUT_MS = 1000;
        
        // Task monitoring intervals
        constexpr uint32_t HEALTH_CHECK_INTERVAL_MS = 5000;     // 5s - basic health monitoring
        constexpr uint32_t DETAILED_MONITOR_INTERVAL_MS = 600000; // 10 minutes - detailed diagnostics
        constexpr uint32_t STATUS_LOG_INTERVAL_MS = 600000;      // 10 minutes - status logging
        
        // Retry and recovery intervals
        constexpr uint32_t RETRY_INTERVAL_MS = 30000;           // 30s - general retry interval
        constexpr uint32_t OFFLINE_RETRY_INTERVAL_MS = 30000;   // 30s - offline device retry
        constexpr uint32_t RECOVERY_DELAY_MS = 30000;           // 30s - error recovery delay

        // Safety and interlock timing
        constexpr uint32_t SAFETY_CHECK_INTERVAL_MS = 100;      // 100ms - pump verification polling
        constexpr uint32_t FLOW_CHECK_WAIT_MS = 500;            // 500ms - flow detection wait
        constexpr uint32_t RELAY_COMMAND_WAIT_MS = 500;         // 500ms - relay command + Modbus verification
        constexpr uint32_t RELAY_RETRY_DELAY_MS = 300;          // 300ms - relay retry delay with bus margin
        constexpr uint32_t FAILSAFE_COOLDOWN_MS = 30000;        // 30s - failsafe cooldown period

        // Error recovery timing
        constexpr uint32_t RECOVERY_WAIT_MS = 5000;             // 5s - recovery operation wait
        constexpr uint32_t RECOVERY_MONITOR_INTERVAL_MS = 60000; // 60s - recovery monitor cycle
        constexpr uint32_t RECOVERY_STABILIZATION_MS = 500;     // 500ms - recovery stabilization
        constexpr uint32_t SYSTEM_STABILIZATION_MS = 2000;      // 2s - full system stabilization
        constexpr uint32_t CLEANUP_DELAY_MS = 200;              // 200ms - cleanup delay
        constexpr uint32_t FINAL_CLEANUP_DELAY_MS = 100;        // 100ms - final cleanup

        // UI / Status indication
        constexpr uint32_t FAILSAFE_LED_BLINK_MS = 500;         // 500ms - LED blink rate in failsafe mode

        // Pump control timing
        constexpr uint32_t PUMP_CHECK_INTERVAL_MS = 500;        // 500ms - pump status check (2Hz)
        constexpr uint32_t PUMP_MIN_STATE_CHANGE_MS = 30000;    // 30s - minimum time between pump on/off transitions (motor protection)
        constexpr uint32_t PUMP_COOLDOWN_MS = 180000;           // 3 min - pump continues after burner stops (heat dissipation)

        // Scheduler intervals
        constexpr uint32_t SCHEDULE_CHECK_INTERVAL_MS = 30000;  // 30s - hot water schedule check
        constexpr uint32_t OTA_UPDATE_CHECK_INTERVAL_MS = 30000; // 30s - OTA update check
        
        // NTP synchronization
        constexpr uint32_t AUTO_SYNC_INTERVAL_MS = 21600000;    // 6 hours - NTP auto sync

        // Time units
        constexpr uint32_t MS_PER_DAY = 86400000;               // 24 hours in ms
    }

    // ===========================
    // Task-Specific Constants
    // ===========================
    namespace Tasks {
        // MQTT Task
        namespace MQTT {
            constexpr uint32_t MIN_RECONNECT_INTERVAL_MS = 1000;
            constexpr uint32_t MAX_RECONNECT_INTERVAL_MS = 30000;
            constexpr uint32_t CONNECTION_CHECK_INTERVAL_MS = 1000;
            constexpr uint32_t QUEUE_DROP_LOG_INTERVAL_MS = 5000;
            constexpr uint32_t QUEUE_HEALTH_LOG_INTERVAL_MS = 60000;
            constexpr uint32_t QUEUE_STATUS_LOG_INTERVAL_MS = 30000;  // 30s - queue size logging
            constexpr int MAX_ITEMS_PER_ITERATION = 12;               // Max messages per processing loop
        }

        // NTP Task
        namespace NTP {
            constexpr uint32_t SYNC_INTERVAL_MS = 3600000;      // 1 hour
            constexpr uint32_t STATUS_LOG_INTERVAL_MS = 300000; // 5 minutes
            constexpr uint32_t RETRY_INTERVAL_MS = 30000;       // 30 seconds
        }

        // Scheduler Task
        namespace Scheduler {
            constexpr uint32_t CHECK_INTERVAL_MS = 30000;       // 30 seconds
            constexpr uint32_t PERSIST_INTERVAL_MS = 300000;    // 5 minutes
        }

        // Persistent Storage Task
        namespace Storage {
            constexpr uint32_t SAVE_INTERVAL_MS = 300000;       // 5 minutes
        }

        // Heating Control Task
        namespace Heating {
            constexpr uint32_t UPDATE_INTERVAL_MS = 10000;      // 10 seconds
            constexpr uint32_t REFRESH_INTERVAL_MS = 300000;    // 5 minutes - parameter refresh
        }

        // Water Heating Control Task
        namespace Wheater {
            constexpr uint32_t REFRESH_INTERVAL_MS = 300000;           // 5 minutes - parameter refresh
            constexpr uint32_t AUTOTUNE_REFRESH_INTERVAL_MS = 300000;  // 5 minutes - autotune status refresh
        }

        // Relay Control Task
        namespace RelayControl {
            constexpr uint32_t MAX_WAIT_TIME_MS = 30000;        // 30s - maximum event wait time
        }

        // Boiler Temperature Control Task
        namespace BoilerTempControl {
            constexpr uint32_t LOG_INTERVAL_MS = 30000;         // 30s - output logging interval
        }

        // MB8ART Sensor Task
        namespace MB8ART {
            constexpr uint32_t RANGE_LOG_INTERVAL_MS = 30000;   // 30s - range warning log interval
        }
    }

    // ===========================
    // Temperature Constants
    // ===========================
    namespace Temperature {
        // Operating limits (in tenths of degrees Celsius)
        constexpr Temperature_t MAX_BOILER_TEMP_C = 1100;       // 110.0°C - Operational ceiling
        constexpr Temperature_t MIN_BOILER_TEMP_C = 200;        // 20.0°C - Operational floor
        constexpr Temperature_t CRITICAL_BOILER_TEMP_C = 1150;  // 115.0°C - Emergency shutdown (5°C margin above max)
        constexpr Temperature_t FREEZE_PROTECTION_TEMP_C = 50;  // 5.0°C - Anti-freeze activation
        
        // Hysteresis values (in tenths of degrees Celsius)
        constexpr Temperature_t TEMP_HYSTERESIS_C = 20;         // 2.0°C
        constexpr Temperature_t TEMP_HYSTERESIS_SMALL_C = 5;    // 0.5°C
        constexpr Temperature_t TEMP_HYSTERESIS_LARGE_C = 50;   // 5.0°C
        
        // Sensor validation (in tenths of degrees Celsius)
        constexpr Temperature_t SENSOR_MIN_VALID_C = -400;      // -40.0°C (generic)
        constexpr Temperature_t SENSOR_MAX_VALID_C = 1250;      // 125.0°C (generic)
        constexpr Temperature_t SENSOR_CHANGE_RATE_MAX_C_PER_SEC = 100; // 10.0°C/sec

        // Sensor-specific validation ranges (in tenths of degrees Celsius)
        namespace SensorRange {
            // Boiler sensors (PT1000/NTC typically rated -50°C to +150°C)
            constexpr Temperature_t BOILER_SENSOR_MIN = -500;   // -50.0°C
            constexpr Temperature_t BOILER_SENSOR_MAX = 1500;   // 150.0°C

            // Water heater tank sensor (lower max - tank should never exceed 100°C)
            constexpr Temperature_t WATER_TANK_SENSOR_MIN = -500;   // -50.0°C
            constexpr Temperature_t WATER_TANK_SENSOR_MAX = 1000;   // 100.0°C

            // Room temperature sensor (ANDRTF3 - building environment)
            constexpr Temperature_t ROOM_SENSOR_MIN = -100;     // -10.0°C
            constexpr Temperature_t ROOM_SENSOR_MAX = 500;      // 50.0°C
        }
        
        // Default setpoints (in tenths of degrees Celsius)
        constexpr Temperature_t DEFAULT_ROOM_SETPOINT_C = 200;  // 20.0°C
        constexpr Temperature_t DEFAULT_WATER_SETPOINT_C = 550; // 55.0°C
        constexpr Temperature_t DEFAULT_BOILER_SETPOINT_C = 700; // 70.0°C

        // Space heating schedule defaults (in tenths of degrees Celsius)
        namespace SpaceHeating {
            constexpr Temperature_t DEFAULT_COMFORT_TEMP = 210;   // 21.0°C comfort mode
            constexpr Temperature_t DEFAULT_ECO_TEMP = 180;       // 18.0°C eco mode
            constexpr Temperature_t DEFAULT_FROST_TEMP = 100;     // 10.0°C frost protection
            constexpr Temperature_t MIN_TARGET_TEMP = 100;        // 10.0°C minimum schedule target (Round 14 Issue #13)
            constexpr Temperature_t MAX_TARGET_TEMP = 300;        // 30.0°C maximum schedule target (Round 14 Issue #13)
            constexpr Temperature_t HEATING_RATE_PER_HOUR = 20;   // 2.0°C/hour typical heating rate
            constexpr Temperature_t HEATING_RATE_PER_MINUTE = 0;  // 0.033°C/min (calculated at runtime)
        }
        
        // Temperature difference thresholds (in tenths of degrees Celsius)
        constexpr Temperature_t TEMP_DIFF_THRESHOLD_TINY_C = 1;    // 0.1°C
        constexpr Temperature_t TEMP_DIFF_THRESHOLD_SMALL_C = 10;  // 1.0°C
        constexpr Temperature_t TEMP_DIFF_THRESHOLD_MEDIUM_C = 50; // 5.0°C

        // Thermal shock protection (output vs return temp difference)
        constexpr Temperature_t MAX_TEMP_DIFFERENTIAL_C = 300;     // 30.0°C max output-return differential

        // Water heater limits
        constexpr Temperature_t WATER_MAX_SAFE_TEMP_C = 650;       // 65.0°C max water heater temp
        
        // Temperature conversion constants
        constexpr float TEMP_SCALE_FACTOR = 10.0f;
        constexpr float TEMP_ROUNDING_POSITIVE = 0.5f;
        constexpr float TEMP_ROUNDING_NEGATIVE = -0.5f;
        constexpr float TEMP_MAX_FLOAT = 3276.7f;
        constexpr float TEMP_MIN_FLOAT = -3276.8f;
        
        // Minimum valid target temperature for control
        constexpr Temperature_t MIN_VALID_TARGET_TEMP_C = 10;  // 1.0°C
        
        // Auto-tuning temperature constants (for boiler output temp, not room temp)
        constexpr Temperature_t AUTOTUNE_TEMP_OFFSET_C = 0;        // Tune at burner low limit (faster oscillations)
        constexpr Temperature_t AUTOTUNE_MAX_SETPOINT_C = 850;     // 85.0°C safety limit (boiler)
    }
    
    // ===========================
    // PID Control Constants
    // ===========================
    namespace PID {
        // Output limits
        constexpr float OUTPUT_MIN = -100.0f;
        constexpr float OUTPUT_MAX = 100.0f;
        
        // Integral windup limits
        constexpr float INTEGRAL_MIN = -100.0f;
        constexpr float INTEGRAL_MAX = 100.0f;
        
        // Default gains
        constexpr float DEFAULT_KP = 2.0f;
        constexpr float DEFAULT_KI = 0.1f;
        constexpr float DEFAULT_KD = 0.5f;
        
        // Adjustment levels
        constexpr float LEVEL_0_THRESHOLD = -50.0f;  // Maximum cooling
        constexpr float LEVEL_1_THRESHOLD = -20.0f;  // Moderate cooling
        constexpr float LEVEL_2_THRESHOLD = -5.0f;   // Light cooling
        constexpr float LEVEL_3_THRESHOLD = 3.0f;    // Maintain (burner off)
        constexpr float LEVEL_4_THRESHOLD = 5.0f;    // HALF power starts
        constexpr float LEVEL_5_THRESHOLD = 10.0f;   // FULL power starts
        // Above LEVEL_5_THRESHOLD = Maximum heating (FULL power)
        
        // Auto-tuning parameters
        constexpr float AUTOTUNE_RELAY_AMPLITUDE = 40.0f;
        constexpr float AUTOTUNE_HYSTERESIS_BAND = 2.0f;
        constexpr float AUTOTUNE_HYSTERESIS_BAND_SMALL = 1.0f;

        namespace Autotune {
            constexpr uint8_t MIN_CYCLES = 3;              // Minimum oscillation cycles needed (3 is standard)
            constexpr uint8_t MAX_CYCLES = 10;             // Maximum cycles before timeout
            constexpr float MAX_TUNING_TIME_SECONDS = 2400.0f;  // 40 minutes maximum tuning time
            constexpr float DEFAULT_RELAY_AMPLITUDE = 50.0f;    // Default relay output amplitude (50% swing)
            constexpr float DEFAULT_RELAY_HYSTERESIS = 1.0f;    // Default hysteresis band (1°C)

            // Safety limits for auto-tuning (in fixed-point: value * 10)
            constexpr Temperature_t MIN_BOILER_TEMP = 150;      // 15.0°C - min boiler temp to start tuning
            constexpr Temperature_t MAX_BOILER_TEMP = 750;      // 75.0°C - max boiler temp to start tuning
            constexpr Temperature_t MAX_TEMP_EXCURSION = 800;   // 80.0°C - abort if exceeded during tuning

            // Result validation limits
            constexpr float MIN_VALID_KP = 0.1f;
            constexpr float MAX_VALID_KP = 10.0f;
            constexpr float MIN_VALID_KI = 0.0f;
            constexpr float MAX_VALID_KI = 1.0f;
            constexpr float MIN_VALID_KD = 0.0f;
            constexpr float MAX_VALID_KD = 5.0f;
            constexpr float MIN_VALID_KU = 0.0f;
            constexpr float MAX_VALID_KU = 50.0f;
            constexpr float MIN_VALID_TU = 30.0f;   // 30 seconds minimum period
            constexpr float MAX_VALID_TU = 600.0f;  // 600 seconds maximum period
        }
        
        // Power control thresholds
        constexpr uint8_t POWER_THRESHOLD_LOW_HIGH = 50;
        constexpr uint32_t MIN_ADJUSTMENT_LEVEL_FOR_ON = 3;
        constexpr uint32_t MIN_ADJUSTMENT_LEVEL_FOR_HIGH = 5;
        
        // Progress reporting
        constexpr uint32_t PROGRESS_PUBLISH_INTERVAL_MS = 5000;
        
        // Fixed-point math scaling
        constexpr int16_t COEFFICIENT_SCALE_FACTOR = 100;
        constexpr int32_t PID_FIXED_POINT_SCALE = 1000;
        
        // Default time delta
        constexpr uint32_t DEFAULT_TIME_DELTA_MS = 1000;
    }

    // ===========================
    // Water Heating Control Constants
    // ===========================
    namespace WaterHeating {
        // Target temperature limits (fixed-point: tenths of °C)
        // Note: tempFromWhole() is not constexpr, so use raw values
        constexpr Temperature_t MIN_TARGET_TEMP = 300;  // 30.0°C minimum (30 * 10)
        constexpr Temperature_t MAX_TARGET_TEMP = 850;  // 85.0°C maximum (85 * 10)

        // PID task timing
        constexpr uint32_t PID_INTERVAL_MS = 2000;      // 2s - faster than space heating (5s)
        constexpr uint32_t WAIT_INTERVAL_MS = 500;      // 500ms - polling when inactive
    }

    // ===========================
    // Relay Configuration
    // ===========================
    namespace Relay {
        // Relay safety limits
        constexpr uint32_t MAX_TOGGLE_RATE_PER_MINUTE = 30;
        constexpr uint32_t SAFETY_INTERLOCK_COUNT = 3;

        // DELAY watchdog: Hardware-level relay safety
        constexpr uint8_t DELAY_WATCHDOG_SECONDS = 10;  // All relays auto-OFF in 10s if not renewed
        // Renewed every 5s (50% safety margin) to ensure continuous operation
        // Protects against ESP32 crash, hang, or power loss

        // Relay states
        constexpr bool RELAY_STATE_OFF = false;
        constexpr bool RELAY_STATE_ON = true;
        constexpr bool RELAY_SAFE_STATE = false;  // Safe position for emergencies
        
        // Relay indices (1-based for user interface)
        constexpr uint8_t BURNER_RELAY = 1;
        constexpr uint8_t HEATING_PUMP_RELAY = 2;
        constexpr uint8_t WATER_PUMP_RELAY = 3;
        constexpr uint8_t ALARM_RELAY = 8;
    }
    
    // ===========================
    // Burner Control Constants
    // ===========================
    namespace Burner {
        // Anti-flapping timers
        constexpr uint32_t MIN_ON_TIME_MS = 120000;              // 2 minutes minimum run time
        constexpr uint32_t MIN_OFF_TIME_MS = 20000;              // 20 seconds minimum off time
        constexpr uint32_t MIN_POWER_CHANGE_INTERVAL_MS = 15000; // 15 seconds between power changes (solenoid valve)
        
        // Power level hysteresis
        constexpr float POWER_LEVEL_HYSTERESIS_PERCENT = 10.0f;  // 10% change required to switch power levels
        constexpr float PID_OUTPUT_DEADBAND = 5.0f;              // ±5% deadband to ignore minor fluctuations
        
        // Burner state machine timings
        // For atmospheric burners (natural draft, no forced fan):
        constexpr uint32_t PRE_PURGE_TIME_MS = 2000;     // 2s pre-purge (atmospheric burner, minimal purge needed)
        constexpr uint32_t IGNITION_TIME_MS = 5000;      // 5s for ignition sequence
        constexpr uint32_t POST_PURGE_TIME_MS = 60000;   // 60s post-purge
        constexpr uint32_t LOCKOUT_TIME_MS = 300000;     // 5 minutes lockout on failure
        // NOTE: For forced-draft burners, increase PRE_PURGE to 15-30s
        
        // Ignition parameters
        constexpr uint8_t MAX_IGNITION_RETRIES = 3;      // Maximum ignition attempts

        // Error logging
        constexpr uint32_t MAX_ERROR_LOG_INTERVAL_MS = 300000;  // 5 minutes max between repeated error logs
        constexpr uint32_t MIN_ERROR_LOG_INTERVAL_MS = 1000;    // 1 second initial interval

        // Request watchdog - auto-clear stale requests to prevent runaway burner
        constexpr uint32_t REQUEST_EXPIRATION_MS = 600000;      // 10 minutes - requests must be refreshed

        // Safety check timing
        constexpr uint32_t STARTUP_GRACE_PERIOD_MS = 60000;     // 60s grace period at startup
        constexpr uint32_t MODE_TRANSITION_GRACE_MS = 2000;     // 2s grace for mode transitions
        constexpr uint32_t GRACE_LOG_INTERVAL_MS = 1000;        // 1s between grace period logs
        constexpr uint32_t TASK_NOTIFY_TIMEOUT_MS = 1000;       // 1s task notification timeout

        // Status reporting
        constexpr uint32_t STATUS_PUBLISH_INTERVAL_MS = 30000;  // 30s - recovery status publishing
    }

    // ===========================
    // Boiler Temperature Control Constants
    // ===========================
    namespace BoilerControl {
        // Default hysteresis bands for three-point bang-bang control
        // Values in Temperature_t (tenths of degrees Celsius)
        constexpr Temperature_t DEFAULT_OFF_HYSTERESIS = 50;     // +5.0°C above target → OFF
        constexpr Temperature_t DEFAULT_ON_HYSTERESIS = 30;      // -3.0°C below target → ON (HALF)
        constexpr Temperature_t DEFAULT_FULL_THRESHOLD = 100;    // -10.0°C below target → FULL

        // Minimum valid target temperature (safety)
        constexpr Temperature_t MIN_TARGET_TEMP = 200;           // 20.0°C minimum target

        // Control loop timing
        constexpr uint32_t CONTROL_INTERVAL_MS = 2500;           // Match MB8ART sensor interval

        // Watchdog timeout for BoilerTempControlTask
        constexpr uint32_t WDT_TIMEOUT_MS = 10000;               // 10s (2x sensor interval + margin)
    }

    // ===========================
    // System Thresholds
    // ===========================
    namespace System {
        // Memory thresholds
        constexpr size_t MIN_FREE_HEAP_CRITICAL = 10000;   // 10KB
        constexpr size_t MIN_FREE_HEAP_WARNING = 20000;    // 20KB
        constexpr size_t MIN_FREE_HEAP_NORMAL = 40000;     // 40KB
        
        // Memory allocation requirements
        constexpr size_t MIN_HEAP_FOR_MQTT = 30000;        // 30KB for MQTT operations
        constexpr size_t MIN_HEAP_FOR_OPERATION = 20000;   // 20KB minimum operational
        constexpr size_t CRITICAL_HEAP_THRESHOLD = 15000;  // 15KB critical threshold
        
        // Stack sizes
        constexpr size_t STACK_SIZE_MINIMUM = 2048;
        constexpr size_t STACK_SIZE_SMALL = 3072;
        constexpr size_t STACK_SIZE_MEDIUM = 4096;
        constexpr size_t STACK_SIZE_LARGE = 6144;
        
        // Watchdog timeouts - based on worst-case task execution times
        // Rule: timeout >= (max_loop_delay + max_processing_time) * 3
        constexpr uint32_t WATCHDOG_MULTIPLIER = 3;
        constexpr uint32_t WATCHDOG_MIN_TIMEOUT_MS = 5000;

        // Task-specific watchdog timeouts (milliseconds)
        // Critical tasks (cause system reset on timeout)
        constexpr uint32_t WDT_BURNER_CONTROL_MS = 15000;   // 15s: 3s idle loop + processing margin
        constexpr uint32_t WDT_RELAY_CONTROL_MS = 10000;    // 10s: 1s chunked waits + Modbus ops

        // Non-critical tasks (logged but no reset)
        constexpr uint32_t WDT_CONTROL_TASK_MS = 10000;     // 10s: remote control handling
        constexpr uint32_t WDT_HEATING_CONTROL_MS = 20000;  // 20s: timer-based processing
        constexpr uint32_t WDT_WHEATER_CONTROL_MS = 20000;  // 20s: timer-based processing
        constexpr uint32_t WDT_MQTT_TASK_MS = 30000;        // 30s: network operations
        constexpr uint32_t WDT_MONITORING_MS = 30000;       // 30s: detailed report generation
        constexpr uint32_t WDT_SENSOR_PROCESSING_MS = 30000; // 30s: Modbus sensor reads
        constexpr uint32_t WDT_SCHEDULER_MS = 30000;        // 30s: schedule processing
        constexpr uint32_t WDT_OTA_TASK_MS = 60000;         // 60s: OTA update operations

        // Error thresholds
        constexpr uint32_t MAX_CONSECUTIVE_ERRORS = 5;
        constexpr uint32_t ERROR_RECOVERY_DELAY_MS = 60000;  // 1 minute

        // System monitoring
        constexpr uint32_t MEMORY_REPORT_INTERVAL_MS = 300000;  // 5 minutes - memory status reporting
    }

    // ===========================
    // Fixed-Point Arithmetic
    // ===========================
    namespace FixedPoint {
        // Percentage scaling: 10000 = 100.00%
        // Allows 2 decimal places: 5432 = 54.32%
        constexpr uint16_t PERCENTAGE_SCALE = 10000;
        constexpr uint16_t PERCENTAGE_100 = 10000;  // 100%
        constexpr uint16_t PERCENTAGE_50 = 5000;    // 50%
    }

    // ===========================
    // Safety Constants
    // ===========================
    namespace Safety {
        // Sensor staleness detection
        constexpr uint32_t SENSOR_STALE_THRESHOLD_MS = 15000;  // 15s - sensors older than this block burner

        // Safety check intervals
        constexpr uint32_t FULL_CHECK_INTERVAL_MS = 5000;      // 5s - full safety check
        constexpr uint32_t CRITICAL_CHECK_INTERVAL_MS = 500;   // 500ms - critical checks
        constexpr uint32_t EMERGENCY_TIMEOUT_MS = 5000;        // 5s - emergency shutdown timeout

        // Recovery parameters
        constexpr uint8_t MAX_RECOVERY_ATTEMPTS = 3;           // Max recovery attempts
        constexpr uint32_t MIN_RESTART_INTERVAL_MS = 5000;     // 5s - minimum between restarts

        // Error tracking
        constexpr uint32_t ERROR_HISTORY_WINDOW_MS = 300000;   // 5 minutes - error tracking window
        constexpr uint8_t MAX_ERRORS_PER_WINDOW = 10;          // Max errors in window

        // Thermal shock thresholds
        // Warning at 80% of limit (4/5 = 80%), represented as numerator/denominator
        constexpr uint8_t THERMAL_SHOCK_WARNING_NUM = 4;       // 80% warning threshold numerator
        constexpr uint8_t THERMAL_SHOCK_WARNING_DEN = 5;       // 80% warning threshold denominator

        // Return preheating (thermal shock mitigation via pump cycling)
        namespace ReturnPreheat {
            constexpr Temperature_t MAX_DIFFERENTIAL = 350;       // 35.0°C - block burner above this
            constexpr Temperature_t SAFE_DIFFERENTIAL = 250;      // 25.0°C - exit preheating below this
            constexpr uint8_t MAX_CYCLES = 8;                     // Maximum pump cycles before timeout
            constexpr uint32_t PREHEAT_TIMEOUT_MS = 600000;       // 10 minutes maximum preheating time
            constexpr uint8_t OFF_MULTIPLIER = 5;                 // Scaling factor for OFF durations (5 = 1x, 10 = 2x, 1 = 0.2x)
            constexpr uint16_t PUMP_MIN_CHANGE_MS = 3000;         // 3s minimum between pump state changes during preheating

            // Progressive ON durations (seconds) - increases each cycle for more circulation
            constexpr uint8_t CYCLE_1_ON_SEC = 3;
            constexpr uint8_t CYCLE_2_ON_SEC = 5;
            constexpr uint8_t CYCLE_3_ON_SEC = 8;
            constexpr uint8_t CYCLE_4_ON_SEC = 12;
            constexpr uint8_t CYCLE_5_PLUS_ON_SEC = 15;           // Cycles 5+ use this

            // Progressive OFF durations (seconds) - decreases each cycle as system warms
            constexpr uint8_t CYCLE_1_OFF_SEC = 25;               // Long wait initially (high differential)
            constexpr uint8_t CYCLE_2_OFF_SEC = 20;
            constexpr uint8_t CYCLE_3_OFF_SEC = 15;
            constexpr uint8_t CYCLE_4_OFF_SEC = 10;
            constexpr uint8_t CYCLE_5_PLUS_OFF_SEC = 5;           // Short wait as system warms up
        }

        // Pressure limits (in hundredths of BAR for fixed-point representation)
        // Note: Pressure_t is defined in include/shared/Pressure.h
        namespace Pressure {
            // Operating limits (hundredths of BAR)
            constexpr int16_t MIN_OPERATING = 100;  // 1.00 BAR minimum operational pressure
            constexpr int16_t MAX_OPERATING = 350;  // 3.50 BAR maximum operational pressure

            // Alarm thresholds (hundredths of BAR)
            constexpr int16_t ALARM_MIN = 50;       // 0.50 BAR - critical low pressure alarm
            constexpr int16_t ALARM_MAX = 400;      // 4.00 BAR - critical high pressure alarm
        }

        // Configuration validation ranges (MQTT command input validation)
        namespace ConfigValidation {
            // Preheat pump minimum change interval
            constexpr uint32_t PREHEAT_PUMP_MIN_MS_MIN = 1000;    // 1 second minimum
            constexpr uint32_t PREHEAT_PUMP_MIN_MS_MAX = 30000;   // 30 seconds maximum

            // Preheat timeout
            constexpr uint32_t PREHEAT_TIMEOUT_MIN_MS = 60000;    // 1 minute minimum
            constexpr uint32_t PREHEAT_TIMEOUT_MAX_MS = 1200000;  // 20 minutes maximum
        }
    }
    
    // ===========================
    // Communication Constants
    // ===========================
    namespace Communication {
        // Modbus
        constexpr uint32_t MODBUS_TIMEOUT_MS = 500;
        constexpr uint32_t MODBUS_RETRY_COUNT = 3;
        constexpr uint32_t MODBUS_RETRY_DELAY_MS = 20;       // Was 100ms before parity fix
        // MODBUS_INTER_FRAME_DELAY_MS is auto-calculated in ESP32-ModbusDevice/src/ModbusTypes.h
        // based on MODBUS_BAUD_RATE (defined in platformio.ini)
        constexpr uint32_t SENSOR_RETRY_DELAY_MS = 10;     // Delay between sensor read retries (was 50ms before parity fix)

        // I2C / FRAM
        constexpr uint32_t I2C_READ_TIMEOUT_MS = 100;      // Timeout for I2C read operations

        // MQTT
        #ifndef MQTT_BUFFER_SIZE
        constexpr size_t MQTT_BUFFER_SIZE = 1024;
        #endif
        constexpr size_t MQTT_TOPIC_MAX_LENGTH = 128;
        constexpr size_t MQTT_PAYLOAD_MAX_LENGTH = 512;
        constexpr uint8_t MQTT_QOS_AT_MOST_ONCE = 0;
        constexpr uint8_t MQTT_QOS_AT_LEAST_ONCE = 1;
        constexpr uint8_t MQTT_QOS_EXACTLY_ONCE = 2;
        
        // Serial
        #ifndef SERIAL_BUFFER_SIZE
        constexpr size_t SERIAL_BUFFER_SIZE = 256;
        #endif
    }
    
    // ===========================
    // Control Loop Constants
    // ===========================
    namespace Control {
        // Update rates
        constexpr uint32_t FAST_CONTROL_LOOP_MS = 100;    // 10 Hz
        constexpr uint32_t NORMAL_CONTROL_LOOP_MS = 1000; // 1 Hz
        constexpr uint32_t SLOW_CONTROL_LOOP_MS = 5000;   // 0.2 Hz
        
        // State machine
        constexpr uint32_t STATE_TRANSITION_DELAY_MS = 100;
        constexpr uint32_t STATE_TIMEOUT_DEFAULT_MS = 30000;
        
        // Safety
        constexpr uint32_t EMERGENCY_STOP_RESPONSE_MS = 10;
        constexpr uint32_t SAFETY_CHECK_INTERVAL_MS = 1000;
    }
    
    // ===========================
    // Diagnostics Constants
    // ===========================
    namespace Diagnostics {
        // Reporting intervals
        #ifndef STATUS_REPORT_INTERVAL_MS
        constexpr uint32_t STATUS_REPORT_INTERVAL_MS = 60000;    // 1 minute
        #endif
        constexpr uint32_t DETAILED_REPORT_INTERVAL_MS = 300000; // 5 minutes
        constexpr uint32_t ERROR_REPORT_COOLDOWN_MS = 3600000;   // 1 hour

        // Memory recovery
        constexpr uint32_t RECOVERY_DELAY_MS = 30000;            // 30s - diagnostics task resume delay

        // MQTT diagnostics intervals (normal operation)
        constexpr uint32_t HEALTH_INTERVAL_MS = 60000;           // 1 minute
        constexpr uint32_t MEMORY_INTERVAL_MS = 300000;          // 5 minutes
        constexpr uint32_t TASKS_INTERVAL_MS = 300000;           // 5 minutes
        constexpr uint32_t SENSORS_INTERVAL_MS = 30000;          // 30 seconds
        constexpr uint32_t RELAYS_INTERVAL_MS = 10000;           // 10 seconds
        constexpr uint32_t NETWORK_INTERVAL_MS = 60000;          // 1 minute
        constexpr uint32_t PERFORMANCE_INTERVAL_MS = 60000;      // 1 minute
        constexpr uint32_t PID_INTERVAL_MS = 5000;               // 5 seconds
        constexpr uint32_t BURNER_INTERVAL_MS = 5000;            // 5 seconds
        constexpr uint32_t MAINTENANCE_INTERVAL_MS = 3600000;    // 1 hour

        // Thresholds
        constexpr float ERROR_RATE_THRESHOLD_PERCENT = 5.0f;
        constexpr uint32_t MIN_SAMPLES_FOR_STATISTICS = 100;
    }
    
    // ===========================
    // Queue Management Constants
    // ===========================
    namespace QueueManagement {
        // Queue depths and timeouts
        constexpr size_t DEFAULT_QUEUE_DEPTH = 10;
        constexpr uint32_t DEFAULT_QUEUE_TIMEOUT_MS = 100;
        
        // Queue metrics
        constexpr uint32_t METRICS_PUBLISH_INTERVAL_MS = 10000;  // 10s
        constexpr size_t METRICS_WINDOW_SIZE = 100;
        constexpr float HEALTHY_DROP_RATE = 0.01f;               // 1% drop rate
        constexpr float WARNING_UTILIZATION = 0.8f;              // 80% utilization
        constexpr uint32_t RECENT_TIME_MS = 5000;                // 5s for recent metrics
    }
    
    // ===========================
    // Error Logging Constants
    // ===========================
    namespace ErrorLogging {
        // Error log limits
        constexpr size_t MAX_ERRORS = 50;
        constexpr size_t MAX_CRITICAL_ERRORS = 5;
        constexpr size_t MAX_MESSAGE_LENGTH = 64;
        constexpr size_t MAX_CONTEXT_LENGTH = 32;

        // Error rate limiting (exponential backoff)
        constexpr uint32_t RATE_LIMIT_INITIAL_INTERVAL_MS = 1000;   // 1 second - first error log
        constexpr uint32_t RATE_LIMIT_MAX_INTERVAL_MS = 300000;     // 5 minutes - max interval between repeated logs

        // Error recovery window
        constexpr uint32_t ERROR_WINDOW_MS = 300000;                // 5 minutes - error tracking window

        // Error persistence keys
        constexpr const char* KEY_ERROR_COUNT = "err_count";
        constexpr const char* KEY_ERROR_INDEX = "err_index";
        constexpr const char* KEY_ERROR_STATS = "err_stats";
        constexpr const char* KEY_ERROR_PREFIX = "err_";
        constexpr const char* KEY_CRITICAL_PREFIX = "crit_";
    }
    
    // ===========================
    // String Constants
    // ===========================
    namespace Strings {
        // MQTT Topics
        constexpr const char* MQTT_TOPIC_STATUS = "status/boiler";
        constexpr const char* MQTT_TOPIC_SENSORS = "sensors/";
        constexpr const char* MQTT_TOPIC_RELAYS = "relays/";
        constexpr const char* MQTT_TOPIC_CONTROL = "control/";
        constexpr const char* MQTT_TOPIC_ALERT = "alert/";
        
        // Status messages
        constexpr const char* STATUS_ONLINE = "online";
        constexpr const char* STATUS_OFFLINE = "offline";
        constexpr const char* STATUS_ERROR = "error";
        constexpr const char* STATUS_FAILSAFE = "failsafe";
    }
    
    // ===========================
    // Buffer Size Constants
    // ===========================
    namespace Buffers {
        // Temperature formatting
        constexpr size_t TEMP_FORMAT_BUFFER_SIZE = 16;
        
        // JSON and string buffers
        constexpr size_t SMALL_JSON_BUFFER_SIZE = 64;
        constexpr size_t MEDIUM_JSON_BUFFER_SIZE = 256;
        constexpr size_t LARGE_JSON_BUFFER_SIZE = 512;
        
        // State machine arrays
        constexpr size_t STATE_NAME_ARRAY_SIZE = 8;
        constexpr size_t MAX_STATE_COUNT = 8;
        
        // Logging and debug
        constexpr size_t LOG_MESSAGE_BUFFER_SIZE = 256;
        constexpr size_t DEBUG_MESSAGE_BUFFER_SIZE = 512;
        
        // Network buffers
        constexpr size_t MQTT_TOPIC_BUFFER_SIZE = 128;
        constexpr size_t MQTT_PAYLOAD_BUFFER_SIZE = 512;
    }
    
    // ===========================
    // Hardware Configuration
    // ===========================
    namespace Hardware {
        // NOTE: Relay assignments defined in include/config/RelayIndices.h (single source of truth)

        // Modbus protocol
        constexpr size_t MAX_MODBUS_DATA = 252;      // Maximum Modbus data size

        // Pressure sensor (4-20mA current loop on MB8ART channel 7)
        namespace PressureSensor {
            constexpr float CURRENT_MIN_MA = 4.0f;              // 4mA = 0 BAR
            constexpr float CURRENT_MAX_MA = 20.0f;             // 20mA = max pressure
            constexpr float PRESSURE_AT_MIN_CURRENT = 0.0f;     // Pressure at 4mA (BAR)
            constexpr float PRESSURE_AT_MAX_CURRENT = 5.0f;     // Pressure at 20mA (BAR)
            constexpr float CURRENT_FAULT_THRESHOLD_MA = 3.5f;  // Below this = sensor disconnected
            constexpr float CURRENT_RANGE_MA = CURRENT_MAX_MA - CURRENT_MIN_MA;  // 16mA
            constexpr float PRESSURE_RANGE_BAR = PRESSURE_AT_MAX_CURRENT - PRESSURE_AT_MIN_CURRENT;  // 5 BAR
        }
    }
    
    // ===========================
    // Fixed-Point Math Constants
    // ===========================
    namespace FixedPoint {
        // Heating curve polynomial coefficients (scaled values)
        constexpr int32_t HEATING_CURVE_COEFF_1 = 14347;    // 1.4347 * 10000
        constexpr int32_t HEATING_CURVE_COEFF_2 = 210;      // 0.021 * 10000
        constexpr int32_t HEATING_CURVE_COEFF_3 = 248;      // 0.000248 * 1000000

        // Scaling factors
        constexpr int32_t POLYNOMIAL_SCALE = 10000;
        constexpr int32_t COEFF3_SCALE = 1000000;
        constexpr int32_t ADJUSTMENT_SCALE = 10000000;
    }

    // ===========================
    // Simulation/Testing Constants
    // ===========================
    namespace Simulation {
        // Fake pressure sensor data for testing (when USE_REAL_PRESSURE_SENSOR not defined)
        // Values in hundredths of BAR (Pressure_t fixed-point format)
        // IMPORTANT: These values MUST be within safe operating range (Safety::Pressure)
        constexpr int16_t FAKE_PRESSURE_NOMINAL = 150;  // 1.50 BAR nominal value
        constexpr int16_t FAKE_PRESSURE_MIN = 140;      // 1.40 BAR minimum variation
        constexpr int16_t FAKE_PRESSURE_MAX = 160;      // 1.60 BAR maximum variation
        constexpr int16_t FAKE_PRESSURE_VARIATION = 5;  // ±0.05 BAR random variation
        constexpr uint32_t FAKE_PRESSURE_UPDATE_INTERVAL_MS = 5000;  // 5 seconds between updates
        constexpr uint32_t FAKE_PRESSURE_LOG_INTERVAL_MS = 60000;    // 1 minute between log messages

        // Compile-time validation: fake pressure must be within safe operating range
        static_assert(FAKE_PRESSURE_MIN >= Safety::Pressure::MIN_OPERATING,
                      "FAKE_PRESSURE_MIN below safe operating minimum");
        static_assert(FAKE_PRESSURE_MAX <= Safety::Pressure::MAX_OPERATING,
                      "FAKE_PRESSURE_MAX above safe operating maximum");
    }
}

#endif // SYSTEM_CONSTANTS_H