# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository Overview

ESP32-based industrial boiler controller with event-driven architecture, 4-layer safety system, MQTT monitoring, and PID temperature control. Field-tested in production with 20+ rounds of deep code analysis. Uses 18 custom ESP32 libraries for modular, reusable components.

## Common Development Tasks

### Building and Uploading

```bash
# Build for development (default)
pio run -e esp32dev_usb_debug_selective

# Build for production
pio run -e esp32dev_usb_release

# Full debug output
pio run -e esp32dev_usb_debug_full

# Upload via USB
pio run -e esp32dev_usb_debug_selective -t upload --upload-port /dev/ttyACM0

# Upload via OTA (after initial USB flash)
pio run -e esp32dev_ota_debug_selective -t upload

# Monitor serial output
pio device monitor -b 921600

# Clean build (REQUIRED after library updates)
rm -rf .pio
pio run
```

### Configuration Setup

1. Copy credentials template:
   ```bash
   cp credentials.example.ini credentials.ini
   # Edit credentials.ini with your MQTT/OTA credentials
   ```

2. Configure network settings in `src/config/ProjectConfig.h`:
   - `ETH_STATIC_IP`, `ETH_GATEWAY`, `ETH_SUBNET`
   - `MQTT_SERVER`

### Testing and Monitoring

```bash
# Run native tests (no hardware required)
pio test -e native_test

# Run embedded tests (ESP32 required)
pio test -e esp32_test --upload-port /dev/ttyACM0

# Initialize parameters via MQTT
python3 scripts/init_parameters.py

# Monitor MQTT topics
mosquitto_sub -h 192.168.20.27 -u USER -P PASS -t "boiler/#" -v
```

### MQTT Testing

```bash
# View all topics
mosquitto_sub -h BROKER_IP -u USER -P PASS -t "#" -v

# System control
mosquitto_pub -h BROKER_IP -u USER -P PASS -t "boiler/cmd/system" -m "on"
mosquitto_pub -h BROKER_IP -u USER -P PASS -t "boiler/cmd/heating" -m "on"

# Get all parameters
mosquitto_pub -h BROKER_IP -u USER -P PASS -t "boiler/params/get/all" -m ""

# Save parameters to NVS
mosquitto_pub -h BROKER_IP -u USER -P PASS -t "boiler/params/save" -m ""

# Configure safety parameters
mosquitto_pub -h BROKER_IP -u USER -P PASS -t "boiler/cmd/config/sensor_stale_ms" -m "60000"
mosquitto_pub -h BROKER_IP -u USER -P PASS -t "boiler/cmd/config/pump_protection_ms" -m "15000"
```

## Architecture Overview

### System Resource Provider (SRP) Pattern

**CRITICAL**: This codebase uses the SRP pattern - NO global variables. All shared resources accessed through `SystemResourceProvider.h`.

```cpp
// WRONG - Do not use globals
extern EventGroupHandle_t xSystemStateEventGroup;

// CORRECT - Use SRP
SRP::setSystemStateEventBits(BIT);
auto mqttManager = SRP::getMQTTManager();
auto readings = SRP::getSensorReadings();  // Remember to lock mutex!
```

### Event-Driven Architecture

Zero polling loops. All tasks use FreeRTOS event groups for inter-task communication:

- **GeneralSystemEventGroup**: System-wide events
- **SystemStateEventGroup**: Burner/heating/water state
- **SensorEventGroup**: Temperature/pressure sensor updates
- **RelayEventGroup**: Relay state changes
- **ControlRequestsEventGroup**: User commands

### Task Structure (18 FreeRTOS Tasks)

| Priority | Tasks | Purpose |
|----------|-------|---------|
| 4 | BurnerControl, RelayControl | Safety-critical operations |
| 3 | Heating, Water, MB8ART, ANDRTF3, Sensor, RYN4, Control | Control logic and sensors |
| 2 | MQTT, Monitoring, HeatingPump, WaterPump | Communication, diagnostics, pump control |
| 1 | OTA | Background firmware updates |

See `docs/TASK_ARCHITECTURE.md` for complete details.

### Fixed-Point Arithmetic

**No floating-point in control loops**. Uses custom fixed-point types:

