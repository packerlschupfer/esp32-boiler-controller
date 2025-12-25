# Open Improvements

Updated: 2026-03-31

## Done (this round)

- C1: Wrong MQTT topic prefix — fixed
- C2: NVS sync for targetTemp/hysteresis — fixed
- C3: DEGRADED failsafe gap — fixed
- C5: ErrorContextCapture bit positions — fixed
- H1: SystemSettings writes without mutex — fixed
- H2: SystemSettings reads without mutex — fixed
- H4: MQTTTask wrong priority — fixed
- M1: DEBUG_FULL BurnerControlTask stack — fixed
- M4: BurnerRuntimeTracker atomic regression — fixed
- Thermal shock limit raised from 30°C to 45°C, made runtime-configurable via MQTT

---

## High

### H3. ModbusErrorTracker Not Integrated in Device Tasks

MonitoringTask queries ModbusErrorTracker and publishes stats via MQTT, but MB8ARTTasks, RYN4Task, and ANDRTF3Task never call `recordSuccess()` / `recordError()`. All published Modbus stats are zeros.

**Fix**: Add tracker calls in each device task's Modbus result handling.

---

### H6. handleStatusCommand() and boiler/config/+ Are Stubs

`boiler/cmd/status` handler only logs, publishes nothing. `boiler/config/+` subscription silently discards messages.

**Fix**: Implement status publish trigger. Either implement or remove the config subscription.

---

## Medium

### M2. RELEASE PersistentStorageTask Stack (1536 vs 5120 in debug)

The 3.3x reduction from debug to release has not been stack-profiled. NVS + JSON + MQTT may overflow 1536 bytes.

**Fix**: Profile high-water mark under RELEASE, increase to 2048-2560 if needed.

---

### M3. useWeatherCompensatedControl=true Default

If outside temp sensor is invalid, weather compensation returns `heatingNeeded = false`, silently disabling heating.

**Fix**: Default to `false`, or add fallback to non-compensated control when outside sensor is unavailable.

---

### M5. Init Return Values Ignored

`StateManager::initialize()`, `TemperatureSensorFallback::initialize()`, `BurnerRequestManager::initialize()` return values are not checked in SystemInitializer.

**Fix**: Check return values, log warnings on failure.

---

### M6. Round 21 Helper Class Tests Pending

BurnerSafetyChecks, BurnerPowerController, BurnerRuntimeTracker, RelayVerificationManager, RelayCommandProcessor have no test files.

**Fix**: Create native tests for safety-critical modules.

---

### M7. boiler/status/burner Not Published Periodically

Burner state is only published on state transitions, not periodically. Monitoring tools miss the current state if they connect after the last transition.

**Fix**: Add periodic burner state publish (every 10-30s) in MQTTPublisher.

---

## Low / Backlog

### L1. MQTT_API.md Has Phantom Topics

Some documented topics are not actually published. Some published topics are undocumented.

**Fix**: Audit all publish/subscribe calls and sync docs.

---

### L2. Water Heating PID Not Autotuned

Water heating PID uses generic defaults (Kp=1.0, Ki=0.5, Kd=0.1), not tuned for this system.

---

### L3. MQTTTopics.h Macro Conflict

MQTTSubscriptionManager.cpp defines local macros instead of including MQTTTopics.h due to unresolved conflict.

---

### L5. Log Rate Limiting Not Build-Mode Aware

Sensor/relay log rate limiter (1-in-10) is the same across all build modes. DEBUG_FULL should log more frequently.

---

## Removed / Not Applicable

- **C4/H7/L4**: FailOpenMonitor — decided to remove entirely (boiler has its own safety systems, ESP32 is supervisory)
- **H5**: Library SHA pinning — deferred, not blocking production
