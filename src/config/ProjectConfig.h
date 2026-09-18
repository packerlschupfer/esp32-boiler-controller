// config/ProjectConfig.h
#pragma once

#include <Arduino.h>
#include <esp_log.h>

// ==========================
// Project Identification
// ==========================
#ifndef PROJECT_NAME
#define PROJECT_NAME "ESPlan Boiler Controller"
#endif
#ifndef PROJECT_VERSION
#define PROJECT_VERSION "0.1.0"
#endif

// Firmware version string - includes git hash if available (from auto_firmware_version.py)
// Maximum expected length: "X.Y.Z-1234567" = 13 chars + null = 14 bytes
// Format: PROJECT_VERSION "-" AUTO_VERSION (e.g., "2.1.0-7d87a38")
#define FIRMWARE_VERSION_MAX_LEN 24  // Allow some headroom for longer versions

#ifdef AUTO_VERSION
#define FIRMWARE_VERSION PROJECT_VERSION "-" AUTO_VERSION
#else
#define FIRMWARE_VERSION PROJECT_VERSION
#endif

// Compile-time validation of firmware version length
static_assert(sizeof(FIRMWARE_VERSION) <= FIRMWARE_VERSION_MAX_LEN,
              "FIRMWARE_VERSION exceeds maximum length - check AUTO_VERSION");

// If not defined in platformio.ini, set defaults
#ifndef DEVICE_HOSTNAME
#define DEVICE_HOSTNAME "ESPlan-Boiler"
#endif

// ==========================
// Hardware Configuration
// ==========================
// Status LED pin (commented out if not used)
// #define STATUS_LED_PIN 2
#ifndef LED_BUILTIN
#define LED_BUILTIN 2  // Default ESP32 LED pin
#endif

// Serial configuration
#ifndef SERIAL_BAUD_RATE
#define SERIAL_BAUD_RATE 921600  // Increased for better logging throughput
#endif

// RS485/Modbus configuration
#define RS485_RX_PIN 36
#define RS485_TX_PIN 4

// Note: MODBUS_BAUD_RATE is defined in platformio.ini build_flags (currently 9600)
//       Used for both UART initialization and inter-frame delay calculation
// Note: MODBUS_INTER_FRAME_DELAY_MS is auto-calculated in ESP32-ModbusDevice/src/ModbusTypes.h
//       Formula: (38500 / MODBUS_BAUD_RATE) + 1ms safety margin
//       At 9600 baud = 5ms, at 19200 = 3ms, at 115200 = 1ms

// Modbus device addresses
#define RYN4_ADDRESS 0x02
#define MB8ART_ADDRESS 0x03
#define ANDRTF3_ADDRESS 0x04

// ==========================
// Sensor Configuration
// ==========================
// Pressure sensor on MB8ART channel 4 (4-20mA, 0-5 BAR / 0-0.5 MPa)
// Enabled - real pressure sensor connected
#define USE_REAL_PRESSURE_SENSOR

// NOTE: When USE_REAL_PRESSURE_SENSOR is undefined, system will use
// simulated pressure data for testing/development.
// See SystemConstants::Simulation for fake sensor configuration.

// Round 20 Issue #7: Safety flag for pressure sensor requirement
// When defined, allows burner operation without a valid pressure sensor (development/testing only)
// WARNING: Do NOT enable in production - pressure monitoring is safety-critical!
#define ALLOW_NO_PRESSURE_SENSOR  // TODO: Remove before production deployment
// #ifdef ALLOW_NO_PRESSURE_SENSOR
//     #warning "ALLOW_NO_PRESSURE_SENSOR is enabled - burner can operate without pressure sensor. Remove for production!"
// #endif

// Optional temperature sensors (default disabled)
// Uncomment to enable these additional MB8ART channels
// #define ENABLE_SENSOR_WATER_TANK_TOP   // CH5 - Top of water tank (stratification)
// #define ENABLE_SENSOR_WATER_RETURN     // CH6 - Water heater return temperature
// #define ENABLE_SENSOR_HEATING_RETURN   // CH7 - Heating system return temperature

// MB8ART active channel configuration
// Only channels 0 to (MB8ART_ACTIVE_CHANNELS-1) will be enabled
// Channels >= MB8ART_ACTIVE_CHANNELS will be deactivated at hardware level
// This prevents error warnings for physically disconnected sensor channels
#define MB8ART_ACTIVE_CHANNELS 5  // CH0-4: Boiler Out, Boiler Return, Water Tank, Outside, Pressure