- **Temperature_t**: `int16_t` in tenths of °C (±3276.7°C, 0.1°C precision)
  ```cpp
  Temperature_t temp = tempFromFloat(25.5f);  // → 255
  float asFloat = tempToFloat(temp);           // → 25.5f
  Temperature_t result = tempAdd(temp1, temp2);
  ```

- **Pressure_t**: `int16_t` in hundredths of BAR (±327.67 BAR, 0.01 BAR precision)

Defined in `src/shared/Temperature.h` and `src/shared/Pressure.h`.

### Safety System (5 Layers)

1. **BurnerSafetyValidator**: Pre-operation validation (7 checks)
2. **SafetyInterlocks**: Continuous monitoring during operation
3. **CentralizedFailsafe**: Coordinated emergency shutdown
4. **DELAY Watchdog**: Hardware-enforced relay auto-OFF (10s, renewed every 5s) - protects against ESP32 failures
5. **Hardware Interlocks**: (Future) Physical safety sensors

Runtime-configurable safety parameters via MQTT:
- Sensor staleness timeout: 30-300s (default 60s)
- Pump protection delay: 5-60s (default 15s)
- Post-purge duration: 30-180s (default 90s)

See `docs/SAFETY_SYSTEM.md` for complete architecture.

## Key Files and Directories

### Configuration
- `platformio.ini` - Build environments, library dependencies, compiler flags
- `credentials.ini` - MQTT/OTA credentials (not in repo, copy from example)
- `src/config/ProjectConfig.h` - Hardware pins, stack sizes, build flags
- `src/config/SystemConstants.h` - All system constants (centralized)

### Core System
- `src/core/SystemResourceProvider.h` - Central resource access (SRP pattern)
- `src/core/SharedResourceManager.h` - Resource lifecycle management
- `src/core/QueueManager.h` - Priority-based MQTT message queuing
- `src/init/SystemInitializer.cpp` - Complete system initialization sequence

### Control Systems
- `src/modules/control/BurnerStateMachine.cpp` - 8-state burner FSM
- `src/modules/control/HeatingControlModule.cpp` - Space heating PID
- `src/modules/control/PumpControlModule.cpp` - Unified pump control (heating + water)
- `src/modules/control/BurnerSafetyValidator.cpp` - Pre-operation safety checks
- `src/modules/control/SafetyInterlocks.cpp` - Continuous safety monitoring
- `src/modules/control/CentralizedFailsafe.cpp` - Emergency shutdown coordinator

### Tasks
- `src/modules/tasks/BurnerControlTask.cpp` - Safety-critical burner control
- `src/modules/tasks/RelayControlTask.cpp` - Physical relay control
- `src/modules/tasks/HeatingControlTask.cpp` - Space heating control
- `src/modules/tasks/WheaterControlTask.cpp` - Water heating control
- `src/modules/tasks/TimerSchedulerTask.cpp` - Generic schedule management
- `src/modules/tasks/MQTTTask.cpp` - MQTT communication with priority queues

### Shared Types
- `src/shared/Temperature.h` - Fixed-point temperature type
- `src/shared/Pressure.h` - Fixed-point pressure type
- `src/shared/RelayState.h` - Relay state with DELAY tracking (requires `initRelayState()`)
- `include/bits/BurnerRequestBits.h` - Event-driven burner request system

### Utilities
- `src/utils/CriticalDataStorage.h` - FRAM-based emergency state persistence
- `src/utils/ErrorHandler.cpp` - Centralized error handling
- `src/DebugMacros.h` - Logging configuration by build mode

### Documentation
- `docs/TASK_ARCHITECTURE.md` - Complete task documentation (25KB)
- `docs/STATE_MACHINES.md` - Burner state machine details (20KB)
- `docs/MQTT_API.md` - Complete MQTT reference (22KB)
- `docs/SAFETY_SYSTEM.md` - 4-layer safety architecture
- `docs/ARCHITECTURE_PATTERNS.md` - SRP pattern, thread safety, Result<T>
- `docs/MUTEX_HIERARCHY.md` - Deadlock prevention
- `docs/EVENT_SYSTEM.md` - Event group definitions

## Library Ecosystem

This project uses **18 custom ESP32 libraries** (all published on GitHub under packerlschupfer):

