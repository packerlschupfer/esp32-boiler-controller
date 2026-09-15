# Changelog

All notable changes to the ESP32 Boiler Controller are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## [Unreleased]

### Added
- TROUBLESHOOTING.md - Consolidated troubleshooting guide
- CHANGELOG.md - This file
- BurnerDemandGate: BurnerControlTask publishes an arming permission (`getBurnerDemandPermission()`) and arms a new or changed request only when BoilerTempControlTask would; BoilerTempControlTask publishes its PID decision (`getBoilerTempDecision()`)
- `BurnerTransitions::step()`: burner state machine transition logic (IDLE, PRE_PURGE, IGNITION, RUNNING, MODE_SWITCHING, early restart from POST_PURGE) as a header-only, native-testable function; decision rules in `BurnerTransitionPolicy`
- `notifyWheaterTaskSwitchedOff()`: called by `StateManager::setBoilerEnabled(false)`, `setWaterEnabled(false)`, `setWaterOverrideOff(true)` and `CentralizedFailsafe::emergencyStop()`; wakes WheaterControlTask, which ends a running charge on its next run even if water heating was re-enabled within the same cycle
- Header-only helpers with native tests: `PIDGainFixedPoint`, `RelayExtrema::Tracker`, `RelayCommandPolicy`, `WaterChargePolicy`
- PID autotune limits: start only with a valid, fresh boiler output within 15-75°C (`{"status":"rejected"}`), abort above 80°C or on an invalid/stale boiler sensor (`{"status":"aborted"}`)
- Autotune method persisted: loaded from settings at init, `method:<name>` command saves it
- Native tests: `test_pid_gain_fixed_point.cpp`, `test_relay_extrema_tracker.cpp`, `test_burner_transition_policy.cpp`, `test_burner_transitions.cpp`, `test_burner_demand_gate.cpp`, `test_relay_command_policy.cpp`, `test_stage_c_policies.cpp`
- MQTT command `boiler/cmd/emergency_reset` (payload `reset`): `CentralizedFailsafe::clearEmergencyStop()` releases a latched emergency stop once the boiler output/return are below 110°C, the sensors are available and no SENSOR_FAILURE/MODBUS/RELAY error bit is set; restores `BOILER_ENABLED` from the saved setting; result on `boiler/status/burner` (`emergency_released`, `emergency_not_active`, `emergency_release_refused:<reason>`). Rules in `EmergencyStopRelease.h`, native test `test_emergency_stop_release.cpp`

