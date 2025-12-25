# ESPlan Monitoring Tools

This directory contains Python scripts for monitoring and debugging the ESPlan Boiler Controller via serial port.

## Prerequisites

- Python 3.6+
- pyserial library
- Device connected via USB (typically `/dev/ttyACM0` or `/dev/ttyUSB0`)

### Installation

```bash
# Install required dependencies
pip install -r requirements.txt
```

## Available Tools

### 1. monitor_system.py
General system monitor for warnings, errors, and overall health.

```bash
# Basic usage - monitor for 60 seconds
python3 monitor_system.py

# Monitor for 2 minutes and show all messages
python3 monitor_system.py -t 120 -a

# Use different serial port
python3 monitor_system.py -p /dev/ttyUSB0
```

**What it monitors:**
- System warnings and errors
- Service resolution issues ("Service not found")
- MQTT publish failures
- MonitoringTask showing 0.0°C
- Task watchdog timeouts
- System state changes

### 2. monitor_watchdog.py
Specifically monitors task timing and watchdog behavior.

```bash
# Monitor MonitoringTask for watchdog issues
python3 monitor_watchdog.py

# Monitor specific task for 3 minutes
python3 monitor_watchdog.py --task BurnerControlTask -t 180
```

**What it monitors:**
- Task initialization timing
- Watchdog timeout errors
- Task lifecycle events
- Time from task start to main loop entry

### 3. monitor_boot.py
Captures and analyzes the boot sequence.

```bash
# Wait for boot sequence (reset device after starting)
python3 monitor_boot.py

# Wait up to 2 minutes for boot
python3 monitor_boot.py -t 120
```

**What it monitors:**
- Boot stage timing
- Service registration
- Task initialization order
- Errors during boot
- Total boot time

### 4. monitor_sensors.py
Monitors sensor readings and validity.

```bash
# Monitor sensor values
python3 monitor_sensors.py

# Show detailed JSON sensor data
python3 monitor_sensors.py -j

# Monitor for 3 minutes
python3 monitor_sensors.py -t 180
```

**What it monitors:**
- All temperature sensor readings
- Sensor validity flags
- Zero temperature readings (0.0°C)
- Invalid sensor states
- MQTT sensor JSON messages

## Common Use Cases

### Debugging "Service not found" warnings
```bash
python3 monitor_system.py -t 120 | grep "Service not found"
```

### Checking for watchdog timeouts
```bash
python3 monitor_watchdog.py --task MonitoringTask
```

### Verifying sensor initialization
```bash
# Start this, then reset the device
python3 monitor_boot.py
```

### Monitoring sensor health
```bash
python3 monitor_sensors.py -j -t 300
```

### Full system health check
```bash
# Run all monitors in sequence
python3 monitor_boot.py && python3 monitor_system.py -t 120 && python3 monitor_sensors.py -t 60
```

## Troubleshooting

### "Permission denied" error
Add user to dialout group:
```bash
sudo usermod -a -G dialout $USER
# Log out and back in for changes to take effect
```

### Wrong serial port
List available ports:
```bash
ls -la /dev/tty* | grep -E "(USB|ACM)"
```

### Device not responding
1. Check USB connection
2. Verify baud rate (115200)
3. Try resetting the device
4. Check if another program is using the port

## For Claude Code Sessions

When debugging issues in future sessions, start with:

1. **General system check:**
   ```bash
   cd tools/monitoring
   python3 monitor_system.py -t 60
   ```

2. **If seeing warnings/errors, get more detail:**
   ```bash
   python3 monitor_system.py -t 120 -a | tee system_log.txt
   ```

3. **For specific issues:**
   - Watchdog: `python3 monitor_watchdog.py`
   - Sensors: `python3 monitor_sensors.py -j`
   - Boot problems: `python3 monitor_boot.py`

4. **Save logs for analysis:**
   ```bash
   python3 monitor_system.py -t 300 > debug_session_$(date +%Y%m%d_%H%M%S).log
   ```

These tools were created during debugging sessions and have proven useful for:
- Identifying timing issues
- Catching initialization problems
- Monitoring system stability
- Verifying fixes

Keep these tools updated as new debugging needs arise!