**Foundation**: LibraryCommon, Logger, MutexGuard, SemaphoreGuard
**Core**: Watchdog, TaskManager, EthernetManager, OTAManager, NTPClient, PersistentStorage, RuntimeStorage
**Framework**: MQTTManager, IDeviceInstance, ModbusDevice
**Hardware**: MB8ART, RYN4, ANDRTF3, DS3231Controller

Library development uses `git+file://` paths in platformio.ini. For production, use GitHub URLs.

After library changes: **Always run `rm -rf .pio && pio run`**

## Critical Development Patterns

### Thread Safety

**Always** protect shared data with mutexes:

```cpp
if (SRP::takeSensorReadingsMutex(TaskTimeouts::MUTEX_WAIT)) {
    auto& readings = SRP::getSensorReadings();
    readings.temperature = newValue;
    SRP::giveSensorReadingsMutex();
} else {
    LOG_ERROR(TAG, "Failed to acquire mutex");
}
```

Use RAII guards from ESP32-MutexGuard library when available.

### Error Handling with Result<T>

All fallible operations return `Result<T>`:

```cpp
Result<float> readSensor() {
    if (!ready) return Result<float>(SystemError::NOT_READY);
    return Result<float>(value);
}

auto result = readSensor();
if (result.isError()) {
    ErrorHandler::handleError(result.error());
} else {
    float value = result.value();
}
```

### Watchdog Feeding

All long-running tasks must feed watchdog:

```cpp
while (!shouldStop()) {
    SRP::getTaskManager().feedWatchdog();
    // Do work...
    vTaskDelay(TaskTimeouts::EVENT_WAIT);
}
```

### Logging with SafeLog.h

For multiple float logging (prevents stack overflow):

```cpp
#include "utils/SafeLog.h"

SafeLog::logFloatPair(TAG, "Temp: %0.1f, Pressure: %0.2f", temp, pressure);
SafeLog::logFloatTriple(TAG, "P=%0.2f I=%0.2f D=%0.2f", p, i, d);
```

Regular logging:
```cpp
LOG_INFO(TAG, "Message");
LOG_ERROR(TAG, "Error: %d", errorCode);
```

### Stack Size Requirements

Stack sizes vary by build mode (defined in `ProjectConfig.h`):

- **DEBUG_FULL**: Largest stacks (most logging)
- **DEBUG_SELECTIVE**: Optimized stacks (strategic logging, DEFAULT)
- **RELEASE**: Minimal stacks (production)

Tasks with float logging need **minimum 3584 bytes** in debug modes.

### Relay Verification and DELAY Commands

**RYN4 Hardware DELAY Commands**:
- Command format: `0x06XX` where XX = delay in seconds (01-99 hex)
- Example: `0x0614` = Relay ON immediately, auto-OFF after 20 seconds
- Used for burner safety: Prevents enable signal gaps during mode transitions
- Reading relay status returns physical state (0x0001/0x0000), never command value

**Verification Behavior**:
- **SET tick**: Write relay changes via `setMultipleRelayStates()`
- **READ tick**: Verify states 1000ms later via `readBitmapStatus()`
- **DELAY-aware**: Skip verification for relays with active hardware countdown
- **Mismatch counting**: First mismatch = DEBUG log (silent retry), 2+ = ERROR log
- **Auto-retry**: Persistent mismatches re-queue on next SET tick

**RelayState Structure** (`src/shared/RelayState.h`):
```cpp
struct RelayState {
    std::atomic<uint8_t> desired;              // Target states
    std::atomic<uint8_t> actual;               // Last verified states
    std::atomic<bool> pendingWrite;            // Write pending flag
    std::atomic<uint8_t> consecutiveMismatches; // Retry counter
    std::atomic<uint8_t> delayMask;            // DELAY active bitmask
    uint32_t delayExpiry[8];                   // DELAY expiration timestamps
    SemaphoreHandle_t delayMutex;              // Protects delayExpiry[]

    void setDelayCommand(uint8_t relay, uint8_t delaySeconds);
    void clearDelay(uint8_t relay);
    bool isDelayActive(uint8_t relay) const;
};
```

**Initialization**: `initRelayState()` MUST be called during system startup to create delay tracking mutex.

