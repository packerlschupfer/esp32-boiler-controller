# Improvement Plan — Round 22

Generated: 2026-02-25
Codebase: esp32-boiler-controller (branch: work, after Round 21)

## Summary

Three independent static analysis passes identified 5 critical bugs, 7 high-severity issues, and 12 medium/low items across the ESP32 boiler controller codebase. The most urgent findings are a wrong MQTT topic prefix causing dead messages in two safety-related code paths, an NVS parameter sync bug causing room setpoint loss on reboot, and a gap in the DEGRADED failsafe level that silently drops subsystem shutdown callbacks. Additionally, 6KB of dead memory pools and two never-called safety infrastructure modules (FailOpenMonitor, ErrorContextCapture) represent wasted RAM and false confidence in error reporting.

---

## Critical — Fix Before Next Deployment

### C1. Wrong MQTT Topic "status/boiler/burner" in Two Locations

**Files**:
- `src/modules/control/BurnerStateMachine.cpp:464`
- `src/modules/mqtt/MQTTCommandHandlers.cpp:924`

**Problem**: Both locations publish to `"status/boiler/burner"` instead of `"boiler/status/burner"`. The topic prefix is inverted. Burner error state messages (from BurnerStateMachine) and lockout reset confirmations (from MQTTCommandHandlers) are published to a topic that no subscriber monitors. Operators receive no notification of burner errors or lockout resets.

**Fix**: Replace `"status/boiler/burner"` with `"boiler/status/burner"` in both files. Consider using the `MQTT_STATUS_PREFIX "/burner"` macro from `MQTTTopics.h` instead of a hardcoded string to prevent future drift.

---

### C2. targetTemp_i32 and hysteresis_i32 Not Synced After loadAll()

**File**: `src/modules/tasks/PersistentStorageTask.cpp:194-267` (registration) and `508-528` (post-load sync block)

**Problem**: After `storage->loadAll(true)` at line 494, the post-load sync block (lines 508-528) explicitly syncs `burner_low_i32`, `burner_high_i32`, `heating_low_i32`, `heating_high_i32`, `water_low_i32`, `water_high_i32`, `outsideHeatingThreshold_i32`, `roomOverheatMargin_i32`, `tankLow_i32`, `tankHigh_i32`, `tankSafeHigh_i32`, and `tankSafeLow_i32` back to SystemSettings. However, `targetTemp_i32` (line 194, maps to `settings.targetTemperatureInside`) and `hysteresis_i32` (line 263, maps to `settings.heating_hysteresis`) are **not** synced after loadAll(). Their `setOnChange` callbacks only fire on MQTT-triggered changes, not on initial NVS load. This is the exact same bug pattern that was just fixed for `wHeaterConfTempLimitHigh` (visible at lines 530-536). Result: room temperature setpoint and heating hysteresis from NVS are silently ignored on every reboot — the system falls back to struct defaults.

**Fix**: Add explicit sync lines after line 528:
```cpp
settings.targetTemperatureInside = static_cast<Temperature_t>(targetTemp_i32);
settings.heating_hysteresis = static_cast<Temperature_t>(hysteresis_i32);
```
Also add zero-value default restoration (same pattern as lines 510-515) if `loadAll()` may zero missing keys.

---

### C3. CentralizedFailsafe DEGRADED Level Doesn't Execute Subsystem Callbacks

**File**: `src/modules/control/CentralizedFailsafe.cpp:153-196`

**Problem**: In `executeFailsafeActions()`, the subsystem callback loop at line 158 has the condition:
```cpp
if (callback.first == Subsystem::ALL || level >= FailsafeLevel::CRITICAL)
```
This means callbacks only execute for `Subsystem::ALL` registrations or when level is CRITICAL or above. Subsystem-specific callbacks registered for DEGRADED level are **never invoked**. The DEGRADED case (line 175) only logs `"System operating in degraded mode"` — it does not call `defaultBurnerFailsafe(level)` and does not execute subsystem-specific shutdown logic. Relay failures trigger DEGRADED (line 439), but the burner keeps running.

