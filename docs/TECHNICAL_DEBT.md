# Technical Debt Log

This document tracks completed architectural changes and pending improvements for the ESP32 Boiler Controller project.

## Completed Removals

### ErrorLogFRAM.cpp.disabled (Removed January 2026)
- **Reason**: Replaced by RuntimeStorage library
- **File**: `src/utils/ErrorLogFRAM.cpp.disabled` (228 lines)
- **Status**: ✅ Deleted
- **Migration**: FRAM error logging uses the `ESP32-RuntimeStorage` library. Only the disabled copy was deleted: `src/utils/ErrorLogFRAM.cpp/.h` still exists as the wrapper over `rtstorage::RuntimeStorage` and is in use (`MonitoringTask.cpp:555`)

### BLE Integration (Removed Round 15)
- **Reason**: Replaced with MB8ART channel 7 for inside temperature
- **Files**:
  - Task configuration removed from `ProjectConfig.h`
  - Diagnostics removed from `MQTTDiagnostics.h`
  - Core initialization removed from `main.cpp`
  - Sensor task integration removed from `SensorTask`
  - Resource provider hooks removed from `SystemResourceProvider`
- **Status**: ✅ Complete, architectural comments retained for context
- **Migration Notes**:
  - Previously used BLE temperature sensor (ANDRTF3 via BLE)
  - Now uses direct Modbus connection to ANDRTF3 (address 0x04)
  - Inside temperature from MB8ART channel 7 for room compensation

### ServiceContainer Pattern (Removed Round 12)
- **Reason**: Migrated to SystemResourceProvider (SRP) pattern
- **Files**:
  - `main.cpp` - removed ServiceContainer initialization
  - `PersistentStorageTask` - migrated to SRP accessors
  - `TaskInitializer` - updated resource access pattern
- **Status**: ✅ Complete, comments retained for migration guidance
- **Migration Path**:
  ```cpp
  // OLD (ServiceContainer pattern)
  auto mqttManager = ServiceContainer::getMQTTManager();

  // NEW (SRP pattern)
  auto mqttManager = SRP::getMQTTManager();
  ```
- **Benefits**:
  - Eliminated global state
  - Thread-safe resource access with built-in mutex protection
  - Centralized resource lifecycle management

### Event Group Consolidation (Round 14)
- **Reason**: Memory optimization (reduced from 7 to 4 event groups)
- **Impact**: Saved ~500 bytes RAM
- **Status**: ✅ Complete
- **Consolidated Groups**:
  1. **GeneralSystemEventGroup**: System-wide events (MQTT, network, errors)
  2. **SystemStateEventGroup**: Burner/heating/water operational state
  3. **SensorEventGroup**: Temperature/pressure sensor updates
  4. **RelayEventGroup**: Relay state changes
- **Removed Groups**:
  - Separate BLE event group
  - Dedicated OTA event group (merged into General)
  - Watchdog event group (merged into General)
- **Current state (2026-09-15)**: event groups were added again since then. `src/core/SystemResourceProvider.h` exposes 11 event group getters (e.g. Burner, BurnerRequest, Heating, ControlRequests, RelayStatus, ErrorNotification besides the four above); see `docs/EVENT_SYSTEM.md`

### Dead Code Cleanup (Round 21 - January 2026)
- **Files Deleted**:
  - `src/utils/ErrorLogFRAM.cpp.disabled` (228 lines)
- **Commented Code Removed**:
  - BLE stack size definitions (4 lines from `ProjectConfig.h`)
  - BLE diagnostic intervals (3 lines from `MQTTDiagnostics.h`)
- **Lines Saved**: 235 lines
- **Status**: ✅ Complete

### Dead Code Cleanup (2026-09-16)
- **Files Deleted**:
  - `src/utils/FailOpenMonitor.h/.cpp` - only `initialize()` was ever called; `recordFailOpen()` had no call site, so the counters stayed zero and `boiler/alert/degraded_operation` / `boiler/status/degraded_checks` were never published
  - `src/diagnostics/MQTTDiagnostics.h/.cpp`, `MQTTDiagnostics_MemoryRecovery.cpp`, `DiagnosticsRecoveryTimer.h/.cpp` - `initialize()` was never called, so the "MQTTDiagnostics" task never ran and `enabled` stayed false; every other entry point (queue metrics, emergency memory recovery, recovery timer) was only reachable from that task
  - `src/core/TaskDependencyManager.cpp`, `include/core/TaskDependencyManager.h` - no caller anywhere; its "TaskHealthMonitor" task was never started