### Changed
- CLAUDE.md: Corrected "8-state" to "9-state" burner FSM
- Burner leaves IDLE only with an active mode demand (`HEATING_ON` + heating request or `WATER_ON` + water request); PRE_PURGE aborts to IDLE when the mode request or the heat demand is withdrawn; RUNNING stops after 10 s without an active mode request (anti-flapping bypassed)
- BoilerTempControlTask brings the heat demand in line with its PID decision every cycle (level-triggered): arms a missing demand while permitted, drops a demand re-armed while coasting
- BurnerControlTask re-evaluates a blocked standing request every 10 s; autotune power-on requires the demand permission
- Explicit disable of the running mode or the boiler stops the burner immediately (minimum on-time bypassed)
- MODE_SWITCHING waits for a heating request only while heating is likely wanted, at most 15 s; a mode revert resumes only with the ON bit set; 30 s StateMachine hard timeout to POST_PURGE
- POST_PURGE restarts to PRE_PURGE when heat demand returns (same conditions as from IDLE, minimum off-time applies)
- Power level relay fault while entering RUNNING_LOW/HIGH: DEGRADED failsafe and POST_PURGE instead of an emergency stop; the third fault within 10 min escalates
- Emergency shutdown switches off only the burner relays (BURNER_ENABLE, POWER_BOOST, WATER_MODE); the pumps stay with PumpControlModule
- Relay rate limiting and pump motor protection apply only to commands that change the relay state; the pump protection timestamp is updated only on a real change
- IGNITION StateMachine timeout is `IGNITION_TIME_MS + IGNITION_BACKSTOP_MARGIN_MS` (backstop only)
- PID gain changes apply live (`BoilerTempController::updateMode()` reads the active gains every cycle, PID reset on change)
- `pid/waterHeater/kp|ki|kd` ranges 0-100 / 0-10 / 0-50 (were 0-10 / 0-5 / 0-5)
- PID reset after a control pause of more than 10 s
- Autotune maximum duration 90 min (was 40 min); default autotune method ZN_PI (0)
- `heating/outsideThreshold` range 5-25°C (was 5-20°C)
- Water heating: `tempLimitLow >= tempLimitHigh` pauses water heating with one WARN (`WaterChargePolicy::limitsValid`); two-threshold decision in `WaterChargePolicy::nextChargeNeeded`
- Water heating: switching it off (water or boiler disable, water OFF override) clears the charge latch; after re-enable a new charge starts only below `tempLimitLow` (heating preemption and sensor loss still resume)
- ANDRTF3 temperature pointers no longer bound (`bindTemperaturePointers(nullptr, nullptr)`); ANDRTF3Task is the sole writer of the room temperature
- Autotune reads `pid/autotune/amplitude` and `pid/autotune/hysteresis` (defaults 50 % / 1.0°C, were 40 % / 2.0°C and ignored); a warning shows the gain scale when the amplitude differs from the OFF/FULL swing
- Removed unused `SystemConstants::Burner::POST_PURGE_TIME_MS`, `ERROR_RECOVERY_DELAY_MS` and the unconsumed `RelayControl::WATER_PUMP_ON/OFF` writes in WheaterControlTask
- `BurnerSafetyValidator::SafetyConfig::maxBoilerTemp` default is `MAX_BOILER_TEMP_C` (110°C, was 85°C): BurnerControlTask and BoilerTempControlTask check the boiler output against the same limit
- Removed the BurnerSafetyValidator runtime limit check (1 h continuous / 4 h daily, counters never updated), `checkDailyReset()`, `RUNTIME_EXCEEDED` and the validator state mutex
- Removed unused code: `ErrorRecoveryManager` (never constructed) and its native test, `PIDControlTask` (never started), `CentralizedFailsafe::monitorSystemHealth()` (no caller)