See `docs/EQUIPMENT_SPECS.md` and `docs/ALGORITHMS.md` for complete details.

## Build Modes

Three build configurations in `platformio.ini`:

1. **DEBUG_SELECTIVE** (default): Strategic logging, optimized performance
   - Flag: `-DLOG_MODE_DEBUG_SELECTIVE`
   - Use: Active development
   - Stack: Optimized (e.g., BurnerControl: 3584 bytes)

2. **DEBUG_FULL**: Maximum verbosity for troubleshooting
   - Flag: `-DLOG_MODE_DEBUG_FULL`
   - Use: Deep debugging only
   - Stack: Largest (e.g., BurnerControl: 2560 bytes + more heap)

3. **RELEASE**: Production-optimized, minimal logging
   - Flag: `-DLOG_MODE_RELEASE`
   - Use: Field deployment
   - Stack: Minimal (e.g., BurnerControl: 1536 bytes)

## Memory Optimizations

Result of 20+ rounds of deep code analysis:
- **6.7KB+ RAM recovered** through optimizations
- Event-driven architecture (zero polling)
- Fixed-point arithmetic (no float in control loops)
- Flash string storage for constants
- Stack tuning based on runtime profiling

## MQTT API Structure

### Command Topics (Subscribed)
```
boiler/cmd/system                    - on/off/reboot
boiler/cmd/heating                   - on/off/override_on/override_off
boiler/cmd/water                     - on/off/override_on/override_off
boiler/cmd/config/sensor_stale_ms    - 30000-300000 (safety config)
boiler/cmd/config/pump_protection_ms - 5000-60000 (equipment protection)
boiler/cmd/config/post_purge_ms      - 30000-180000 (post-purge duration)
boiler/cmd/scheduler/add             - Add schedule (JSON)
boiler/cmd/scheduler/remove          - Remove schedule by ID
```

### Status Topics (Published)
```
boiler/status/sensors         - Temperature/pressure JSON (10s interval)
boiler/status/health          - System health (60s interval)
boiler/status/burner          - Burner state machine state
boiler/status/safety_config   - Current safety configuration
boiler/status/device/ip       - Device IP address
```

### Parameter Topics
```
boiler/params/heating/setpoint       - Room temperature setpoint
boiler/params/wheater/tempLimitLow   - Water heating start threshold
boiler/params/pid/spaceHeating/kp    - PID proportional gain
boiler/params/get/all                - Request all parameters
boiler/params/save                   - Save to NVS
```

See `docs/MQTT_API.md` for complete reference.

## Burner State Machine

8-state finite state machine with anti-flapping protection:

```
IDLE → PRE_PURGE → IGNITION → RUNNING_LOW/HIGH → POST_PURGE → (IDLE | LOCKOUT | ERROR)
```

- **Minimum on-time**: 2 minutes (equipment protection)
- **Minimum off-time**: 20 seconds (anti-flapping)
- **Request expiration**: 10 minutes (watchdog)

See `docs/STATE_MACHINES.md` for complete state diagrams and transitions.

## Hardware Configuration

### Required Components
- ESP32-DevKitC or compatible
- LAN8720A Ethernet PHY (MDC=23, MDIO=18, CLK=17)
- MB8ART 8-channel temperature sensor (Modbus RTU, RX=36, TX=4)
- RYN4 8-channel relay module (Modbus RTU)
- RS485 transceiver (MAX485 or similar)

### Optional Components
- ANDRTF3 room temperature sensor (Modbus RTU)
- DS3231 RTC module (I2C, SDA=21, SCL=22)
- MB85RC256V FRAM (I2C) - for enhanced data logging
- Flame sensor, flow sensor, pressure sensor (recommended for unattended operation)

## Network Configuration

Default settings (modify in `src/config/ProjectConfig.h`):
- **Device IP**: 192.168.20.40 (static)
- **Gateway**: 192.168.20.1
- **MQTT Broker**: Configure in `credentials.ini`
- **OTA Port**: 3232

## Testing

### Run Tests
```bash
# Native tests (development machine)
pio test -e native_test

# Embedded tests (requires ESP32)
pio test -e esp32_test --upload-port /dev/ttyACM0

# Run specific test with verbose output
pio test -e native_test -v --filter test_temperature_conversion
```