// ==========================
// Ethernet Configuration
// ==========================
#ifndef ETH_PHY_ADDR
#define ETH_PHY_ADDR 0
#endif

#ifndef ETH_PHY_MDC_PIN
#define ETH_PHY_MDC_PIN 23
#endif

#ifndef ETH_PHY_MDIO_PIN
#define ETH_PHY_MDIO_PIN 18
#endif

#ifndef ETH_PHY_POWER_PIN
#define ETH_PHY_POWER_PIN -1  // No power pin
#endif

#define ETH_PHY_LAN8720 ETH_PHY_LAN8720
#define ETH_CLOCK_MODE ETH_CLOCK_GPIO17_OUT
#define ETH_CONNECTION_TIMEOUT_MS 15000

// Static IP Configuration
#define USE_STATIC_IP

#ifndef ETH_STATIC_IP
#define ETH_STATIC_IP      192, 168, 20, 40
#endif

#ifndef ETH_GATEWAY
#define ETH_GATEWAY        192, 168, 20, 1
#endif

#ifndef ETH_SUBNET
#define ETH_SUBNET         255, 255, 255, 0
#endif

#ifndef ETH_DNS1
#define ETH_DNS1           192, 168, 20, 1
#endif

#ifndef ETH_DNS2
#define ETH_DNS2           8, 8, 8, 8
#endif

// Optional: Custom MAC address
// #define ETH_MAC_ADDRESS {0x02, 0x00, 0x00, 0x12, 0x34, 0x56}

// ==========================
// OTA Settings
// ==========================
#ifndef OTA_PASSWORD
#define OTA_PASSWORD "update-password"  // Set your OTA password here
#endif

#ifndef OTA_PORT
#define OTA_PORT 3232
#endif

// ==========================
// MQTT Configuration
// ==========================
#ifndef MQTT_SERVER
#define MQTT_SERVER "192.168.20.27"
#endif

#ifndef MQTT_PORT
#define MQTT_PORT 1883
#endif
#define MQTT_CLIENT_ID DEVICE_HOSTNAME

// SECURITY: Credentials should be provided via:
// 1. Build flags: -DMQTT_USERNAME="user" -DMQTT_PASSWORD="pass"
// 2. Environment variables at build time
// 3. Secure configuration file (not in git)
#ifndef MQTT_USERNAME
#define MQTT_USERNAME ""
#endif

#ifndef MQTT_PASSWORD
#define MQTT_PASSWORD ""
#endif

#define MQTT_RECONNECT_INTERVAL_MS 5000
#define MQTT_KEEP_ALIVE_SECONDS 60

// ==========================
// Logging Configuration
// ==========================
// Log mode selection (define only one)
// #define LOG_MODE_DEBUG_FULL      // Full debug logging
// #define LOG_MODE_DEBUG_SELECTIVE // Selective debug logging

// Only define LOG_MODE_RELEASE if no other log mode is defined
#if !defined(LOG_MODE_DEBUG_FULL) && !defined(LOG_MODE_DEBUG_SELECTIVE) && !defined(LOG_MODE_RELEASE)
    #define LOG_MODE_RELEASE         // Production logging (default)
#endif

// ==========================
// Task Configuration
// ==========================
// Stack sizes (in bytes) - Three-tier optimization based on logging mode
#if defined(LOG_MODE_DEBUG_FULL)
    // DEBUG FULL MODE - Conservative stack sizes
    #define STACK_SIZE_OTA_TASK              2048  // After fix: conservative for debug mode
    #define STACK_SIZE_MONITORING_TASK       4096  // Safety: increased for task status array allocation
    #define STACK_SIZE_MODBUS_CONTROL_TASK   1536  // After fix: with debug logging
    #define STACK_SIZE_RELAY_CONTROL_TASK    2560  // After fix: needs more for multiple log messages
    #define STACK_SIZE_SENSOR_TASK           2048  // After fix: sensor processing
    #define STACK_SIZE_CONTROL_TASK          2048  // After fix: needs more for logging
    #define STACK_SIZE_WHEATER_CONTROL_TASK  3072  // After fix: needs significant stack for float formatting
    #define STACK_SIZE_MQTT_TASK             3072  // After fix: MQTT with debug
    #define STACK_SIZE_PERSISTENT_STORAGE_TASK 5120  // Optimized publishGroupedCategory() reduces stack usage
    #define STACK_SIZE_BURNER_CONTROL_TASK   4096  // M1 fix: was 2560 (inverted vs DEBUG_SELECTIVE 4096); DEBUG_FULL needs MORE stack for logging
    #define STACK_SIZE_MB8ART_PROCESSING_TASK 3072  // Increased for float formatting in logs
    #define STACK_SIZE_BOILER_TEMP_TASK      4096  // DEBUG_FULL logs more than SELECTIVE
    #define STACK_SIZE_RYN4_PROCESSING_TASK  1536  // After fix: relay processing
    #define STACK_SIZE_TIMER_SCHEDULER_TASK 3072  // Generic timer scheduler (replaces HotWaterScheduler)
    #define STACK_SIZE_LOOP_TASK             3072  // After fix: main loop with debug
    #define STACK_SIZE_PUMP_CONTROL_TASK     2560  // After fix: pump control with logging