**Fix**: Change the callback filter to also execute for DEGRADED:
```cpp
if (callback.first == Subsystem::ALL || level >= FailsafeLevel::DEGRADED)
```
Or add `defaultBurnerFailsafe(level)` to the DEGRADED case in the switch block, similar to how CRITICAL calls it at line 183. The appropriate fix depends on whether DEGRADED should reduce power or shut down the burner — document the design decision.

---

### C4. FailOpenMonitor and ErrorContextCapture Never Called — 6KB Dead Memory Pools

**Files**:
- `src/utils/FailOpenMonitor.cpp:37` — `recordFailOpen()` has zero call sites
- `src/utils/FailOpenMonitor.cpp:86` — `publishDegradedStatus()` has zero call sites
- `src/utils/ErrorContextCapture.cpp:23` — `captureSnapshot()` has zero call sites
- `src/utils/MemoryPool.cpp:14-17` — 4 pools (`diagnosticBufferPool`, `configBufferPool`, `calcBufferPool`, `errorBufferPool`) totaling 6KB have zero `allocate()` calls

**Problem**: Round 21 created FailOpenMonitor and ErrorContextCapture as infrastructure for fail-open tracking and error diagnostics, plus 4 new memory pools to support them. None of these are actually wired into the codebase. All 4 fail-open code paths (water flow assumption, pressure assumption, hardware interlock bypass, flame sensor proxy) never call `recordFailOpen()`. Critical error paths (ignition failure, emergency stop) produce no diagnostic snapshots. The 6KB of pool memory is allocated at startup but never used.

**Fix**: Either integrate or remove:
- **Integrate**: Add `FailOpenMonitor::recordFailOpen()` calls at each fail-open assumption site. Add `ErrorContextCapture::captureSnapshot()` calls at ignition failure, emergency stop, and lockout entry. Add `FailOpenMonitor::publishDegradedStatus()` to MonitoringTask's periodic loop. Use the memory pools in the error/diagnostic code paths.
- **Remove**: If integration is deferred, remove the 4 unused pools from `MemoryPool.cpp:14-17` and `MemoryPool.h:234-252` to reclaim 6KB RAM. Keep FailOpenMonitor and ErrorContextCapture source files but document them as unintegrated.

---

### C5. ErrorContextCapture Uses Hardcoded Bit Positions Instead of SystemEvents Constants

**File**: `src/utils/ErrorContextCapture.cpp:77-79`

**Problem**: The snapshot capture code uses:
```cpp
snapshot.burnerActive = (snapshot.systemStateBits & (1 << 0)) != 0;
snapshot.heatingActive = (snapshot.systemStateBits & (1 << 1)) != 0;
snapshot.waterActive = (snapshot.systemStateBits & (1 << 2)) != 0;
```
The actual SystemState event bit definitions (from `include/events/SystemEventsGenerated.h:30-35`) are:
- `BOILER_ENABLED = (1UL << 0UL)` — bit 0
- `HEATING_ENABLED = (1UL << 2UL)` — bit 2
- `HEATING_ON = (1UL << 3UL)` — bit 3
- `WATER_ENABLED = (1UL << 4UL)` — bit 4
- `WATER_ON = (1UL << 5UL)` — bit 5

So `heatingActive` reads bit 1 (undefined/unused) instead of bit 3 (`HEATING_ON`), and `waterActive` reads bit 2 (`HEATING_ENABLED`) instead of bit 5 (`WATER_ON`). The diagnostic snapshot would report incorrect active states.

**Fix**: Replace hardcoded values with the proper constants:
```cpp
snapshot.burnerActive = (snapshot.systemStateBits & SystemEvents::SystemState::BOILER_ENABLED) != 0;
snapshot.heatingActive = (snapshot.systemStateBits & SystemEvents::SystemState::HEATING_ON) != 0;
snapshot.waterActive = (snapshot.systemStateBits & SystemEvents::SystemState::WATER_ON) != 0;
```