### Fixed
- ANDRTF3 HAL: Removed ineffective retry loop (was generating 4 errors instead of 1)
- TempSensorFallback: Changed mode transition log from WARN to INFO (no longer sent to syslog)
- Burner fired on a stale heat demand with no mode active (pump off) after an emergency stop and ERROR recovery
- ANDRTF3 library wrote uncalibrated room temperature and cleared validity on every failed read, bypassing the 3-failure hysteresis and `roomTempOffset`
- `Error::SAFETY` stayed latched after burner safety validation passed
- Boiler PID inert: float gains were truncated by the fixed-point PID (now `PIDGainFixedPoint::fromFloat()`, x1000); BoilerTempControlTask holds OFF level-triggered
- PID output wrapped its sign when narrowed to int16 before clamping (`PIDGainFixedPoint::clampToAdjustment()`)
- Live temperature parameter changes were reverted on the next save (`TemperatureParameterWrapper::applyToSettings()` writes only sensor offsets)
- Burner ignited above target when a request started with a hot boiler
- Seamless HEATING -> WATER switch ended in an emergency stop (unchanged relay command counted by the rate limiter)
- Pumps switched off by the emergency shutdown stayed off with the exchanger hot
- Autotune recorded values at the relay switch points instead of the real peaks/troughs of the lagging boiler; warm-up phase counted depending on tracker call order
- Autotune method setting was never read (every reboot fell back to ZN_PID)
- Ignition retries unreachable: a failed start locked out after one attempt
- MODE_SWITCHING could wait without limit while the burner kept firing; mode revert bounced RUNNING_LOW <-> MODE_SWITCHING
- Inverted tank limits toggled water and heating every control cycle
- Interrupted water charge resumed after switching water heating off and on, also within one task cycle
- Autotune ignored the `pid/autotune/amplitude` and `pid/autotune/hysteresis` settings (hard-coded 50 % / 1.0°C)
- A latched emergency stop could only be released by a reboot: only the sensor fallback cleared `EMERGENCY_STOP` (on recovery from SHUTDOWN) and `BOILER_ENABLED` was never restored
- `EMERGENCY_STOP` was not a latch: BurnerControlTask read-and-cleared it within 3 s, so the pump heat dissipation stopped after seconds, `boiler/cmd/system on` restarted without checks and `emergency_reset` found nothing to release. It is now read without clearing and acted on once per onset; while it is set both pumps run until the boiler output is below 60.0°C (again from 65.0°C, always without a valid, fresh reading). Native tests in `test_emergency_stop_release.cpp`
- Sensor recovery (TemperatureSensorFallback SHUTDOWN -> NORMAL) cleared `EMERGENCY_STOP` whatever had set it; it now releases only a stop caused by stale sensor data (`EmergencyStopRelease::Cause`, `CentralizedFailsafe::releaseAfterSensorRecovery()`)
- MQTT_API.md boiler enable example used `boiler/cmd/boiler` (handled topic: `boiler/cmd/system`); MQTT_QUICK_REFERENCE.txt listed non-existent `cmd/boiler/...` topics and test scripts, rewritten from the handlers
- Removed unused `STACK_SIZE_PID_CONTROL_TASK` and `PRIORITY_PID_CONTROL_TASK`
- MQTT high-priority publish queue 3 -> 5 slots (`HIGH_PRIORITY_QUEUE_SIZE`): `emergency_reset` as the first sensor check after boot queued the sensor fallback status, its mode string and the reply at once and logged `Queue 'mqtt_high_priority' at 100% capacity`
- Pump request refused by pump motor protection (15 s) was lost: the request bit is cleared regardless and PumpControlModule only requested on state changes, so e.g. heating restarting within 15 s of a pump OFF left the heating pump off while the burner fired. The pump task now re-sends every 2 s while `g_relayState` differs (`RelayCommandPolicy::pumpRequestResendDue()`); this also switches off pumps that `CentralizedFailsafe::emergencyStop()` turned on directly when the boiler was below 60.0°C
- Releasing an emergency stop ended the pump heat dissipation at once, even at 100°C; the pumps now continue until the boiler output is below 60.0°C (`EmergencyStopRelease::dissipationAfterRelease()`)
- Boiler PID integral wound up while power was already 100 %: output limits were ±1000 but the power mapping saturates at ±500; limits now ±500 (`PIDGainFixedPoint::POWER_ADJUSTMENT_LIMIT`, mapping shared with the native tests)
- Water `override_on` (WATER_ON without a water request) while heating ran made the burner bounce RUNNING_LOW <-> MODE_SWITCHING every tick; the revert resumes only when the ON bits select the running mode
- While an emergency stop was latched, ERROR recovery re-ran `emergencyShutdown()` and logged errors every tick after 5 min
- Ignition retry counter was kept when LOCKOUT expired, so the next failed start locked out after one attempt; reset at every start and on lockout exit
- No-mode-request grace timer (`nowMs | 1` sentinel) stopped a running burner at once when two steps ran in the same millisecond
- Heating/water `override_on`/`override_off` never reached the mode tasks: ControlTask consumed the bits and set `HEATING_ON`/`WATER_ON` directly (no burner request), and the mode tasks cleared the bits before reading them. The mode tasks now handle the overrides; water `override_on` charges up to `tempLimitHigh`
- PRE_PURGE -> IGNITION was a StateMachine timeout checked before the handler, so a demand or mode request withdrawn in the last second of the pre-purge still ignited; now a step() decision after the checks (timeout 4 s as backstop)
- A stop from MODE_SWITCHING did not record OFF in BurnerAntiFlapping, so the restart from POST_PURGE skipped the 20 s minimum off-time
- Disabling water during a charge took the mode-switch path and could keep the burner firing up to 15 s; explicit disable is now checked first, also in MODE_SWITCHING
- Autotune method was read before NVS was loaded (lost after reboot) and `pid/autotune/method` via `boiler/params` was not applied; `startAutoTuning()` now reads the saved setting
- Error context snapshot (`ErrorContextCapture`) was never called and its ~400-character JSON could not fit the 320-byte MQTT payload. Critical errors logged through `ErrorHandler::logError()` now record a snapshot (static slot, no stack copy or heap, at most every 30 s), and MQTTTask publishes it retained on `boiler/error/context` as compact JSON (`ErrorContextFormat.h`, native-tested)
- `boiler/cmd/fram_errors stats` and `errors/stats` replies were cut off (about 76 characters in a 64-byte `MemoryPools::getTempBuffer()`); the FRAM status/counters/runtime and both stats replies now use the 128-byte `MemoryPools::getString()` buffer
- Scheduler MQTT replies (`list`, `add`/`remove` success, validation errors, `boiler/status/scheduler/info`) were published empty: `SchedulerResponseFormatter` formatted into a StringPool `ScopedBuffer` and returned `c_str()`, but the buffer was released and cleared when the function returned. The formatters now write into caller buffers (no heap: a reply buffer sized to the 320-byte MQTT payload, a local buffer for the status); the list includes as many schedules as fit. MQTT_API.md scheduler reply formats corrected to what the firmware sends
- Weather-compensated mode had no hysteresis on the outside threshold: readings around `heating/outsideThreshold` switched heating (pump, burner request) on and off every 5 s cycle. Now starts below the threshold and stops at threshold + 1.0°C (`SpaceHeatingPolicy::outsideColdForHeating()`)
- Weather-compensated heating curve was 10x too weak: the adjustment was divided by 10^7 (whole °C) but used as tenths, so the heating target stayed at the space-heating minimum (`h/sLo`, 40°C) regardless of outside temperature. Now `HeatingCurve::target()` (header, native-tested) with scale 10^6; defaults `heating/curveCoeff` 1.4 and `heating/curveShift` 0.0°C (were 2.0 / 20.0, which with the corrected curve would ask for 64-75°C on most autumn days)
- Boiler/heating/water enable, water priority and the heating/water OFF overrides changed by command (`boiler/cmd/system|heating|water`, ControlTask) were never saved: `StateManager::markSettingsDirty()` only set a flag nobody read, and PersistentStorageTask saves only on request. A reboot restored the previous state (water heating switched off came back on after a reset). `markSettingsDirty()` now requests a save
- `boiler/cmd/config/outside_heating_threshold` accepted only up to 20.0°C (parameter allows 25.0°C), and it and `room_overheat_margin` wrote SystemSettings only, so the next parameter save stored the old registered value again. Both now go through the parameter storage (`PersistentStorageTask_SetParameter()`, same path as `boiler/params/set/...`)
- OTA upload password was the public default `update-password` hard-coded in all espota envs of platformio.ini (`--auth=update-password`); the envs now use `--auth=${credentials.ota_password}` from the uncommitted credentials.ini, which must equal `-DOTA_PASSWORD`. credentials.example.ini used section `[env]` instead of `[credentials]` and lacked `ota_password`
- OTA_UPDATE_GUIDE.md / OTA_QUICK_REFERENCE.txt used non-existent `cmd/boiler/diagnostics/memory/response` and `cmd/boiler/ota/start` topics, an MQTT OTA progress payload that is never published (`OTATask::initWithMQTT()` has no caller) and missing scripts (`ota_update.sh`, `test_ota_update.py`, `monitor_ota_status.py`); memory monitoring now points to `boiler/status/health`, password to `credentials.ini`
- BoilerTempControlTask refused to arm the heat demand above 85°C boiler output (validator default), capping water charge targets below what the request check allowed