#elif defined(LOG_MODE_DEBUG_SELECTIVE)
    // DEBUG SELECTIVE MODE - Optimized based on runtime profiling (Dec 2025)
    // H3: Safety-critical tasks increased by +512 bytes for margin
    #define STACK_SIZE_OTA_TASK              3072  // Dec 2025: was 2560, had 816 free
    #define STACK_SIZE_MONITORING_TASK       3584  // Dec 2025: was 4096, had 2232 free
    #define STACK_SIZE_MODBUS_CONTROL_TASK   3584  // Increased - was showing only 448 bytes free
    #define STACK_SIZE_RELAY_CONTROL_TASK    4096  // H3: Safety-critical +512 (was 3584)
    #define STACK_SIZE_SENSOR_TASK           3584  // Dec 2025: was 4096, ANDRTF3 had 2272 free
    #define STACK_SIZE_CONTROL_TASK          3584  // H3: Safety-critical +512 (was 3072)
    #define STACK_SIZE_WHEATER_CONTROL_TASK  3584  // Dec 2025: was 4096, had 2344 free
    #define STACK_SIZE_MQTT_TASK             3584  // Dec 2025: was 3072, had 712 free
    #define STACK_SIZE_PERSISTENT_STORAGE_TASK 5120  // DO NOT TRIM. Measured peak is only
                                                 // 1860 B, which makes this look like 3 kB of
                                                 // spare DRAM - it is not. tools/stack_walker.py
                                                 // puts the worst-case bound at 4272 B (chain
                                                 // 3904 + prefix + interrupt frame), i.e. only
                                                 // 848 B of real headroom; processCommandQueue
                                                 // alone has a 1328 B frame, the largest in the
                                                 // image. A 2026-09-18 review proposed trimming
                                                 // 1536 B from here to fund the pump and
                                                 // BoilerTempCtrl increases; that would have put
                                                 // the stack ~690 B BELOW its own static bound.
                                                 // Re-check with the walker before touching it.
    #define STACK_SIZE_BURNER_CONTROL_TASK   4096  // H3: Safety-critical +512 (was 3584)
    #define STACK_SIZE_MB8ART_PROCESSING_TASK 3072  // Dec 2025: was 4096, had 2568 free
    #define STACK_SIZE_BOILER_TEMP_TASK      3584  // 2026-09-17: was 3072 with 644 B free;
                                                   // raised for the autotune-completion branch
                                                   // (BoilerTempControlTask.cpp snprintfs three
                                                   // %.4f into char[160]), which no measurement
                                                   // has ever exercised. Measured peak 2540 B.
    #define STACK_SIZE_RYN4_PROCESSING_TASK  2560  // Restored to original size
    #define STACK_SIZE_TIMER_SCHEDULER_TASK 3072  // Generic timer scheduler (replaces HotWaterScheduler)
    #define STACK_SIZE_LOOP_TASK             4096  // After fix: needs more for initialization
    #define STACK_SIZE_PUMP_CONTROL_TASK     2560  // 2026-09-17 MEASURED: 2560 gives 928 B free
                                                 // (peak 1840 B, static from boot, verified at 11.2 h
                                                 // through a full water cycle). Was 3072 aspirationally,
                                                 // but TaskInitializer shadowed this macro with a local
                                                 // 2048 so the pumps never saw either value. 2560 is what
                                                 // is actually validated; the extra 512 B/task is not
                                                 // funded against a 1812 B DRAM floor.