---

## High — Fix This Sprint

### H1. ~15 SystemSettings Writes in MQTTCommandHandlers Without Mutex

**File**: `src/modules/mqtt/MQTTCommandHandlers.cpp:680-847`

**Problem**: Multiple `SRP::getSystemSettings()` writes happen without taking the system settings mutex. Affected parameters: `preheatEnabled` (line 682), `preheatOffMultiplier` (688), `preheatMaxCycles` (695), `preheatTimeoutMs` (703), `preheatPumpMinMs` (711), `preheatSafeDiff` (718), `pumpCooldownMs` (729), `useWeatherCompensatedControl` (739), `outsideTempHeatingThreshold` (746), `roomTempOverheatMargin` (755), `roomTempCurveShiftFactor` (766), `useBoilerTempPID` (775), plus syslog settings (789-847). By contrast, `handleHeatingCommand()` (line 217) and `handleRoomTargetCommand()` (line 247) correctly acquire the mutex via `SRP::takeSystemSettingsMutex()`. This inconsistency creates data races — the MQTT task can write while control tasks read.

**Fix**: Wrap each settings write block in `SRP::takeSystemSettingsMutex()` / `SRP::giveSystemSettingsMutex()` pairs, or better yet, use RAII MutexGuard. Group adjacent writes under a single lock acquisition where possible.

---

### H2. WheaterControlTask and HeatingControlTask Read SystemSettings Without Mutex

**Files**:
- `src/modules/tasks/WheaterControlTask.cpp:310, 424, 470`
- `src/modules/tasks/HeatingControlTask.cpp:244, 311, 336, 424, 487`

**Problem**: Both tasks call `SRP::getSystemSettings()` and read fields (temperature limits, `wheaterPriorityEnabled`, `useWeatherCompensatedControl`, weather params) without acquiring the system settings mutex. These reads can tear if MQTTCommandHandlers writes to the same fields concurrently (especially multi-byte fields like `float` and `Temperature_t`).

**Fix**: Acquire the system settings mutex before reading, or snapshot the needed settings into local variables under a single short lock at the top of each control cycle. The snapshot approach minimizes mutex hold time and is consistent with the sensor readings pattern already used in the codebase.

---

### H3. ModbusErrorTracker Not Integrated in Device Task Files

**File**: `src/modules/tasks/MonitoringTask.cpp:729-745` (reads tracker), but `MB8ARTTasks.cpp`, `src/modules/tasks/RYN4Task.cpp`, `src/modules/tasks/ANDRTF3Task.cpp` (do not feed tracker)

**Problem**: MonitoringTask queries `ModbusErrorTracker` for per-device stats (CRC errors, timeouts, etc.) and publishes them via MQTT. However, the actual device task files that perform Modbus I/O never call `ModbusErrorTracker::recordSuccess()` or `ModbusErrorTracker::recordError()`. All published Modbus stats are zeros. This is documented as "pending" in `docs/MODBUS_ERROR_TRACKING_INTEGRATION.md`.

**Fix**: In each device task's Modbus read/write result handling, call `ModbusErrorTracker::recordSuccess(address)` on success and `ModbusErrorTracker::recordError(address, errorType)` on failure. The tracker API is already available via `#include <ModbusErrorTracker.h>`.

---

### H4. MQTTTask Started at Wrong Priority (3 Instead of 2)

**File**: `src/modules/tasks/MQTTTask.cpp:560`

**Problem**: MQTTTask is created with `PRIORITY_CONTROL_TASK` (value 3) instead of `PRIORITY_MQTT_TASK` (value 2, defined at `src/config/ProjectConfig.h:281`). MQTT communication is not safety-critical and should not compete with control tasks at priority 3. This can cause priority inversion where MQTT processing delays sensor or heating control.

**Fix**: Change `PRIORITY_CONTROL_TASK` to `PRIORITY_MQTT_TASK` on line 560 of MQTTTask.cpp.

---

### H5. No SHA Pinning for 18 Libraries in platformio.ini

