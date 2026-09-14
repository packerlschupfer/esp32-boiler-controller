# Troubleshooting Guide

This document consolidates common issues and solutions for the ESP32 Boiler Controller.

---

## Table of Contents

1. [Build Issues](#build-issues)
2. [Serial/Monitoring Issues](#serialmonitoring-issues)
3. [Modbus Communication](#modbus-communication)
4. [MQTT Issues](#mqtt-issues)
5. [Sensor Issues](#sensor-issues)
6. [Relay Control Issues](#relay-control-issues)
7. [Memory Issues](#memory-issues)
8. [Safety System Issues](#safety-system-issues)
9. [Network Issues](#network-issues)
10. [Heating and Water Control](#heating-and-water-control)

---

## Build Issues

### Library changes not applied

**Symptom:** Code changes in libraries don't appear after rebuild.

**Solution:**
```bash
rm -rf .pio && pio run
```

**Reason:** PlatformIO caches compiled libraries. A clean build forces re-download and recompilation.

---

### Compilation warnings about pressure sensor

**Symptom:**
```
warning: 'USE_REAL_PRESSURE_SENSOR' is not defined
```

**Solution:** This is expected during development. For production:
1. Install physical pressure sensor
2. Enable `USE_REAL_PRESSURE_SENSOR` in `ProjectConfig.h`
3. Remove `ALLOW_NO_PRESSURE_SENSOR` flag

---

### Out of flash memory

**Symptom:**
```
region `dram0_0_seg' overflowed
```

**Solution:**
1. Use `RELEASE` build mode (smaller code, no debug logging)
2. Check partition table allows sufficient app space
3. Review large static buffers in code

---

## Serial/Monitoring Issues

### Wrong baud rate

**Symptom:** Garbage characters in serial monitor.

**Solution:** Use 921600 baud (not 115200):
```bash
pio device monitor -b 921600
```
Or in `platformio.ini`: `monitor_speed = 921600`

---

### Port conflicts

**Symptom:**
```
could not open port /dev/ttyACM0
```

**Solution:**
```bash
# Find process using port
fuser /dev/ttyACM0

# Kill it (replace PID)
kill -9 <PID>
```

---

### termios error in pio monitor

**Symptom:**
```
termios.error: (5, 'Input/output error')
```

**Solution:** Use Python serial monitor:
```python
import serial
ser = serial.Serial('/dev/ttyACM0', 921600, timeout=1)
while True:
    line = ser.readline()
    if line:
        print(line.decode('utf-8', errors='replace').rstrip())
```

---

### Log buffer overflow

**Symptom:** Missing log lines, garbled output.

**Reason:** Serial baud too slow for log volume.

**Solution:** Ensure 921600 baud. If still overflowing, switch to `DEBUG_SELECTIVE` build mode.

---

## Modbus Communication

### CRC errors / bus collisions

**Symptom:**
```
[ModbusRTU][E] CRC error
```

**Common causes:**
1. External Modbus tool running (mbpoll, modbus-cli)
2. RS485 termination resistors missing/wrong
3. Wiring issues (A/B swapped, ground missing)

**Solution:**
- **Never** use external Modbus tools while ESP32 is running
- Add 120Ω termination resistors at each end of RS485 bus
- Verify common ground between all devices

---

### Device not responding

**Symptom:**
```
[MB8ART][E] Timeout waiting for response
```

**Checklist:**
1. Verify device address (DIP switches)
2. Check baud rate matches (9600 default for most devices)
3. Verify RS485 A/B wiring (some devices swap labels)
4. Power cycle the Modbus device

---

### Bus mutex timeout

**Symptom:**
```
[ModbusDevice][W] Bus mutex timeout
```

**Cause:** Another device holding the bus too long.

**Solution:**
- Check for retry loops in device libraries
- Verify ModbusCoordinator tick scheduling
- Reduce timeout values if appropriate

---

## MQTT Issues

### Subscription failures

**Symptom:** Commands not received, no sensor data published.

**Solution:** Wait 2+ seconds after connection before subscribing:
```cpp
// In MQTTTask, delay is already implemented
vTaskDelay(pdMS_TO_TICKS(2000));
```

---

### Message queue full

**Symptom:**
```
[QueueManager][W] Queue full, dropping LOW priority message
```

**Solution:**
- Normal under heavy load (backpressure working correctly)
- If frequent, check MQTT broker connectivity
- CRITICAL messages bypass queue and are never dropped

---

### Connection drops

**Symptom:** MQTT reconnects frequently.

**Checklist:**
1. Check network stability (ping broker)
2. Verify MQTT keepalive (default 60s)
3. Check broker logs for disconnect reason
4. Verify `credentials.ini` settings

---

## Sensor Issues

### ANDRTF3 returns 0x0000

**Symptom:**
```
[ANDRTF3][E] ERROR: Received 0x0000 - sensor error or communication fault!
```

**Cause:** Intermittent RS485 communication issue (hardware/electrical).

**Solutions:**
1. Check RS485 termination (120Ω at each end)
2. Add decoupling capacitors (100nF) at sensor power pins
3. Verify common ground connection
4. Check cable shielding and routing (away from power cables)

**Note:** 99.7% success rate is acceptable. The system retries on next coordinator tick (5s).

---

### MB8ART channel shows error

**Symptom:**
```
[MB8ART][E] Channel X: Modbus error code 0x7530
```

**Cause:** Sensor disconnected or faulty.

**Solution:**
1. Check PT1000 sensor wiring (2-wire vs 3-wire)
2. Verify channel configuration matches sensor type
3. Check for broken wires or corrosion

---

### Temperature out of range

**Symptom:**
```
[SensorTask][W] Temperature out of range: -32768
```

**Cause:** Invalid reading (sensor disconnected or shorted).

**Solution:** Check physical sensor connection. Value -32768 (0x8000) typically indicates no response.

---

## Relay Control Issues

### Relay state mismatch

**Symptom:**
```
[RYN4Proc][E] Relay verification FAILED! Sent: 0x01, Actual: 0x00
```

**Causes:**
1. DELAY timer still active (expected, not an error)
2. Relay hardware failure
3. Modbus communication error

**Solution:**
- First occurrence: System auto-retries on next tick
- Persistent: Check relay module power and wiring
- Check for DELAY mask in logs (deferred verification is normal)

---

### Relays all OFF unexpectedly

**Symptom:** All relays turn OFF, system shows emergency stop.

**Cause:** DELAY watchdog triggered (ESP32 stopped renewing DELAY commands).

**This is a SAFETY FEATURE:** Hardware auto-OFF protects against ESP32 crash.

**Investigation:**
1. Check for watchdog reset (was ESP32 rebooting?)
2. Check for task starvation (high-priority task blocking)
3. Review logs before the event

---

## Memory Issues

### Stack overflow

**Symptom:**
```
Stack canary watchpoint triggered (TaskName)
```

**Solution:**
1. Increase stack size in `ProjectConfig.h` for affected task
2. Check for large local variables (move to heap or static)
3. Reduce logging in that task

**Stack size guidelines:**
- Debug mode: Add 512-1024 bytes extra
- Tasks with float logging: Minimum 3584 bytes
- Safety-critical tasks: Add 512-byte safety margin

---

### Heap exhaustion

**Symptom:**
```
[MemoryManager][E] Allocation failed: 1024 bytes
```

**Solution:**
1. Check for memory leaks (allocations without matching frees)
2. Reduce static buffer sizes
3. Use `heap_caps_print_heap_info()` to diagnose

---

## Safety System Issues

### LOCKOUT state

**Symptom:** Burner won't start, state shows LOCKOUT.

**Cause:** Ignition failed on `MAX_IGNITION_RETRIES` (3) attempts. Each failed attempt is retried through pre-purge (`Ignition retry N/3`) before the lockout (`Max ignition retries exceeded`).

**Solution:**
1. Fix underlying issue (gas supply, ignition electrode, flame sensor)
2. Reset via MQTT: `mosquitto_pub -t "boiler/cmd/burner_reset" -m "lockout"` (payload `lockout` or `reset`). `BurnerStateMachine::resetLockout()` acts only in LOCKOUT: it clears the retry counter, returns to IDLE and publishes `lockout_reset` on `boiler/status/burner`. Do not send `reset` to `boiler/cmd/system` - that reboots the controller.
3. Without MQTT: LOCKOUT ends automatically after 5 min (`LOCKOUT_TIME_MS`), or power cycle

---

### Emergency stop triggered

**Symptom:**
```
EMERGENCY STOP: <reason>
```

**Causes** (`CentralizedFailsafe::emergencyStop()` via `SafetyInterlocks::triggerEmergencyShutdown()`):
1. Critical boiler temperature: output at or above 115.0°C (`Critical temperature exceeded`)
2. Stale boiler sensor data while the burner runs (`Sensor data stale during operation`)
3. Burner request not refreshed for 10 min (`Burner request watchdog expired`)

This path sets `EMERGENCY_STOP` and clears `BOILER_ENABLED`. Other burner faults (pressure out of range, failed interlock check, third power level relay fault within 10 minutes) call only `BurnerStateMachine::emergencyStop()`: burner in ERROR, no `EMERGENCY STOP:` log, automatic recovery after `errorRecoveryMs`.

**Note:** The emergency shutdown switches off only the burner relays (BURNER_ENABLE, POWER_BOOST, WATER_MODE). `EMERGENCY_STOP` stays set until released; meanwhile both pumps run until the boiler output is below 60.0°C, see [Pumps keep running after an emergency stop](#pumps-keep-running-after-an-emergency-stop).

**Recovery:**
1. Check error logs for cause
2. Fix underlying issue; let the boiler cool below 110.0°C
3. Release via MQTT: `mosquitto_pub -t "boiler/cmd/emergency_reset" -m "reset"`. The result is published (not retained) on `boiler/status/burner`:
   - `emergency_released` - `EMERGENCY_STOP` cleared; `BOILER_ENABLED` set again if the boiler is enabled in the saved settings. The burner still leaves ERROR only after `errorRecoveryMs` (default 5 min)
   - `emergency_not_active` - `EMERGENCY_STOP` is not set
   - `emergency_release_refused:temperature_high` - boiler output invalid or at/above 110.0°C, or boiler return at/above 110.0°C
   - `emergency_release_refused:sensors_unavailable` - sensor fallback cannot continue operation, or `SENSOR_FAILURE` error bit set
   - `emergency_release_refused:system_errors` - `SENSOR_FAILURE`, `MODBUS` or `RELAY` error bit set
4. Without the command, `EMERGENCY_STOP` stays set until the sensor fallback recovers from SHUTDOWN to NORMAL (`BOILER_ENABLED` then stays cleared, `boiler/cmd/system` `on` sets it) or a reboot. `boiler/cmd/system` `on` alone does not restart the burner while `EMERGENCY_STOP` is set

`boiler/cmd/burner_reset` does not release an emergency stop; it only acts in LOCKOUT.

---

### Safety interlock failed

**Symptom:**
```
[SafetyInterlocks][E] Interlock check failed: OVER_TEMP
```

**Solution:** Address the specific interlock failure:
- **OVER_TEMP:** Wait for boiler to cool, check thermostat
- **UNDER_PRESSURE:** Check expansion vessel, system pressure
- **SENSOR_STALE:** Check sensor connections, Modbus communication

---

## Network Issues

### Ethernet not connecting

**Symptom:**
```
[EthernetManager][E] Failed to initialize Ethernet
```

**Checklist:**
1. Verify LAN8720A wiring (MDC=23, MDIO=18, CLK=17)
2. Check 50MHz crystal on LAN8720A module
3. Verify power supply (3.3V, sufficient current)
4. Check Ethernet cable and switch port

---

### Static IP not working

**Symptom:** Device not reachable at configured IP.

**Solution:**
1. Verify IP not conflicting with another device
2. Check subnet mask and gateway in `ProjectConfig.h`
3. Verify switch/router allows static IPs

---

### OTA upload fails

**Symptom:**
```
[OTA][E] Upload failed
```

**Checklist:**
1. Verify device is reachable (ping)
2. Check OTA password in `credentials.ini`
3. Ensure sufficient flash space for new firmware
4. Try reducing upload speed

---

## Heating and Water Control

### Water heating does not restart after re-enabling

**Symptom:** Water heating was switched off during a charge and enabled again. No charge starts although the tank is below `tempLimitHigh`.

**Cause:** Expected. Switching water heating off (water or boiler disable, water OFF override, emergency stop) ends the charge and clears the charge latch, also when water heating is enabled again within the same control cycle:
```
[WaterControlTask][I] Water heating switched off - ending charge
```
A new charge starts only when the tank drops below `wheater/tempLimitLow`. Heating preemption and a temporary sensor loss do not clear the latch; that charge resumes.

---

### Water heating paused: limits inconsistent

**Symptom:**
```
[WaterControlTask][W] Water limits inconsistent: low 60.0°C >= high 50.0°C - water heating paused
```

**Cause:** `wheater/tempLimitLow` is not below `wheater/tempLimitHigh`. Each limit is range-checked on its own (low 30-60°C, high 50-85°C), so sending both in the wrong order inverts the pair.

**Solution:** Set the limits so low < high. When raising both, send `tempLimitHigh` first; when lowering both, send `tempLimitLow` first. Water heating resumes by itself (`Water limits consistent again`).

---

### Heat demand not armed

**Symptom:**
```
[BurnerUpdate][I] Heat demand not armed - boiler 64.4°C, target 47.0°C (BoilerTempCtrl arms when heat is needed)
```

**Cause:** Expected. A heating or water request started while the boiler was already above its target. BurnerControlTask does not ignite the burner; BoilerTempControlTask arms the demand once its PID wants heat. `[BoilerTempCtrl][I] Re-asserting burner OFF - demand was re-armed while coasting` is also normal.

---

### Burner stops immediately after disabling heating or water

**Symptom:**
```
[BurnerStateMachine][I] Space heating disabled - stopping burner now (minimum on-time bypassed)
```

**Cause:** Expected. Disabling the mode the burner is running in, or the boiler, goes to post-purge without waiting for the anti-flapping minimum on-time. Disabling the other mode does not stop the burner. If heat demand returns during post-purge, the burner restarts from there once the minimum off-time is over (`Heat demand returned during post-purge - restarting after N ms`).

---

### Stale heat demand ignored

**Symptom:**
```
[BurnerStateMachine][W] Ignoring stale heat demand - no active heating/water mode request
```

**Cause:** A heat demand is still latched (e.g. after an emergency stop and ERROR recovery) but no heating or water mode is requesting the burner. The burner starts only with an active mode (`HEATING_ON` + heating request, or `WATER_ON` + water request), so it cannot fire with the pumps off. Logged at most once per minute. A running burner likewise stops after 10 s without an active mode request (`Burner running without active heating/water mode request for N ms - stopping`).

---

### Pumps keep running after an emergency stop

**Symptom:** Burner is in ERROR, but a circulation pump is still on.

**Cause:** Expected. The emergency shutdown switches off only the burner relays. The pumps are controlled by PumpControlModule and follow `HEATING_ON`/`WATER_ON` (including overrun), so the heat in the exchanger is still carried away.

While `EMERGENCY_STOP` is set (`CentralizedFailsafe::emergencyStop()`), both pumps run until the boiler output is below 60.0°C, and again from 65.0°C; without a valid, fresh boiler output reading they keep running:
```
[HeatingPumpCtrl][W] Emergency heat dissipation done (boiler output 59.8°C)
```

---

### Burner stops with a power level fault

**Symptom:**
```
[BurnerStateMachine][E] RUNNING_HIGH: failed to set power level (1/3) - stopping burner via post-purge
```

**Cause:** The POWER_BOOST relay command was refused (relay rate limit, queue full, Modbus error). The burner post-purges and may restart. The third fault within 10 minutes escalates to an emergency stop.

**Solution:** Check the log before the fault for relay or RYN4 communication errors.

---

### PID auto-tune rejected or aborted

**Symptom:** `boiler/status/pid/autotune/result` shows `{"status":"rejected","reason":"boiler temp"}` or `{"status":"aborted"}`.

**Cause:**
- **rejected:** Boiler output invalid, stale or outside 15-75°C when `start` was sent
- **aborted:** Boiler output became invalid or stale, or exceeded 80°C, during the relay test; the heat demand is withdrawn
- The autotune also stops when no heating or water request is active, and after 90 minutes at most

**Solution:** Start with the boiler between 15°C and 75°C and keep a heating or water request active for the whole test.

---

## Diagnostic Commands

### MQTT Diagnostics

```bash
# View all sensor data
mosquitto_sub -h BROKER -u USER -P PASS -t "boiler/status/#" -v

# Check system health
mosquitto_sub -h BROKER -u USER -P PASS -t "boiler/status/health"

# Get all parameters
mosquitto_pub -h BROKER -u USER -P PASS -t "boiler/params/get/all" -m ""

# View error log
mosquitto_pub -h BROKER -u USER -P PASS -t "boiler/errors/list" -m "20"
```

### Serial Diagnostics

```bash
# Monitor with timestamp
pio device monitor -b 921600 | ts '%H:%M:%.S'

# Filter specific tags
pio device monitor -b 921600 | grep -E '\[(MB8ART|RYN4|ANDRTF3)\]'
```

---

## Getting Help

1. Check logs for specific error messages
2. Search this document for symptoms
3. Review relevant docs in `docs/` directory
4. Check `CLAUDE.md` for architecture overview
5. Review analysis reports in `.claude/analysis-runs/`

---

*Last updated: 2026-09-14*
