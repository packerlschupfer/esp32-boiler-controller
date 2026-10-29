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
//
// 2026-10-18: NO LONGER DEFINED. It had been defined unconditionally since the initial
// release - two lines under its own warning, tagged "TODO: Remove before production
// deployment", with the #warning that would have surfaced it at every build commented out.
// Effect while defined: BurnerSafetyValidator.cpp:37 set allowMissingPressure = true and
// BurnerSafetyRules.h:133 skipped Check::PRESSURE_MISSING, so a 4-20 mA loop break or any
// invalid reading left the burner running with one LOG_WARN per validation. Combined with
// the inert pressure event bits (Burner::ERROR_PRESSURE is set by MB8ARTTasks and read by
// nobody; processSafetyEvent waits on four bits nothing sets), every signal this firmware
// raises on a pressure-sensor failure was inert.
//
// What changes now: an invalid pressure reading blocks the burner for that validation.
// It does NOT latch - validateBurnerOperation runs per cycle and heatDemand is recomputed
// each time, so a transient Modbus hiccup costs a cycle or two of firing, not a lockout
// needing a reset. The in-range value checks (PRESSURE_LOW / PRESSURE_HIGH) were always
// live and are unaffected.
//
// Re-enable ONLY for bench work with no sensor fitted, and note the #warning below is
// active again so the build will tell you.
// #define ALLOW_NO_PRESSURE_SENSOR
#ifdef ALLOW_NO_PRESSURE_SENSOR
    #warning "ALLOW_NO_PRESSURE_SENSOR is enabled - burner can operate without pressure sensor. Remove for production!"
#endif

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
    #define STACK_SIZE_OTA_TASK              4096  // MEASURED 2026-09-18: 4096, not 3072. An OTA *auth exchange alone* took OTATask from
                                                   // 2144 B free to 384 B - below LOW_STACK_WARN_BYTES (512) - i.e. 2688 B used on a
                                                   // 3072 B stack, and the successful-transfer branch (_runUpdate -> client.printf ->
                                                   // Update.write) is deeper still and was NOT measured. The depth is Arduino-core
                                                   // (ArduinoOTAClass::_onRx has an 880 B frame and calls Print::printf -> _svfprintf_r),
                                                   // so it does NOT shrink with log mode: every tier needs this, which is why the
                                                   // DEBUG_FULL (was 2048) and RELEASE (was 1536) tiers were below measured usage too.
                                                   // Funded by the ~5.5 kB of DRAM that LOG_DEFERRED_FORMAT freed. Re-check with
                                                   // 'boiler/cmd/diagnostics' payload 'tasks' after any OTA.
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
    #define STACK_SIZE_NTP_TASK              3584  // DEBUG_FULL: more logging than SELECTIVE
    #define STACK_SIZE_RYN4_PROCESSING_TASK  1536  // After fix: relay processing
    #define STACK_SIZE_TIMER_SCHEDULER_TASK 3072  // Generic timer scheduler (replaces HotWaterScheduler)
    #define STACK_SIZE_LOOP_TASK             3072  // After fix: main loop with debug
    #define STACK_SIZE_PUMP_CONTROL_TASK     2560  // After fix: pump control with logging