**File**: `platformio.ini:57-95` (and `507-535` for test env)

**Problem**: All 18 library dependencies use bare GitHub URLs without commit SHA or tag pinning (e.g., `https://github.com/packerlschupfer/ESP32-LibraryCommon.git`). Between `rm -rf .pio` cycles, PlatformIO re-fetches libraries and may pull different commits, producing non-deterministic builds. A broken library push could silently break the boiler controller.

**Fix**: Pin each library to a specific commit SHA or release tag:
```ini
https://github.com/packerlschupfer/ESP32-LibraryCommon.git#v1.0.0
; or
https://github.com/packerlschupfer/ESP32-LibraryCommon.git#abc1234
```
Establish a library version bump workflow: update SHA, clean build, test, commit.

---

### H6. handleStatusCommand() and boiler/config/+ Are Stubs

**Files**:
- `src/modules/mqtt/MQTTCommandHandlers.cpp:370-373` — `handleStatusCommand()` logs but publishes nothing
- `src/modules/mqtt/MQTTSubscriptionManager.cpp:178-181` — `boiler/config/+` subscription callback only logs the message

**Problem**: `boiler/cmd/status` is a documented command that should trigger a status publish, but the handler is a stub that only logs `"Status request received"`. The `boiler/config/+` subscription silently discards all config update messages — it logs them but takes no action. Users who send config updates via this topic path get no error and no effect.

**Fix**: For `handleStatusCommand()`, trigger a publish of all status topics (sensors, burner state, health, safety config) or set event bits that cause the periodic publisher to emit immediately. For `boiler/config/+`, either implement the config routing logic (parse subtopic, apply setting) or remove the subscription and document that config changes use `boiler/cmd/config/*` instead.

---

### H7. FailOpenMonitor::publishDegradedStatus() Never Called From Task Loop

**File**: `src/utils/FailOpenMonitor.cpp:86`

**Problem**: `publishDegradedStatus()` is defined and builds an MQTT JSON payload with all tracked fail-open check counts, but it is never called from any periodic task (MonitoringTask, MQTTTask, etc.). Even if `recordFailOpen()` were wired in (see C4), the accumulated data would never be published.

**Fix**: Add a periodic call to `FailOpenMonitor::publishDegradedStatus()` in MonitoringTask's health reporting cycle (e.g., every 60 seconds alongside the existing health publish). Gate it behind a check like `FailOpenMonitor::hasActiveFailOpens()` to avoid publishing empty reports.

---

## Medium — Next Sprint

### M1. DEBUG_FULL BurnerControlTask Stack Inverted (2560 < DEBUG_SELECTIVE 4096)

**File**: `src/config/ProjectConfig.h:215, 238`

**Problem**: DEBUG_FULL mode defines `STACK_SIZE_BURNER_CONTROL_TASK` as 2560 bytes (line 215), while DEBUG_SELECTIVE defines it as 4096 bytes (line 238). DEBUG_FULL enables more logging output, which requires more stack for format strings and variadic arguments. The values appear to be swapped or the DEBUG_FULL value was not updated after the Round 21 refactoring that increased DEBUG_SELECTIVE.

**Fix**: Set DEBUG_FULL `STACK_SIZE_BURNER_CONTROL_TASK` to at least 4096 bytes (matching or exceeding DEBUG_SELECTIVE). Profile actual high-water mark usage under DEBUG_FULL before finalizing.

---

### M2. RELEASE PersistentStorageTask Stack 1536 Bytes (DEBUG Uses 5120)

**File**: `src/config/ProjectConfig.h:259`

**Problem**: RELEASE mode defines `STACK_SIZE_PERSISTENT_STORAGE_TASK` as 1536 bytes, while both debug modes use 5120. PersistentStorageTask uses PersistentStorage (NVS operations), ArduinoJson (JSON serialization), MQTT subscriptions, and parameter publishing. The 3.3x reduction from debug to release seems aggressive and has not been stack-profiled.