- **Also Removed**:
  - `QueueManager::publishMetrics()` plus `lastMetricsPublish_` / `METRICS_PUBLISH_INTERVAL_MS` (only caller was the dead diagnostics task; the body returned early on `isEnabled()`)
  - The four unused Round 21 memory pools (`DiagnosticBuffer`, `ConfigBuffer`, `CalcBuffer`, `ErrorBuffer`) from `MemoryPool.h/.cpp` - never allocated from, and lazily initialised, so no RAM change
  - Topic macros `MQTT_ALERT_PREFIX`, `MQTT_ALERT_DEGRADED_OPERATION`, `MQTT_STATUS_DEGRADED_CHECKS` (`alert/critical` and `alert/warning` use string literals in `ErrorHandler.cpp` and are unaffected)
- **Runtime Behaviour**: unchanged, except that the boot log line `Initializing fail-open monitor...` is gone
- **Status**: ✅ Complete

## Pending TODOs

Status markers checked against the code on 2026-09-15: **Open**, **Partly done**, **Done**.

### Production Safety Checklist

#### Critical (Required for Unattended Operation)
- [ ] **Remove `ALLOW_NO_PRESSURE_SENSOR` flag** (`ProjectConfig.h:80`)
  - **Status**: Open (flag still defined at `src/config/ProjectConfig.h:80`)
  - **Risk**: Currently allows burner operation without pressure monitoring
  - **Action**: Install 4-20mA pressure transducer, remove flag
  - **Timeline**: Before unattended deployment

- [ ] **Integrate flame sensor hardware** (`BurnerSafetyChecks::isFlameDetected()`, `src/modules/control/BurnerSafetyChecks.h:29`)
  - **Status**: Open
  - **Current**: Uses relay state as proxy for flame detection
  - **Risk**: Cannot detect flame loss during burner operation
  - **Action**: Install ionization rod or UV flame sensor
  - **Timeline**: Before unattended deployment
  - **Implementation**: Update `isFlameDetected()` to read GPIO pin

- [ ] **Install flow sensor** (Not yet implemented)
  - **Status**: Open (`BurnerSafetyValidator::checkHardwareInterlocks()` is still a stub)
  - **Current**: Uses temperature differential as proxy
  - **Risk**: Cannot detect circulation pump failure
  - **Action**: Install flow switch in heating circuit
  - **Timeline**: Recommended for production

#### High Priority
- [ ] **OTA partition verification** (Mentioned in OTA logs)
  - **Status**: Open (no verification record found)
  - **Issue**: Verify sufficient flash partition size for OTA updates
  - **Action**: Test OTA with full firmware size
  - **Timeline**: Before production deployment

- [ ] **Complete runtime stack profiling** (In progress)
  - **Status**: Partly done. Selective mode profiled (Dec 2025), Release mode pending (e.g. RELEASE `STACK_SIZE_PERSISTENT_STORAGE_TASK` 1536 unprofiled, `ProjectConfig.h:256`)
  - **Action**: 24-hour stress test in RELEASE mode
  - **Timeline**: Before production optimization

#### Medium Priority
- [ ] **MQTT QoS Configuration Review**
  - **Status**: Open (e.g. burner status still published with QoS 0, `BurnerStateMachine.cpp:619`)
  - **Current**: Most messages use QoS 0 (fire-and-forget)
  - **Recommendation**: Safety events should use QoS 1 (at-least-once)
  - **Action**: Review and categorize message priorities
  - **Timeline**: Next round of improvements

- [ ] **Parameter Validation Hardening**
  - **Status**: Open (not re-audited)
  - **Issue**: Some MQTT parameter setters accept wide ranges
  - **Action**: Add strict range validation based on equipment specs
  - **Timeline**: Before exposing MQTT to external networks

#### Low Priority
- [ ] **BurnerStateMachine Test Coverage**
  - **Status**: Partly done
  - **Current**: 50 burner test functions: `test_burner_transitions.cpp` 32 (`BurnerTransitions::step()` scenarios incl. MODE_SWITCHING), `test_burner_demand_gate.cpp` 10, `test_burner_transition_policy.cpp` 8 (the simplified-model files `test_burner_state_machine.cpp` and `test_burner_safety.cpp` were removed 2026-09-15; they tested local copies, not firmware)
  - **Target**: Add concurrency tests for seamless mode switching (still open)
  - **Timeline**: Continuous improvement