#else  // LOG_MODE_RELEASE
    // RELEASE MODE - Aggressive optimization with SafeLog
    #define STACK_SIZE_OTA_TASK              1536  // >= configMINIMAL_STACK_SIZE (was 1024, rejected)
    #define STACK_SIZE_MONITORING_TASK       3584  // Dec 2025: increased from 3072 for task status allocation + LOG_DEBUG overhead
    #define STACK_SIZE_MODBUS_CONTROL_TASK   1536  // >= configMINIMAL_STACK_SIZE (was 1024, rejected)
    #define STACK_SIZE_RELAY_CONTROL_TASK    1536  // After fix: needs margin for logging
    #define STACK_SIZE_SENSOR_TASK           1536  // >= configMINIMAL_STACK_SIZE (was 1024, rejected)
    #define STACK_SIZE_CONTROL_TASK          1536  // >= configMINIMAL_STACK_SIZE (was 1024, rejected)
    #define STACK_SIZE_WHEATER_CONTROL_TASK  2048  // After fix: needs margin for float ops in release
    #define STACK_SIZE_MQTT_TASK             1536  // After fix: MQTT operations
    #define STACK_SIZE_PERSISTENT_STORAGE_TASK 1536  // After fix: JSON operations
    #define STACK_SIZE_BURNER_CONTROL_TASK   1536  // After fix: needs margin for logging
    #define STACK_SIZE_MB8ART_PROCESSING_TASK 1536  // After fix: increased for logging
    #define STACK_SIZE_BOILER_TEMP_TASK      3072  // > the 2540 B peak measured in debug;
                                                   // release peaks are unmeasured
    #define STACK_SIZE_RYN4_PROCESSING_TASK  1536  // >= configMINIMAL_STACK_SIZE (was 768, rejected)
    #define STACK_SIZE_TIMER_SCHEDULER_TASK 1536  // Generic timer scheduler (replaces HotWaterScheduler)
    #define STACK_SIZE_LOOP_TASK             1536  // After fix: main loop minimal
    #define STACK_SIZE_PUMP_CONTROL_TASK     2560  // NOT "minimal": the measured pump peak is
                                                 // 1840 B (debug build), so 1536 was below the
                                                 // observed high-water mark. Release peaks are
                                                 // unmeasured - do not trim without measuring.
#endif

// NetworkMonitor is created with a raw xTaskCreate in NetworkInitializer (it runs
// before the TaskManager-supervised tasks and the network is explicitly non-critical),
// so it gets no watchdog registration and no restart supervision. Its size was an
// inline literal, invisible to the tiers and to the assertions below - the same blind
// spot as the pump stack. Not mode-tuned: this task only polls link state.
#define STACK_SIZE_NETWORK_MONITOR_TASK  2048

// ==========================
// LOG_MODE_RELEASE is NOT a usable configuration
// ==========================
// LOG_MODE_RELEASE IS THE DEFAULT (see the mode block above: an env that defines no
// log mode gets it), so this is not a dormant unused tier - it is what any new
// environment silently inherits. esp32_test was compiling against it until 2026-09-18.
//
// Measured 2026-09-18. The release stack tier above was never validated on hardware,
// and it is not merely untested - it is too small on most tasks, by two independent
// methods that agree:
//
// 1. Against the MEASURED high-water marks from the debug_selective image (8ee1446),
//    which are a GENEROUS lower bound for release: release removes only LOG_DEBUG and
//    LOG_VERBOSE, while LOG_INFO/WARN/ERROR remain and each costs ~1312 B of stack.
//    11 of 18 tasks have a release stack BELOW their already-measured peak -
//    MQTTTask by 900 B, PersistentStorage by 324, OTATask by 264, MB8ART by 260,
//    TimerSched by 252, BurnerControl by 232, RelayControl by 160, RYN4Proc by 152,
//    ANDRTF3 by 124, ControlTask by 96, HeatingControl by 68.
//
// 2. Against the static worst-case bounds from tools/stack_walker.py run on the
//    release ELF (--stale-excludes-ok --elf .pio/build/esp32dev_usb_release/firmware.elf):
//    13 of 18 exceed their stack, including BurnerControl (needs 3952, has 1536),
//    WheaterControl (4000 vs 2048), PersistentStorage (4256 vs 1536), MQTTTask
//    (3712 vs 1536), HeatingControl (3760 vs 1536) and RYN4Proc - the task that
//    writes the relays - at 2944 vs 1536.
//
// Sizing the tier properly needs roughly +18 kB of DRAM. The debug build measures
// about 5 kB free with a 4 kB largest block, so that memory does not exist: release
// mode's premise, that much smaller stacks are safe because there is less logging, is
// simply false here. The bounds are driven by the same call chains either way.
//
// So this is a hard stop rather than a comment. A release image would overflow the
// burner, relay and pump-adjacent task stacks on a live gas boiler. Until the tier is
// re-sized against measurements from a release build that has actually run, do not
// flash one. Define ALLOW_UNVALIDATED_RELEASE_STACKS to build anyway - for bench work
// on hardware that is not connected to a boiler, never for the real device.
#if defined(LOG_MODE_RELEASE) && !defined(ALLOW_UNVALIDATED_RELEASE_STACKS)
#error "LOG_MODE_RELEASE stacks are measurably too small (11/18 tasks below their measured debug peak; 13/18 above their static bound). See the comment in ProjectConfig.h. Define ALLOW_UNVALIDATED_RELEASE_STACKS for bench-only builds."
#endif