**Fix**: Profile the actual high-water mark of PersistentStorageTask under RELEASE build. If NVS + JSON + MQTT subscribe operations need more than ~1200 bytes (leaving 336 for margin), increase the RELEASE stack size. A reasonable starting point would be 2048-2560 bytes.

---

### M3. useWeatherCompensatedControl=true Default Risks Disabling Heating Without Outside Sensor

**File**: `src/config/SystemSettingsStruct.h:126`

**Problem**: `useWeatherCompensatedControl` defaults to `true`. In `HeatingControlModule.cpp:38`, if this flag is true and the outside temperature sensor is invalid/disconnected, the weather compensation logic returns `heatingNeeded = false`, effectively disabling space heating. Deployments without a functioning outside temperature sensor will have heating silently disabled.

**Fix**: Either default `useWeatherCompensatedControl` to `false` (require explicit opt-in), or add a guard in the weather compensation path that falls back to non-compensated control when `isOutsideTempValid` is false.

---

### M4. BurnerRuntimeTracker.burnerStartTime Downgraded From std::atomic to Plain static

**Files**:
- `src/modules/control/BurnerRuntimeTracker.h:51` — declared as `static uint32_t`
- `src/modules/control/BurnerRuntimeTracker.cpp:14` — defined as `uint32_t BurnerRuntimeTracker::burnerStartTime = 0`

**Problem**: Before the Round 21 extraction, `burnerStartTime` was `std::atomic<uint32_t>` inside BurnerStateMachine. The extracted BurnerRuntimeTracker declares it as plain `static uint32_t`. The header comment at line 49 says "atomic for thread-safety" but the type is not atomic. If `getStartTime()` is called from a different task than the one calling `recordStartTime()`/`updateRuntimeCounters()`, this is a data race.

**Fix**: Change the declaration to `static std::atomic<uint32_t> burnerStartTime` in the header and update the definition in the .cpp file. If all callers are confirmed to run in the same BurnerControlTask context, document this single-task constraint and keep the plain type.

---

### M5. Init Functions Called Fire-and-Forget

**File**: `src/init/SystemInitializer.cpp:263, 495, 499, 505`

**Problem**: `StateManager::initialize()` (line 263), `TemperatureSensorFallback::initialize()` (line 495), `FailOpenMonitor::initialize()` (line 499), and `BurnerRequestManager::initialize()` (line 505) return values that are not checked. If any of these fail (e.g., mutex creation failure), the system proceeds with uninitialized subsystems. By contrast, `burnerSystemController_->initialize()` at line 548 correctly checks its return value.

**Fix**: Check return values and either log + continue (for non-critical subsystems) or abort initialization (for safety-critical ones like StateManager). At minimum, log a warning if initialization fails.

---

### M6. 6 Round 21 Helper Class Tests Still Pending

**File**: `test/README.md`

**Problem**: Round 21 extracted 5 helper classes (BurnerSafetyChecks, BurnerPowerController, BurnerRuntimeTracker, RelayVerificationManager, RelayCommandProcessor) plus SafeLog. Test entries are listed as pending in `test/README.md` but no test files exist. These modules contain safety-critical logic (safety checks, power limiting, relay verification).

**Fix**: Create native test files for at least the safety-critical modules:
- `test/test_native/test_burner_safety_checks.cpp`
- `test/test_native/test_burner_power_controller.cpp`
- `test/test_native/test_relay_verification_manager.cpp`

Mock SRP dependencies using the existing test patterns.

---

### M7. boiler/status/burner Not Published by MQTTPublisher

**File**: `src/modules/mqtt/MQTTPublisher.cpp`

**Problem**: The topic `boiler/status/burner` is referenced in documentation and manually published in two locations (BurnerStateMachine.cpp and MQTTCommandHandlers.cpp — both with the wrong prefix, see C1), but MQTTPublisher.cpp does not publish burner state as part of its periodic status cycle. There is no regular burner state publication — only event-driven publishes from BurnerStateMachine state transitions.