- [ ] **Modbus Retry Logic Optimization**
  - **Status**: Open
  - **Current**: Fixed retry counts and timeouts
  - **Opportunity**: Adaptive retry based on failure patterns
  - **Timeline**: Performance optimization phase

## Architectural Decisions (Retained for Context)

### Why Fixed-Point Arithmetic?
**Decision**: Use `Temperature_t` (int16_t tenths) and `Pressure_t` (int16_t hundredths)
**Rationale**:
- ESP32 lacks hardware FPU
- Control loops run at 100-200Hz, floating-point adds significant overhead
- Fixed-point provides deterministic performance
- Precision sufficient for HVAC control (±0.1°C, ±0.01 BAR)

**Trade-off**: Requires careful overflow checking, but gained 15-20% performance in control loops

### Why Event-Driven Architecture?
**Decision**: Use FreeRTOS event groups instead of polling loops
**Rationale**:
- Eliminated 18 polling tasks consuming 100ms+ sleep cycles
- Reduced task switching overhead by ~40%
- Improved worst-case response latency from 200ms to <10ms
- Saved ~800 bytes RAM (reduced queue depths)

**Trade-off**: More complex initialization order (see `docs/INITIALIZATION_ORDER.md`)

### Why 19 Tasks Instead of Fewer Modules?
**Decision**: One task per responsibility (Burner, Relay, Sensor, MQTT, etc.)
**Rationale**:
- Each task has dedicated priority for safety-critical operations
- Stack isolation prevents cascading failures
- Watchdog can detect individual task failures
- Modular testing (can disable non-critical tasks)

**Trade-off**: Higher RAM usage (~24KB task stacks), but justified by safety requirements

### Why Custom Libraries Instead of Monorepo?
**Decision**: 19 custom ESP32 libraries (plus 2 forks) published to GitHub
**Rationale**:
- Reusable across multiple ESP32 projects
- Version-controlled dependencies via PlatformIO
- Enforces API boundaries and reduces coupling
- Enables independent testing

**Trade-off**: Library updates require `rm -rf .pio` rebuild cycle

## Memory Optimization History

Total RAM recovered through 20+ rounds of deep code analysis: **6.7KB+**

| Round | Optimization | Savings |
|-------|--------------|---------|
| R12 | ServiceContainer → SRP | ~400 bytes |
| R14 | Event group consolidation | ~500 bytes |
| R15 | BLE stack removal | ~1.2KB |
| R16 | Fixed-point arithmetic | ~800 bytes |
| R17 | Stack tuning (DEBUG_SELECTIVE) | ~2.5KB |
| R18 | Queue depth optimization | ~600 bytes |
| R19 | Flash string migration | ~400 bytes |
| R20 | Safety checks consolidation | ~300 bytes |

**Current Memory Status** (DEBUG_SELECTIVE mode):
- **Heap**: ~180KB free (peak 220KB at boot)
- **Stack**: Largest task 5120 bytes (PersistentStorage), most <4KB
- **Flash**: ~60% utilization (1.2MB / 2MB available)

## Code Quality Metrics

| Metric | Value | Target |
|--------|-------|--------|
| Lines of Code | ~15,000 | Stable |
| Test Coverage | 209 native tests (`RUN_TEST` in `test/test_native/test_main.cpp`) | Round 21 helper tests still missing |
| Documentation | ~180KB | Comprehensive |
| Build Warnings | 0 | Maintained |
| Watchdog Resets | 0 (production) | Zero tolerance |

## References

- **Initialization**: `docs/INITIALIZATION_ORDER.md`
- **Task Architecture**: `docs/TASK_ARCHITECTURE.md`
- **Safety System**: `docs/SAFETY_SYSTEM.md`
- **Memory Strategy**: `docs/MEMORY_OPTIMIZATION.md`
- **Mutex Hierarchy**: `docs/MUTEX_HIERARCHY.md`

---

**Last Updated**: January 2026 (Round 21); status markers, test count and event group note updated 2026-09-15
**Maintainer**: Claude Code Analysis