// ==========================
// Stack-size floor (compile-time)
// ==========================
// TaskManager::startTask()/startTaskPinned() REJECT any request below
// configMINIMAL_STACK_SIZE and return false (TaskManager.cpp:124, :217). Nothing
// aborts the boot on that path - TaskInitializer logs a LOG_WARN/LOG_ERROR and
// carries on - so an under-sized stack means the task SILENTLY DOES NOT EXIST.
//
// On this framework configMINIMAL_STACK_SIZE = CONFIG_FREERTOS_IDLE_TASK_STACKSIZE
// (1024) + configSTACK_OVERHEAD_TOTAL, and CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK
// is 1, contributing 60 -> 1084 B. The LOG_MODE_RELEASE tier above used to set
// eight macros below that line, five of them on tasks that are actually created:
// RYN4_PROCESSING (768, the task that WRITES THE RELAYS), CONTROL (1024, used
// twice), SENSOR (1024), MODBUS_CONTROL (1024) and OTA (1024). A release image
// would have booted with no relay writer, no control tasks, no sensor acquisition
// and no OTA, reporting only two log lines. Found by review 2026-09-18; no release
// env has been flashed.
//
// These assertions make that a BUILD failure instead of a silent runtime one.
// Never "fix" a failure here by lowering the floor - raise the stack.
static_assert(STACK_SIZE_OTA_TASK              >= configMINIMAL_STACK_SIZE, "STACK_SIZE_OTA_TASK below configMINIMAL_STACK_SIZE - task would be silently rejected");
static_assert(STACK_SIZE_MONITORING_TASK       >= configMINIMAL_STACK_SIZE, "STACK_SIZE_MONITORING_TASK below configMINIMAL_STACK_SIZE");
static_assert(STACK_SIZE_MODBUS_CONTROL_TASK   >= configMINIMAL_STACK_SIZE, "STACK_SIZE_MODBUS_CONTROL_TASK below configMINIMAL_STACK_SIZE");
static_assert(STACK_SIZE_RELAY_CONTROL_TASK    >= configMINIMAL_STACK_SIZE, "STACK_SIZE_RELAY_CONTROL_TASK below configMINIMAL_STACK_SIZE");
static_assert(STACK_SIZE_SENSOR_TASK           >= configMINIMAL_STACK_SIZE, "STACK_SIZE_SENSOR_TASK below configMINIMAL_STACK_SIZE");
static_assert(STACK_SIZE_CONTROL_TASK          >= configMINIMAL_STACK_SIZE, "STACK_SIZE_CONTROL_TASK below configMINIMAL_STACK_SIZE");
static_assert(STACK_SIZE_WHEATER_CONTROL_TASK  >= configMINIMAL_STACK_SIZE, "STACK_SIZE_WHEATER_CONTROL_TASK below configMINIMAL_STACK_SIZE");
static_assert(STACK_SIZE_MQTT_TASK             >= configMINIMAL_STACK_SIZE, "STACK_SIZE_MQTT_TASK below configMINIMAL_STACK_SIZE");
static_assert(STACK_SIZE_PERSISTENT_STORAGE_TASK >= configMINIMAL_STACK_SIZE, "STACK_SIZE_PERSISTENT_STORAGE_TASK below configMINIMAL_STACK_SIZE");
static_assert(STACK_SIZE_BURNER_CONTROL_TASK   >= configMINIMAL_STACK_SIZE, "STACK_SIZE_BURNER_CONTROL_TASK below configMINIMAL_STACK_SIZE");
static_assert(STACK_SIZE_MB8ART_PROCESSING_TASK >= configMINIMAL_STACK_SIZE, "STACK_SIZE_MB8ART_PROCESSING_TASK below configMINIMAL_STACK_SIZE");
static_assert(STACK_SIZE_BOILER_TEMP_TASK      >= configMINIMAL_STACK_SIZE, "STACK_SIZE_BOILER_TEMP_TASK below configMINIMAL_STACK_SIZE");
static_assert(STACK_SIZE_RYN4_PROCESSING_TASK  >= configMINIMAL_STACK_SIZE, "STACK_SIZE_RYN4_PROCESSING_TASK below configMINIMAL_STACK_SIZE");
static_assert(STACK_SIZE_TIMER_SCHEDULER_TASK  >= configMINIMAL_STACK_SIZE, "STACK_SIZE_TIMER_SCHEDULER_TASK below configMINIMAL_STACK_SIZE");
static_assert(STACK_SIZE_LOOP_TASK             >= configMINIMAL_STACK_SIZE, "STACK_SIZE_LOOP_TASK below configMINIMAL_STACK_SIZE");
static_assert(STACK_SIZE_PUMP_CONTROL_TASK     >= configMINIMAL_STACK_SIZE, "STACK_SIZE_PUMP_CONTROL_TASK below configMINIMAL_STACK_SIZE");
static_assert(STACK_SIZE_NETWORK_MONITOR_TASK  >= configMINIMAL_STACK_SIZE, "STACK_SIZE_NETWORK_MONITOR_TASK below configMINIMAL_STACK_SIZE");