**Fix**: Add periodic burner state publication to MQTTPublisher's status cycle (e.g., publish current FSM state, power level, and runtime every 10-30 seconds). This provides a baseline for monitoring even when state transitions are infrequent.

---

## Low — Backlog

### L1. MQTT_API.md Stale — Phantom Topics, Wrong Command Names

**Files**: `docs/MQTT_API.md`, `CLAUDE.md`

**Problem**: MQTT_API.md documents `boiler/status/diagnostics/tasks` and `boiler/status/diagnostics/memory` as "implemented" but neither is published by MQTTPublisher.cpp. CLAUDE.md documents the system command topic as `boiler/cmd/system`, while MQTT_API.md may reference `boiler/cmd/boiler`. Several undocumented topics exist: `boiler/error/context`, `boiler/alert/degraded_operation`, `boiler/diagnostics/modbus/#`, FRAM topics.

**Fix**: Audit all `MQTTTask::publish()` calls and `mqttManager->subscribe()` calls to build a ground-truth topic list. Update MQTT_API.md to match actual implementation. Remove phantom topics, add undocumented ones.

---

### L2. Water Heating PID Gains Not Autotuned

**File**: `src/modules/tasks/PersistentStorageTask.cpp` (PID registration), `src/modules/control/PIDControlModule.cpp`

**Problem**: Water heating PID gains (Kp=1.0, Ki=0.5, Kd=0.1) are generic defaults, not tuned for the water heating thermal system. The space heating PID has the same issue but is less critical since it has weather compensation as a secondary control path.

**Fix**: Run a PID autotune cycle on the water heating system (step response test). Record the tuned gains and update the defaults. Alternatively, implement the PID autotune command (`boiler/cmd/pid_autotune`) to allow field tuning.

---

### L3. Hardcoded Topic Strings Bypass MQTTTopics.h Macros

**Files**: `src/modules/mqtt/MQTTPublisher.cpp`, `src/modules/mqtt/MQTTSubscriptionManager.cpp:10-14`

**Problem**: MQTTSubscriptionManager.cpp defines local `#define` macros (lines 10-14) instead of including `MQTTTopics.h`, with a comment citing "macro conflicts" as the reason. MQTTPublisher.cpp also uses some hardcoded strings. The macro conflict is undocumented and unresolved.

**Fix**: Investigate and resolve the macro conflict in MQTTTopics.h (likely a name collision with another header). Once resolved, replace local defines and hardcoded strings with the centralized macros.

---

### L4. FailOpenMonitor Threshold Check Uses == Instead of >=

**File**: `src/utils/FailOpenMonitor.cpp:49`

**Problem**: The MQTT alert condition is:
```cpp
if (s.consecutiveFailOpens == CONSECUTIVE_ALERT_THRESHOLD && !s.mqttAlertSent)
```
Using `==` means the alert fires exactly once at the threshold count. If the count somehow jumps past the threshold (e.g., rapid successive calls), the alert never fires. Using `>=` is more robust.

**Fix**: Change `==` to `>=`:
```cpp
if (s.consecutiveFailOpens >= CONSECUTIVE_ALERT_THRESHOLD && !s.mqttAlertSent)
```

---

### L5. logSensorStatus/logRelayStatus Rate Limiting Too Aggressive

**File**: `src/modules/tasks/MonitoringTask.cpp:477-482, 526-531`

**Problem**: Both functions use a 1-in-10 rate limiter (log every 10th call). Given the monitoring cycle interval, this means detailed sensor/relay logs appear very infrequently in debug builds, making real-time debugging harder.

**Fix**: Make the rate limit configurable per build mode. For DEBUG_FULL, use 1-in-3 or 1-in-5. For DEBUG_SELECTIVE, keep 1-in-10. For RELEASE, these are compiled out anyway via `LOG_DEBUG` filtering.

---

## Effort Estimate