Test categories:
- **Native tests**: Temperature conversion, safety logic, memory pools
- **Embedded tests**: Relay control, Modbus communication

See `test/README.md` for complete testing documentation.

## Production Status

**Field-Tested**: ✅ Weeks of continuous operation, zero watchdog resets

**Safe WITH supervision**: Current deployment uses proxy sensors (relay state for flame, temperature differential for flow)

**For unattended operation**, install:
- Physical flame sensor (ionization rod or UV detector)
- Flow sensor (verify water circulation)
- Pressure sensor (4-20mA transducer, currently optional)

## Common Issues and Solutions

### Build Issues
- **Library changes not applied**: Run `rm -rf .pio` before building
- **Compilation warnings about pressure sensor**: Expected when `USE_REAL_PRESSURE_SENSOR` not defined

### Runtime Issues
- **Stack overflow** ("Stack canary watchpoint triggered"): Increase stack size in `ProjectConfig.h`
- **MQTT subscription failures**: Wait 2+ seconds after connection before subscribing
- **Mutex timeout**: Check RAII guard usage, verify no deadlocks (see `docs/MUTEX_HIERARCHY.md`)

### Modbus Testing
- **WARNING**: Do NOT use `mbpoll` or other Modbus tools on `/dev/ttyUSB485` while ESP32 is running!
- **Issue**: ESP32 is the Modbus master. External tools create bus collisions → CRC errors → system instability
- **Symptoms**: Cascading CRC errors, bus mutex timeouts, requires reboot
- **For testing**: Power off ESP32 first, OR use MQTT diagnostics (`boiler/status/sensors`, `boiler/cmd/config/*`)

### Serial Monitoring
- **Baud rate**: 921600 (not 115200!) - configured in `platformio.ini` as `monitor_speed`
- **Port conflicts**: Check with `fuser /dev/ttyACM0`, kill conflicting processes
- **termios error**: Use alternative monitor like minicom or custom Python script
- **Buffer overflow**: Serial baud is 921600 to prevent log buffer overflow

**Reliable Python serial monitor** (when `pio device monitor` has termios issues):
```python
import serial
import time

ser = serial.Serial('/dev/ttyACM0', 921600, timeout=1)

# Optional: Reset ESP32
ser.dtr = False
ser.rts = True
time.sleep(0.1)
ser.rts = False
ser.dtr = True

# Read output
while True:
    line = ser.readline()
    if line:
        print(line.decode('utf-8', errors='replace').rstrip())
```

## Slash Commands

Custom development workflows in `.claude/commands/`:

- `/check-project-status` - Quick status check
- `/platformio-workflow` - Complete PlatformIO workflow
- `/quick-fix` - Common PlatformIO issue fixes
- `/commit-library-changes` - Library commit workflow
- `/analyze-main-project` - Architecture analysis
- `/iterate-main-project` - Continue improvements

## Important Notes

1. **Safety-first design**: All changes must be reviewed for safety impact
2. **Thread safety required**: Follow SRP pattern, use mutexes properly
3. **Fixed-point arithmetic**: No float in control loops
4. **Event-driven only**: No polling loops
5. **Clean builds after library updates**: Always `rm -rf .pio`
6. **Serial port**: Default is `/dev/ttyACM0` at 921600 baud
7. **MQTT credentials**: Never commit `credentials.ini` to git
8. **Documentation**: Update relevant docs in `docs/` when changing architecture
9. **RelayState initialization**: `initRelayState()` must be called during system startup (creates delay tracking mutex)

## Additional Resources

- **Full documentation**: `docs/` directory (~180KB technical documentation)
- **Initialization sequence**: `docs/INITIALIZATION_ORDER.md` - System startup order and dependencies
- **Memory strategy**: `docs/MEMORY_OPTIMIZATION.md` - ESP32 static buffer rationale
- **Task architecture**: `docs/TASK_ARCHITECTURE.md` - All 18 FreeRTOS tasks
- **Algorithms**: `docs/ALGORITHMS.md` - 13 control algorithms including Modbus scheduling
- **Library repositories**: https://github.com/packerlschupfer
- **Issue tracker**: GitHub Issues
- **Claude development notes**: `.claude/` directory (session summaries, task logs)