#elif defined(LOG_MODE_DEBUG_SELECTIVE)
    // DEBUG SELECTIVE MODE - Optimized based on runtime profiling (Dec 2025)
    // H3: Safety-critical tasks increased by +512 bytes for margin
    //
    // 2026-10-16: eight macros raised by +512 B each so that EVERY task clears its
    // static worst-case bound by at least LOW_STACK_WARN_BYTES (512). Before this,
    // tools/stack_walker.py on the prod_debug_selective ELF put eight tasks below that
    // line and four of them under 115 B - less than one ordinary function frame:
    //
    //   task                 bound   was    margin      now    margin
    //   WheaterControl        3520   3584       64     4096       576
    //   Monitoring            3504   3584       80     4096       592
    //   HeatingPump           2480   2560       80     3072       592
    //   WaterPump             2480   2560       80     3072       592
    //   PumpControl           2448   2560      112     3072       624
    //   TimerSched            2832   3072      240     3584       752
    //   MQTTTask              3312   3584      272     4096       784
    //   HeatingControl        3280   3584      304     4096       816
    //   RYN4Proc              2752   3072      320     3584       832
    //   BoilerTempCtrl        3232   3584      352     4096       864
    //
    // None of this is new information: the comment below on the release tier already
    // recorded WheaterControl at ~3520 and HeatingControl at ~3280, and
    // TaskInitializer.cpp:350 records the pumps wanting the profiled 3072 and settling
    // for 2560. Every one of those deferrals gave the SAME reason - the DRAM did not
    // exist ("+1024 B total, and it is NOT funded by anything"; "the device measures
    // ~5 kB free"). One of them even tried to fund itself by trimming SyslogTask and
    // withdrew because that task self-deletes when syslog is disabled.
    //
    // What changed: syslog was switched OFF in NVS on 2026-10-16 and the reboot
    // reclaimed ~11 kB - malloc DRAM 3.2 -> 14.5 kB free, largest block 1.65 -> 12.9 kB
    // (boiler/cmd/diagnostics memory). This batch spends 5632 B of that, so the
    // deferrals are now funded rather than deferred again.
    //
    // Note these are STATIC bounds, not measured peaks. Measured peaks sit 900-1900 B
    // lower (MQTTTask 2400 B used, bound 3312). The bound is still the right criterion:
    // it is what the release and DEBUG_FULL gates below are written against, and the
    // deep branches it includes - queue-overflow and backpressure logging - were
    // observed firing on 2026-10-16, so they are live paths, not theory.
    //
    // Measured cost after this change: see the git commit. Re-check with
    // 'boiler/cmd/diagnostics' payload 'tasks' and 'memory'.
    #define STACK_SIZE_OTA_TASK              4096  // MEASURED 2026-09-18: 4096, not 3072. An OTA *auth exchange alone* took OTATask from
                                                   // 2144 B free to 384 B - below LOW_STACK_WARN_BYTES (512) - i.e. 2688 B used on a
                                                   // 3072 B stack, and the successful-transfer branch (_runUpdate -> client.printf ->
                                                   // Update.write) is deeper still and was NOT measured. The depth is Arduino-core
                                                   // (ArduinoOTAClass::_onRx has an 880 B frame and calls Print::printf -> _svfprintf_r),
                                                   // so it does NOT shrink with log mode: every tier needs this, which is why the
                                                   // DEBUG_FULL (was 2048) and RELEASE (was 1536) tiers were below measured usage too.
                                                   // Funded by the ~5.5 kB of DRAM that LOG_DEFERRED_FORMAT freed. Re-check with
                                                   // 'boiler/cmd/diagnostics' payload 'tasks' after any OTA.
    #define STACK_SIZE_MONITORING_TASK       4096  // 2026-10-16: was 3584, bound 3504 -> was 80 B margin  // Dec 2025: was 4096, had 2232 free
    #define STACK_SIZE_MODBUS_CONTROL_TASK   3584  // Increased - was showing only 448 bytes free
    #define STACK_SIZE_RELAY_CONTROL_TASK    4096  // H3: Safety-critical +512 (was 3584)
    #define STACK_SIZE_SENSOR_TASK           3584  // Dec 2025: was 4096, ANDRTF3 had 2272 free
    #define STACK_SIZE_CONTROL_TASK          4096  // 2026-10-16: was 3584, bound 3280 (HeatingControl shares this) -> was 304 B  // H3: Safety-critical +512 (was 3072)
    #define STACK_SIZE_WHEATER_CONTROL_TASK  4096  // 2026-10-16: was 3584, bound 3520 -> was 64 B, the tightest in the system  // Dec 2025: was 4096, had 2344 free
    #define STACK_SIZE_MQTT_TASK             4096  // 2026-10-16: was 3584, bound 3312 -> was 272 B  // Dec 2025: was 3072, had 712 free
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
    #define STACK_SIZE_NTP_TASK              3584  // 2026-09-18: was a hardcoded 4096 in main.cpp.
                                                   // Trimmed to FUND the RYN4Proc raise, which had
                                                   // pushed heap_dram_blk to 2548 - just under
                                                   // MIN_DRAM_BLOCK_WARNING (2560). Safe to trim
                                                   // precisely because this bound is CONFIRMED:
                                                   // measured peak 2784 vs static bound 2816, a
                                                   // 32 B gap, so there is no hidden depth here -
                                                   // the opposite of RYN4Proc, whose deep path had
                                                   // never run.
                                                   //
                                                   // FIRST TRIED 3072 AND THAT WAS TOO FAR. It
                                                   // respects the bound (256 B above it) but the
                                                   // device then reported 360 B FREE, under
                                                   // LOW_STACK_WARN_BYTES (512), so the live
                                                   // firmware raised a low-stack warning - a real
                                                   // one, on a task I had just trimmed. Sizing to
                                                   // the static bound alone is not enough when a
                                                   // warning threshold sits above the result.
                                                   // 3584 leaves 768 B above the bound and ~870 B
                                                   // free in practice. Net vs the RYN4Proc raise is
                                                   // now zero, not -512 B.
                                                   // NTPTask is not safety-critical and its failure
                                                   // path is "no time sync", not "no heat".
                                                   //
                                                   // 2026-10-30: the "no hidden depth" claim above is
                                                   // NO LONGER TRUE, and it was the walker that was
                                                   // wrong, not the device. performNTPSync() ends a sync
                                                   // with rtcUpdateCallback() - an indirect call through
                                                   // a runtime-assigned pointer - so updateRTCFromNTP and
                                                   // the DS3231 I2C tail were invisible to the tool and
                                                   // its bound read 2340-2352 B. Hardware measures a
                                                   // 2788 B peak. tools/stack_walker.excludes now carries
                                                   // that edge and the bound reads 2756-2768 B. The tool
                                                   // STILL reports BOUND VIOLATED, by 20 B rather than
                                                   // 436: Adafruit_I2CDevice::write/_read dispatch the
                                                   // I2C transport through true indirects whose frames
                                                   // count as 0. Left as-is deliberately at a 796 B
                                                   // margin - see tools/stack_walker.md. The branch only went live in 12aab50
                                                   // (the callback registration used to be discarded), so
                                                   // the 2026-09-18 figures were honest when measured.
                                                   // Real margin is 796 B free - 284 B above
                                                   // LOW_STACK_WARN_BYTES. Do NOT trim this again: at
                                                   // 3072 the bound alone would fit and the device would
                                                   // still warn, exactly as it did in September.
    #define STACK_SIZE_BOILER_TEMP_TASK      4096  // 2026-10-16: was 3584, bound 3232 -> was 352 B  // 2026-09-17: was 3072 with 644 B free;
                                                   // raised for the autotune-completion branch
                                                   // (BoilerTempControlTask.cpp snprintfs three
                                                   // %.4f into char[160]), which no measurement
                                                   // has ever exercised. Measured peak 2540 B.
    #define STACK_SIZE_RYN4_PROCESSING_TASK  3584  // 2026-10-16: was 3072, bound 2752 -> was 320 B; writes the relays  // 2026-09-18: was 2560, which left only
                                                 // 96 B above the static bound (2464). This task
                                                 // WRITES THE RELAYS and its deepest runtime path
                                                 // is an emergency chain that has never executed:
                                                 // handleReadTick -> checkRelayHealthAndEscalate
                                                 // -> triggerFailsafe -> executeFailsafeActions
                                                 // -> emergencyStop -> emergencyShutdown. Measured
                                                 // peak is only 1640 B, so none of that depth is
                                                 // empirically confirmed. The decisive point: an
                                                 // UNRESOLVED INDIRECT on this very path was hiding
                                                 // ~1.9 kB of failsafe-callback depth until it was
                                                 // modelled by hand (see the edge: entries in
                                                 // tools/stack_walker.excludes), and 69 unresolved
                                                 // indirects remain on this task. 96 B cannot
                                                 // absorb one more such discovery, nor a single
                                                 // added frame on that chain. Costs +512 B DRAM.
    #define STACK_SIZE_TIMER_SCHEDULER_TASK 3584  // 2026-10-16: was 3072, bound 2832 -> was 240 B  // Generic timer scheduler (replaces HotWaterScheduler)
    #define STACK_SIZE_LOOP_TASK             4096  // After fix: needs more for initialization
    #define STACK_SIZE_PUMP_CONTROL_TASK     3072  // 2026-10-16: was 2560, bound 2480 x3 tasks -> was 80 B; the profiled 3072 deferred in TaskInitializer.cpp:350  // 2026-09-17 MEASURED: 2560 gives 928 B free
                                                 // (peak 1840 B, static from boot, verified at 11.2 h
                                                 // through a full water cycle). Was 3072 aspirationally,
                                                 // but TaskInitializer shadowed this macro with a local
                                                 // 2048 so the pumps never saw either value. 2560 is what
                                                 // is actually validated; the extra 512 B/task is not
                                                 // funded against a 1812 B DRAM floor.