| ID | Title | Estimated Effort |
|----|-------|-----------------|
| C1 | Wrong MQTT topic prefix (2 locations) | 10 minutes |
| C2 | targetTemp_i32 / hysteresis_i32 not synced after loadAll() | 15 minutes |
| C3 | DEGRADED failsafe doesn't execute subsystem callbacks | 30 minutes |
| C4 | FailOpenMonitor + ErrorContextCapture never called + dead pools | 2-4 hours (integrate) or 30 min (remove pools) |
| C5 | ErrorContextCapture hardcoded bit positions | 10 minutes |
| H1 | SystemSettings writes without mutex (~15 sites) | 1-2 hours |
| H2 | Control tasks read SystemSettings without mutex | 1-2 hours |
| H3 | ModbusErrorTracker integration in device tasks | 2-3 hours |
| H4 | MQTTTask wrong priority constant | 5 minutes |
| H5 | Library SHA pinning (18 libraries) | 1 hour |
| H6 | Implement handleStatusCommand() + boiler/config handler | 2-3 hours |
| H7 | Wire publishDegradedStatus() into MonitoringTask | 30 minutes |
| M1 | Fix DEBUG_FULL BurnerControlTask stack size | 15 minutes |
| M2 | Profile + fix RELEASE PersistentStorageTask stack | 1 hour |
| M3 | Weather compensation default / fallback guard | 30 minutes |
| M4 | Restore std::atomic on burnerStartTime | 15 minutes |
| M5 | Check init function return values | 30 minutes |
| M6 | Write Round 21 helper class tests | 4-6 hours |
| M7 | Add periodic burner state publish | 1 hour |
| L1 | Audit and update MQTT_API.md | 2 hours |
| L2 | Water heating PID autotune | 4-8 hours (includes testing) |
| L3 | Resolve MQTTTopics.h macro conflict | 1 hour |
| L4 | FailOpenMonitor threshold == to >= | 5 minutes |
| L5 | Build-mode configurable log rate limiting | 30 minutes |

---

## Suggested Implementation Order

1. **C1 — Wrong MQTT topic** (10 min). Trivial two-line fix with immediate operational impact. Restores burner error and lockout reset visibility.

2. **C2 — NVS sync for targetTemp/hysteresis** (15 min). Another trivial fix that restores correct room setpoint on reboot. Same proven pattern as the wheater tank fix already in the codebase.

3. **C5 — ErrorContextCapture bit positions** (10 min). Quick fix that makes the module correct for when it gets integrated. No functional impact yet (since captureSnapshot is uncalled), but blocks C4 integration.

4. **H4 — MQTTTask priority** (5 min). One-word change. Restores intended task scheduling.

5. **C3 — DEGRADED failsafe gap** (30 min). Requires a design decision (reduce power vs. shut down) but the fix itself is small. Critical for safety — relay failures must produce meaningful failsafe action.

6. **H1 + H2 — SystemSettings mutex consistency** (2-4 hours combined). Address together since both are the same class of bug. Start with H1 (writes) since those are the more dangerous race, then H2 (reads).

7. **M1 + M2 — Stack size corrections** (1 hour). Quick config fixes that prevent potential stack overflows in DEBUG_FULL and RELEASE builds.

8. **M4 — Restore atomic on burnerStartTime** (15 min). Thread safety regression that should be fixed before it causes a subtle timing bug.

9. **H5 — Library SHA pinning** (1 hour). Build reproducibility. Do this before any production deployment.

10. **C4 — FailOpenMonitor + ErrorContextCapture integration** (2-4 hours). Depends on C5 being done first. If time is limited, at minimum remove the 6KB of dead memory pools and defer full integration.

11. **H7 — Wire publishDegradedStatus()** (30 min). Natural companion to C4. Only meaningful after FailOpenMonitor is integrated.

12. **H3 — ModbusErrorTracker integration** (2-3 hours). Enables Modbus diagnostics that are already published but always show zeros.

13. **H6 — Implement status/config stubs** (2-3 hours). Completes the MQTT API contract.

14. **M3 + M5 + M6 + M7** — Remaining medium items in any order.

15. **L1-L5** — Backlog items as time permits.