// Task priorities (higher number = higher priority)
// Safety-critical tasks get priority 4 to ensure timely response to safety events
#define PRIORITY_OTA_TASK 1
#define PRIORITY_DEBUG_TASK 1
#define PRIORITY_MONITORING_TASK 2
#define PRIORITY_RELAY_STATUS_TASK 3
#define PRIORITY_RELAY_CONTROL_TASK 4
#define PRIORITY_MODBUS_STATUS_TASK 3
#define PRIORITY_MODBUS_CONTROL_TASK 4
#define PRIORITY_SENSOR_TASK 3
#define PRIORITY_CONTROL_TASK 3
#define PRIORITY_WHEATER_CONTROL_TASK 3
#define PRIORITY_MQTT_TASK 2
#define PRIORITY_PUMP_CONTROL_TASK 3
#define PRIORITY_BURNER_CONTROL_TASK 4  // Safety-critical: higher than other control tasks
#define PRIORITY_MB8ART_PROCESSING_TASK 3
#define PRIORITY_RYN4_PROCESSING_TASK 3

// Task Intervals - Optimized per mode
// NOTE: Several tasks have been converted to event-driven and no longer use polling intervals
#if defined(LOG_MODE_DEBUG_FULL)
    #define MONITORING_TASK_INTERVAL_MS        300000   // 5 minute - periodic health reports
    #define RESOURCE_LOG_PERIOD_MS             300000   // 5 minute

#elif defined(LOG_MODE_DEBUG_SELECTIVE)
    #define MONITORING_TASK_INTERVAL_MS        600000  // 10 minutes - periodic health reports
    #define RESOURCE_LOG_PERIOD_MS             600000  // 10 minutes

#else  // LOG_MODE_RELEASE
    #define MONITORING_TASK_INTERVAL_MS        540000  // 30 minutes - periodic health reports
    #define RESOURCE_LOG_PERIOD_MS             540000  // 30 minutes)
#endif

// Watchdog timeouts (in milliseconds)
#define WATCHDOG_TIMEOUT_SECONDS 30
#define WATCHDOG_MIN_HEAP_BYTES 10000

