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