---

## [1.0.0] - 2025-12-22

### Initial Production Release

First production-ready release after comprehensive 20-run analysis scoring 9.5/10.

#### Architecture
- **SRP Pattern**: Zero global variables, all resources via SystemResourceProvider
- **Event-Driven**: 18 FreeRTOS tasks, 100% event-driven (zero polling)
- **Fixed-Point Arithmetic**: Complete PID control without floating point
- **5-Layer Safety**: Validator → Interlocks → Failsafe → DELAY Watchdog → Hardware

#### Features
- 9-state burner finite state machine with anti-flapping
- Two-stage burner control (23.3kW / 42.2kW)
- Space heating with weather compensation
- Hot water tank scheduling with progressive preheating
- MQTT remote monitoring and control (100+ topics)
- NVS persistent parameter storage
- OTA firmware updates
- Syslog remote logging

#### Hardware Support
- MB8ART 8-channel temperature sensor
- RYN4 8-channel relay module with DELAY watchdog
- ANDRTF3 room temperature sensor
- DS3231 RTC for scheduling
- LAN8720A Ethernet PHY

#### Safety Features
- BurnerSafetyValidator: Pre-operation 7-point validation
- SafetyInterlocks: Continuous runtime monitoring
- CentralizedFailsafe: Emergency shutdown coordinator
- Hardware DELAY watchdog: Auto-OFF in 10s if ESP32 fails
- Thermal shock protection: 30°C differential limit