#else  // LOG_MODE_RELEASE
    // RELEASE MODE - Aggressive optimization with SafeLog
    #define STACK_SIZE_OTA_TASK              4096  // MEASURED 2026-09-18: 4096, not 3072. An OTA *auth exchange alone* took OTATask from
                                                   // 2144 B free to 384 B - below LOW_STACK_WARN_BYTES (512) - i.e. 2688 B used on a
                                                   // 3072 B stack, and the successful-transfer branch (_runUpdate -> client.printf ->
                                                   // Update.write) is deeper still and was NOT measured. The depth is Arduino-core
                                                   // (ArduinoOTAClass::_onRx has an 880 B frame and calls Print::printf -> _svfprintf_r),
                                                   // so it does NOT shrink with log mode: every tier needs this, which is why the
                                                   // DEBUG_FULL (was 2048) and RELEASE (was 1536) tiers were below measured usage too.
                                                   // Funded by the ~5.5 kB of DRAM that LOG_DEFERRED_FORMAT freed. Re-check with
                                                   // 'boiler/cmd/diagnostics' payload 'tasks' after any OTA.
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
    #define STACK_SIZE_NTP_TASK              3072  // as DEBUG_SELECTIVE; bound is log-mode-insensitive
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
// spot as the pump stack.
//
// 2026-10-18: raised 2048 -> 3072 because 2048 was BELOW its static bound. The old
// comment here said "this task only polls link state", and that is what hid the
// problem: on link-up it also calls EthernetManager::logEthernetStatus(), whose
// NetworkInterface::macAddress() builds an Arduino String through snprintf ->
// _svfprintf_r (an 800 B frame) -> _malloc_r. Measured with
//   tools/stack_walker.py --no-task-manager-wrapper "NetworkInitializer::networkMonitorTask(void*)"
// (no TaskManager wrapper frame, because xTaskCreate is raw):
//   prod_debug_selective          chain 1744 B -> used 2068-2080 B   over 2048 by 20-32 B
//   esp32dev_usb_debug_selective  chain 1568 B -> used 1892-1904 B   only 144-156 B spare
// The 800 B frame is the framework's own String build, so LOG_DEFERRED_FORMAT cannot
// remove it; that is also why the two tiers differ by 176 B rather than more.
//
// Why no high-water mark ever caught this: the deep path runs ONCE, at link-up, and the
// task then calls vTaskDelete(NULL) (NetworkInitializer.cpp:135). It is therefore absent
// from the `tasks` diagnostic dump by the time anyone reads it - confirmed, 0 occurrences
// in the 2026-10-18 dump - so no peak for it has ever been recorded in any run. The
// e3f230a sweep that cleared "every task" by 512 B enumerated the dump, and so missed
// this one. Enumerate xTaskCreate call sites, not the dump.
//
// 3072 gives 992-1004 B over the prod bound and 1168-1180 B over the usb one. The cost is
// transient: the task deletes itself at link-up, so the extra 1024 B returns to the heap
// and steady-state DRAM is unchanged.
#define STACK_SIZE_NETWORK_MONITOR_TASK  3072