#define OTA_TASK_WATCHDOG_TIMEOUT_MS (OTA_TASK_INTERVAL_MS * 4 + 5000)
#define MONITORING_TASK_WATCHDOG_TIMEOUT_MS (MONITORING_TASK_INTERVAL_MS + 5000)
#define RELAY_CONTROL_TASK_WATCHDOG_TIMEOUT_MS (RELAY_CONTROL_TASK_INTERVAL_MS * 4 + 5000)
#define RELAY_STATUS_TASK_WATCHDOG_TIMEOUT_MS (RELAY_STATUS_TASK_INTERVAL_MS * 2 + 5000)
#define MODBUS_CONTROL_TASK_WATCHDOG_TIMEOUT_MS 30000  // 30 seconds - must be > MB8ART_CONTROL_TASK_TIMEOUT_MS
#define MODBUS_STATUS_TASK_WATCHDOG_TIMEOUT_MS 30000   // 30 seconds - safe margin for 5s interval
#define MQTT_TASK_WATCHDOG_TIMEOUT_MS 30000
#define CONTROL_TASK_WATCHDOG_TIMEOUT_MS 15000  // 15 seconds for control tasks

#if defined(LOG_MODE_DEBUG_FULL)
    #define DEBUG_TASK_WATCHDOG_TIMEOUT_MS      30000   // 30 seconds
#elif defined(LOG_MODE_DEBUG_SELECTIVE)
    #define DEBUG_TASK_WATCHDOG_TIMEOUT_MS      45000   // 45 seconds
#else  // LOG_MODE_RELEASE
    #define DEBUG_TASK_WATCHDOG_TIMEOUT_MS      60000   // 60 seconds
#endif

// ==========================
// Log Tags
// ==========================
// LOG_TAG_* defines removed - using modern C++ pattern: static const char* TAG = "ModuleName";
// Each module now defines its own TAG constant at file scope.

// Optional: Debug mode specific buffer sizes
#if defined(LOG_MODE_DEBUG_FULL)
    #define MODBUS_LOG_BUFFER_SIZE 512
    #define STATUS_LOG_BUFFER_SIZE 512
    #define EVENT_LOG_BUFFER_SIZE 512
#else
    #define MODBUS_LOG_BUFFER_SIZE 384  // Reduced from 512
    #define STATUS_LOG_BUFFER_SIZE 384  // Reduced from 512
    #define EVENT_LOG_BUFFER_SIZE 256
#endif

// ==========================
// Logging Macros
// ==========================
// Use LoggingMacros.h which handles conditional Logger usage
#include "LoggingMacros.h"

// ==========================
// Sensor Diagnostics Configuration
// ==========================
#ifndef SENSOR_DIAGNOSTICS_INTERVAL_MS
    #if defined(LOG_MODE_DEBUG_FULL)
        #define SENSOR_DIAGNOSTICS_INTERVAL_MS 300000    // 5 minutes in debug
    #elif defined(LOG_MODE_DEBUG_SELECTIVE)
        #define SENSOR_DIAGNOSTICS_INTERVAL_MS 600000    // 10 minutes
    #else
        #define SENSOR_DIAGNOSTICS_INTERVAL_MS 1800000   // 30 minutes in release
    #endif
#endif

#ifndef SENSOR_DIAGNOSTICS_ON_ERROR_THRESHOLD
    #define SENSOR_DIAGNOSTICS_ON_ERROR_THRESHOLD 5.0f  // 5% failure rate triggers diagnostics
#endif

#ifndef SENSOR_ERROR_DIAGNOSTICS_COOLDOWN_MS
    #define SENSOR_ERROR_DIAGNOSTICS_COOLDOWN_MS 3600000  // 1 hour cooldown between error-triggered diagnostics
#endif

// ==========================
// MB8ART Configuration
// ==========================
#define PROJECT_MB8ART_MIN_REQUEST_INTERVAL_MS 10    // Minimum 10ms between requests (was 50ms before parity fix)
#define PROJECT_MB8ART_REQUEST_TIMEOUT_MS 500        // 500ms timeout for Modbus requests
#define PROJECT_MB8ART_INTER_REQUEST_DELAY_MS MODBUS_INTER_FRAME_DELAY_MS  // Dynamic 3.5 char time
#define PROJECT_MB8ART_RETRY_COUNT 3                 // 3 retries for failed requests
#define MB8ART_CACHE_VALIDITY_MS 1800       // 1.8 seconds for 2s polling
#define MB8ART_TEMPERATURE_PRECISION 2       // 2 decimal places for display

