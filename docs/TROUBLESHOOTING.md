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

**Cause:** Too many failed ignition attempts.

**Solution:**
1. Fix underlying issue (gas supply, ignition electrode, flame sensor)
2. Reset via MQTT: `mosquitto_pub -t "boiler/cmd/system" -m "reset"`
3. Power cycle if MQTT not available

---

### Emergency stop triggered

**Symptom:**
```
[CentralizedFailsafe][E] EMERGENCY STOP - All relays OFF
```

**Causes:**
1. Over-temperature condition
2. Pressure fault
3. Multiple consecutive safety check failures

**Recovery:**
1. Check error logs for cause
2. Fix underlying issue
3. Reset via MQTT or power cycle

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

*Last updated: 2025-12-22*