// SyslogTask and ModbusInit: the last two task stacks that were inline literals, moved
// here 2026-09-18 so they are visible to the tier system and covered by the
// configMINIMAL_STACK_SIZE assertions below like everything else. Neither is mode-tuned:
// both are short-lived boot-path tasks whose work does not change with the log level.
//
// SYSLOG: do NOT trim this to fund another task. It was briefly cut to 3072 on 2026-09-17
// on a false premise - the "2064 B spare" figure belonged to LogSub, a DIFFERENT task.
// SyslogTask does not appear in a steady-state dump because SYSLOG_ENABLED_DEFAULT is
// false and it eventually self-deletes, but it does NOT self-delete promptly: it blocks on
// xEventGroupWaitBits(NETWORK_READY | STORAGE_READY, 30000) first, so it holds all 4096 B
// across boot - and it is created BEFORE the pumps and MonitoringTask. Kept at 4096
// because syslog_enabled is runtime-settable (post-reboot) and the task is then
// persistent, needing EthernetUDP plus Syslog::sendInternal's 1088 B frame.
#define STACK_SIZE_SYSLOG_TASK           4096
#define STACK_SIZE_MODBUS_INIT_TASK      4096

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
// LOG_MODE_DEBUG_FULL IS IN THE SAME STATE, and it is FLASHABLE at the boiler.
// Found 2026-09-18 by a review of the commit that added the release gate below: gating
// release alone was half a fix. DEBUG_FULL logs strictly MORE than DEBUG_SELECTIVE, so
// it cannot need less stack - yet NINE of seventeen macros are smaller:
//
//   macro                    DEBUG_FULL   SELECTIVE   static bound
//   RYN4_PROCESSING              1536        3072      ~2464   <- writes the relays
//   MODBUS_CONTROL               1536        3584
//   SENSOR                       2048        3584
//   CONTROL                      2048        3584      ~3280 (HeatingControl)
//   RELAY_CONTROL                2560        4096
//   OTA                          2048        3072
//   MQTT                         3072        3584
//   WHEATER_CONTROL              3072        3584      ~3520
//   LOOP                         3072        4096
//
// tools/stack_walker.py on a DEBUG_FULL build reports "STATIC BOUND EXCEEDS STACK even
// without float formatting" for RYN4ProcessingTask and HeatingControlTask. Bringing the
// tier up to mere parity with SELECTIVE costs +11264 B of DRAM; the device measures
// ~5 kB free. So, exactly as with release, this tier cannot be made safe within the
// budget and must not be flashed.
//
// This one is worse than release in one respect: esp32dev_ota_debug_full sets
// upload_protocol = espota with upload_port = 192.168.20.40, the boiler's own address.
// A single `pio run -e esp32dev_ota_debug_full -t upload` would put a 1536 B
// RYN4ProcessingTask on the live device over the air.
//
// Note for whoever reads this next: "builds with 0 warnings" is NOT validation. Every
// commit in the 2026-09-18 batch cited a clean debug_full build as evidence while this
// was true of it.
#if defined(LOG_MODE_DEBUG_FULL) && !defined(ALLOW_UNVALIDATED_DEBUG_FULL_STACKS)
#error "LOG_MODE_DEBUG_FULL stacks are measurably too small: 9 of 17 are BELOW the DEBUG_SELECTIVE values despite logging more, and RYN4ProcessingTask (1536) / HeatingControlTask (2048) exceed their static bounds. See the comment in ProjectConfig.h. Define ALLOW_UNVALIDATED_DEBUG_FULL_STACKS for bench-only builds."
#endif

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
static_assert(STACK_SIZE_NTP_TASK              >= configMINIMAL_STACK_SIZE, "STACK_SIZE_NTP_TASK below configMINIMAL_STACK_SIZE");
static_assert(STACK_SIZE_RYN4_PROCESSING_TASK  >= configMINIMAL_STACK_SIZE, "STACK_SIZE_RYN4_PROCESSING_TASK below configMINIMAL_STACK_SIZE");
static_assert(STACK_SIZE_TIMER_SCHEDULER_TASK  >= configMINIMAL_STACK_SIZE, "STACK_SIZE_TIMER_SCHEDULER_TASK below configMINIMAL_STACK_SIZE");
static_assert(STACK_SIZE_LOOP_TASK             >= configMINIMAL_STACK_SIZE, "STACK_SIZE_LOOP_TASK below configMINIMAL_STACK_SIZE");
static_assert(STACK_SIZE_PUMP_CONTROL_TASK     >= configMINIMAL_STACK_SIZE, "STACK_SIZE_PUMP_CONTROL_TASK below configMINIMAL_STACK_SIZE");
static_assert(STACK_SIZE_NETWORK_MONITOR_TASK  >= configMINIMAL_STACK_SIZE, "STACK_SIZE_NETWORK_MONITOR_TASK below configMINIMAL_STACK_SIZE");
static_assert(STACK_SIZE_SYSLOG_TASK           >= configMINIMAL_STACK_SIZE, "STACK_SIZE_SYSLOG_TASK below configMINIMAL_STACK_SIZE");
static_assert(STACK_SIZE_MODBUS_INIT_TASK      >= configMINIMAL_STACK_SIZE, "STACK_SIZE_MODBUS_INIT_TASK below configMINIMAL_STACK_SIZE");

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