// ==========================
// RYN4 Configuration
// ==========================
#define RYN4_NUM_RELAYS 8                     // Number of relays
#define DEFAULT_RELAY_STATE false             // false = OFF/CLOSED, true = ON/OPEN
#define RYN4_MIN_REQUEST_INTERVAL_MS 10       // Minimum 10ms between requests (was 50ms before parity fix)
#define RYN4_REQUEST_TIMEOUT_MS 500           // 500ms timeout for Modbus requests
#define RYN4_RESPONSE_TIMEOUT_MS 1000         // 1s timeout for RYN4 responses
#define RYN4_INTER_REQUEST_DELAY_MS MODBUS_INTER_FRAME_DELAY_MS  // Dynamic 3.5 char time
#define RYN4_INTER_COMMAND_DELAY_MS 50        // 50ms delay between consecutive commands (restored - bus stability)
#define RYN4_RETRY_COUNT 3                    // 3 retries for failed requests

// Relay operation safety limits
#define MIN_RELAY_SWITCH_INTERVAL_MS 150      // Minimum time between relay toggles
#define MAX_RELAY_TOGGLE_RATE_PER_MIN 30      // Maximum toggles per minute per relay

// Optional relay features
#define ENABLE_RELAY_SAFETY_CHECKS            // Comment out to disable safety checks
#define ENABLE_RELAY_EVENT_LOGGING            // Comment out to disable event logging

// ==========================
// BLE Configuration (Removed)
// ==========================
// BLE functionality has been removed from this project.
// Inside temperature is now provided by MB8ART channel 7.

// Temperature source selection
#define USE_BLE_FOR_INSIDE_TEMP 0       // Always 0 - using MB8ART channel 7

// BLE sensor task is permanently disabled
#define ENABLE_BLE_SENSOR_TASK 0        // Disabled - using MB8ART channel 7 for inside temp

// Temperature/Humidity validation ranges (still used for MB8ART)
#define TEMPERATURE_MIN_VALID -40.0f
#define TEMPERATURE_MAX_VALID 100.0f
#define HUMIDITY_MIN_VALID 0.0f
#define HUMIDITY_MAX_VALID 100.0f

// ==========================
// Temperature Thresholds
// ==========================
#define TEMP_THRESHOLD_HIGH_WARNING 80.0f
#define TEMP_THRESHOLD_HIGH_CRITICAL 90.0f
#define TEMP_THRESHOLD_LOW_WARNING 5.0f
#define TEMP_THRESHOLD_LOW_CRITICAL 0.0f

// ==========================
// System Configuration
// ==========================
#define ENABLE_SENSOR_EVENT_LOGGING
#define ENABLE_SYSTEM_STATE_LOGGING

// Buffer sizes
#define SERIAL_BUFFER_SIZE 256
#define LOG_BUFFER_SIZE 384    // Reduced from 512
#define MQTT_BUFFER_SIZE 768   // Reduced from 1024
// CONFIG_LOG_BUFFER_SIZE is defined in DebugMacros.h

// ==========================
// Debug Options
// ==========================
#ifdef LOG_MODE_DEBUG_FULL
    #define DEBUG_HEAP_MONITORING
    #define DEBUG_TASK_STATISTICS
    #define DEBUG_MODBUS_COMMUNICATION
    #define DEBUG_EVENT_GROUPS
#endif

#ifdef LOG_MODE_DEBUG_SELECTIVE
    #define DEBUG_HEAP_MONITORING
    #define DEBUG_TASK_STATISTICS
#endif

// ==========================
// Enable Features
// ==========================
#define ENABLE_MONITORING_TASK 1

// ==========================
// Shared Resources
// ==========================
// External shared resources declarations
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

// Access these resources through SystemResourceProvider (SRP):
// - SRP::getSensorReadingsMutex()
// - SRP::getRelayReadingsMutex()
// - SRP::getSensorEventGroup()
// - SRP::getRelayEventGroup()

// ==========================
// Optional Test Modes
// ==========================
// Relay test mode - uncomment to enable automatic relay testing
// #define RELAY_TEST_MODE
#ifdef RELAY_TEST_MODE
    #define RELAY_TEST_INTERVAL_MS 5000         // Toggle relay every 5 seconds
#endif

// Memory leak test - uncomment to enable memory leak testing
// #define ENABLE_MEMORY_LEAK_TEST
#ifdef ENABLE_MEMORY_LEAK_TEST
    #define MEMORY_LEAK_TEST_DELAY_MS 150000    // Run test after 2.5 minutes
#endif