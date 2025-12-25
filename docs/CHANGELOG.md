# Changelog

All notable changes to the ESP32 Boiler Controller are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## [Unreleased]

### Added
- TROUBLESHOOTING.md - Consolidated troubleshooting guide
- CHANGELOG.md - This file

### Changed
- CLAUDE.md: Corrected "8-state" to "9-state" burner FSM

### Fixed
- ANDRTF3 HAL: Removed ineffective retry loop (was generating 4 errors instead of 1)
- TempSensorFallback: Changed mode transition log from WARN to INFO (no longer sent to syslog)

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

*Last updated: 2025-12-22*