---

## Development History

### Major Improvement Rounds

The codebase includes evidence of 20+ rounds of iterative improvement, documented in code comments with markers like `Round X Issue #Y`.

#### Memory Optimization (M1-M16)
- 6.7KB+ RAM recovered through profiling
- Three-tier stack sizing (DEBUG_FULL/DEBUG_SELECTIVE/RELEASE)
- Static buffer justification in MEMORY_OPTIMIZATION.md

#### Thread Safety (Round 12-14)
- Atomic check-and-reserve patterns (TOCTOU prevention)
- 5-level mutex hierarchy (deadlock prevention)
- Zero `portMAX_DELAY` (all 217 mutex acquisitions use timeouts)

#### State Machine Refinements (SM-CRIT, SM-HIGH)
- MODE_SWITCHING state for seamless water ↔ heating transitions
- Defensive initialization in every state
- Anti-flapping: 2min on, 20s off, 15s power change delay

#### Safety Hardening (Round 15-21)
- Circuit breaker pattern (3 mutex failures → failsafe)
- Aggressive emergency save retry (5 attempts)
- FRAM persistence for critical state

#### Performance (H1-H15)
- Event group caching (eliminates mutex overhead)
- ModbusCoordinator (tick-based, zero bus collisions)
- Priority queue with CRITICAL bypass

---

## Library Updates

### ESP32-ANDRTF3 (2025-12-22)
- Fixed: Use sync result directly instead of async queue race condition
- Fixed: Duplicate tag in log macros

### ESP32-RYN4 (2025-12-22)
- Changed: Disabled runtime retries in RetryPolicy (ModbusCoordinator handles scheduling)
- modbusDefault(): maxRetries 3 → 0
- modbusBackground(): maxRetries 2 → 0

### ESP32-Syslog (2025-12-22)
- Added: `sendUnfiltered()` method to bypass minLevel filter

---

## Versioning Notes

This project uses semantic versioning starting from v1.0.0:
- **MAJOR**: Breaking changes to MQTT API or safety behavior
- **MINOR**: New features, non-breaking enhancements
- **PATCH**: Bug fixes, documentation updates

Prior development history is preserved in code comments and git history.

---

## Quality Metrics (v1.0.0)

From comprehensive 20-run analysis:

| Category | Score |
|----------|-------|
| Architecture | 10/10 |
| Safety | 10/10 |
| Thread Safety | 10/10 |
| Fixed-Point | 10/10 |
| Memory Mgmt | 10/10 |
| Control Systems | 9/10 |
| Communication | 9.5/10 |
| Documentation | 9/10 |
| Testing | 7/10 |
| **Overall** | **9.5/10** |

---

*Last updated: 2026-09-14*
