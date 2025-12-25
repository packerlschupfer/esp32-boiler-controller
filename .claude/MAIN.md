# Claude Development Documentation

## Project Overview
ESPlan Boiler Controller - ESP32-based boiler control system with advanced safety features, MQTT monitoring, and PID control.

## Important Files

### Core System Files
- `include/bits/BurnerRequestBits.h` - Event-driven burner request system
- `src/core/SharedResourceManager.h` - Central resource management
- `src/core/SystemResourceProvider.h` - System resource access layer
- `src/modules/tasks/BurnerControlTask.cpp` - Main burner control logic
- `src/modules/tasks/HeatingControlTask.cpp` - Space heating control
- `src/modules/tasks/WheaterControlTask.cpp` - Water heating control
- `src/modules/control/HeatingControlModule.cpp` - PID control integration
- `src/modules/control/BurnerStateMachine.cpp` - Burner state management
- `src/modules/tasks/ANDRTF3Task.cpp` - Room temperature sensor (replaces MB8ART channel 7)
- `src/modules/tasks/NTPTask.cpp` - NTP time synchronization
- ~~`src/modules/tasks/HotWaterSchedulerTask.cpp`~~ - Replaced by TimerSchedulerTask
- `src/modules/tasks/TimerSchedulerTask.cpp` - Generic scheduler for multiple schedule types
- `src/modules/tasks/PersistentStorageTask.cpp` - Parameter storage management
- `src/shared/Temperature.h` - Fixed-point temperature type (Temperature_t, tenths of °C)
- `src/shared/Pressure.h` - Fixed-point pressure type (Pressure_t, hundredths of BAR)
- `src/utils/CriticalDataStorage.h` - FRAM-based emergency state persistence
- `src/core/QueueManager.h` - Priority-based message queuing with overflow handling

### Scheduler System (New Generic Implementation)
- `include/TimerSchedule.h` - Schedule data structure with union for different types
- `include/IScheduleAction.h` - Interface for schedule action handlers
- `src/modules/scheduler/WaterHeatingScheduleAction.cpp` - Water heating schedule handler
- `src/modules/scheduler/SpaceHeatingScheduleAction.cpp` - Space heating schedule handler
- `src/RuntimeStorageSchedules.cpp` - FRAM persistence for schedules
- `include/RuntimeStorageSchedules.h` - Schedule storage API

### Configuration Files
- `platformio.ini` - Build configuration
- `credentials.ini` - MQTT credentials (not in repo)
- `src/config/ProjectConfig.h` - Hardware pins, build flags, stack sizes
- `src/config/SystemConstants.h` - All system constants (centralized)
- `docs/mqtt_hotwater_commands.md` - Hot water scheduler MQTT API
- `config_migration_todo.md` - Configuration centralization tracking

### Archived/Deprecated
- ~~`src/archive/`~~ - Directory removed in cleanup (January 2025)

## Fixed-Point Architecture

### Temperature Type (Temperature_t)
- **Type**: `int16_t` (tenths of degrees Celsius)
- **Range**: -3276.8°C to +3276.7°C with 0.1°C precision
- **Example**: 273 = 27.3°C, -50 = -5.0°C
- **Conversion**:
  ```cpp
  Temperature_t temp = tempFromFloat(25.5f);  // → 255
  float asFloat = tempToFloat(temp);          // → 25.5f
  Temperature_t fromWhole = tempFromWhole(20); // → 200
  ```
- **Arithmetic**: `tempAdd()`, `tempSub()`, `tempAbs()`, `tempDivInt()`
- **Comparison**: `tempGreater()`, `tempLess()`, `tempIsValid()`

### Time Utilities (Utils.h)
- `Utils::elapsedMs(startTime)` - Safe elapsed time calculation (handles overflow)
- `Utils::hasTimedOut(startTime, timeoutMs)` - Timeout check helper

### Pressure Type (Pressure_t)
- **Type**: `int16_t` (hundredths of BAR)
- **Range**: -327.68 to +327.67 BAR with 0.01 BAR precision
- **Example**: 150 = 1.50 BAR, 235 = 2.35 BAR
- **Sensor**: 4-20mA current loop on MB8ART channel 7
- **Build Flag**: `USE_REAL_PRESSURE_SENSOR` (undefined = fake data with compile warning)
- **Safety Limits**:
  - Operating: 1.00-3.50 BAR (100-350 hundredths)
  - Alarms: 0.50-4.00 BAR (50-400 hundredths)

### Benefits of Fixed-Point
- ✅ No floating-point CPU cycles in control loops
- ✅ Deterministic results (no IEEE 754 precision issues)
- ✅ Smaller memory footprint (2 bytes vs 4 bytes)
- ✅ Faster arithmetic operations
- ✅ MQTT sends as integers (no float serialization)

## Configuration Management

### SystemConstants.h Organization
All system parameters centralized in organized namespaces:

- **Safety::Pressure** - Operating limits (100-350) and alarms (50-400)
- **Simulation** - Fake sensor ranges and update intervals
- **Temperature::SpaceHeating** - Comfort/Eco/Frost defaults (210/180/100)
- **PID::Autotune** - Min/max cycles, timeouts, amplitude defaults
- **Burner** - Error logging intervals, anti-flapping timers
- **Hardware::PressureSensor** - 4-20mA calibration constants

### Build Flags (ProjectConfig.h)
- `USE_REAL_PRESSURE_SENSOR` - Enable real pressure sensor (undefine for testing)
- `LOG_MODE_DEBUG_FULL` - Full debug logging (larger stacks)
- `LOG_MODE_DEBUG_SELECTIVE` - Selective debug (optimized stacks)
- `LOG_MODE_RELEASE` - Production mode (minimal stacks, aggressive optimization)

### Stack Sizes by Mode
Critical tasks with safety margins:
- **PID Control**: 4096 bytes (debug), 2048 bytes (release)
- **Monitoring**: 4096 bytes (debug), 3072 bytes (release)
- **Burner Control**: 3072 bytes (debug selective), 1536 bytes (release)

## Deep Code Analysis (7 Rounds Complete)

Comprehensive code analysis performed Nov-Dec 2025. See `docs/DEEP_CODE_ANALYSIS_HISTORY.md` for full details.

### Round 1: Concurrency & Safety
- RAII mutex guards in SafetyInterlocks
- Timer cleanup handlers in all control tasks
- Fixed millis() wraparound in TemperatureSensorFallback

### Round 2: Memory & Performance
- Fixed-point conversion (QueueMetrics, HealthMonitor)
- Stack optimization: 4KB RAM recovered
- PID improvements: derivative-on-PV, conditional anti-windup

### Round 3: Resilience
- Clear all 14 event groups on startup
- Modbus error detection (0xFFFF, 0x0000)
- Queue message restoration with xQueueSendToFront

### Round 4: Control Logic & Equipment Protection
- Pump motor protection (30s minimum state change)
- Sensor hysteresis (3 valid start, 2 invalid stop)
- Burner request expiration watchdog (10 minutes)
- Override flags persist to NVS

### Round 5: Communication & Monitoring
- MQTT command deduplication (8-entry hash cache)
- Subscription retry mechanism (every 5s)
- NTP/DS3231 RTC fallback after 5 failures
- ANDRTF3 direct read fallback on coordinator timeout

### Round 6: Security & Defensive Programming
- Input validation: strtof() with endptr (not atof)
- tempAbs() INT16_MIN overflow protection
- Schedule time/temperature bounds validation
- Division-by-zero guards

### Round 7: Performance & Timing (Dec 2025)
- **FRAM non-blocking**: taskYIELD() instead of delay(1)
- **Sensor staleness detection**: 15s max age before blocking burner
- **BurnerControlTask priority**: Elevated from 3 to 4 (safety-critical)
- **NVS recovery**: eraseNamespace() on corruption, graceful fallback
- **MQTT critical bypass**: PRIORITY_CRITICAL messages bypass queue under pressure

### Memory Improvements
- **4.5KB+ RAM saved** through flash string storage and adaptive sensors
- **Stack analysis completed** - increased sizes for tasks showing <100 bytes free
- **Memory pools** for MQTT messages (512/256 byte blocks)
- **Priority-based queue dropping** for MQTT overflow handling

## Safety Features

### Sensor Validation
- **Staleness detection**: Sensor data older than 15s blocks burner operation
- **Hysteresis**: DISABLED (set to 1) - sensors are stable and accurate in this installation
  - _Archived: Round 4 added 3/2 hysteresis for flaky sensor protection, removed Dec 2025_
  - _Code structure preserved in TemperatureSensorFallback.h/cpp for future use_
- **Modbus error detection**: 0xFFFF, 0x0000, 0x7530 treated as invalid
- **Fallback modes**: Graceful degradation when sensors fail

### Equipment Protection
- **Pump motor protection**: 30s minimum between on/off transitions
- **Burner anti-flapping**: Minimum on-time (2 min) and off-time (20s)
- **Request expiration**: Burner requests expire after 10 minutes (watchdog)
- **Relay rate limiting**: Prevents rapid toggling

### Error Recovery
- **NVS corruption**: Automatic namespace erase and recovery attempt
- **NTP failure**: Falls back to DS3231 RTC after 5 failures
- **Modbus timeout**: ANDRTF3 falls back to direct read after 30s
- **MQTT reconnect**: Exponential backoff with subscription retry

### Task Priorities
| Task | Priority | Notes |
|------|----------|-------|
| BurnerControlTask | 4 | Safety-critical, highest control priority |
| RelayControlTask | 4 | Direct hardware control |
| ModbusControlTask | 4 | Hardware communication |
| HeatingControlTask | 3 | Control logic |
| WheaterControlTask | 3 | Control logic |
| MQTTTask | 2 | Non-critical communication |
| MonitoringTask | 2 | Diagnostics |

### MQTT Message Priorities
| Priority | Level | Behavior |
|----------|-------|----------|
| CRITICAL | 0 | Bypasses queue when under pressure |
| HIGH | 1 | Processed first, never throttled |
| MEDIUM | 2 | Throttled at 80% queue utilization |
| LOW | 3 | Throttled at 50% queue utilization |

### Persistent Storage (FRAM)
- **CriticalDataStorage**: Emergency states, PID tuning, error logs, runtime counters
- **Memory map**: 0x4C20-0x7FFF reserved for critical data
- **CRC32 validation**: All stored structures protected
- **Integration**: CentralizedFailsafe saves emergency state on shutdown

### MQTT Enhancements
- **Sensor data**: Compact JSON with fixed-point values
  - Format: `{"t":{"bo":254,"br":253,...},"p":150,"r":21}`
  - Pressure published as integer (150 = 1.50 BAR)
- **Diagnostics**: FreeRTOS task monitoring with stack depth reporting
- **Priority queues**: High priority for sensor data, normal for status

## Build Notes
- Need to run `rm -rf .pio` before building to ensure clean build environment
- Compile warnings expected: "Using FAKE pressure sensor data" (when USE_REAL_PRESSURE_SENSOR not defined)

## ESP32 Serial Monitoring & Development

### Serial Port Issues
- PlatformIO monitor can fail with `termios.error: (25, 'Inappropriate ioctl for device')` when run in non-interactive shells
- Use alternative monitoring methods:
  ```python
  import serial
  ser = serial.Serial('/dev/ttyACM0', 921600, timeout=1)
  while True:
      if ser.in_waiting:
          print(ser.readline().decode('utf-8', errors='ignore'))
  ```
- minicom works but may have display issues: `minicom -D /dev/ttyACM0 -b 921600`
- Serial port conflicts: Check for other processes using the port with `fuser /dev/ttyACM0`

### Network Configuration (Updated Nov 2025)
- **Device IP**: 192.168.20.40 (static)
- **MQTT Broker**: 192.168.20.27
- **Gateway/NTP**: 192.168.20.1
- **OTA Host**: 192.168.20.16

Static IP is configured in `src/config/ProjectConfig.h` via:
- `USE_STATIC_IP` - Enable static IP mode
- `ETH_STATIC_IP`, `ETH_GATEWAY`, `ETH_SUBNET`, `ETH_DNS1`, `ETH_DNS2`

### Device Testing Workflow
1. **Upload firmware**: `pio run -e esp32dev_usb_debug_selective -t upload --upload-port /dev/ttyACM0`
2. **Wait for boot**: Device takes ~30 seconds to fully initialize
3. **Check connectivity**: `ping 192.168.20.40`
4. **Verify MQTT**: `mosquitto_sub -h 192.168.20.27 -u YOUR_MQTT_USER -P YOUR_MQTT_PASSWORD -t "#" -C 5`
5. **Monitor boot log**: Save to file for analysis (`/tmp/bootlog`)

### MQTT Testing with mosquitto_pub/sub
```bash
# Monitor all topics
mosquitto_sub -h 192.168.20.27 -u YOUR_MQTT_USER -P YOUR_MQTT_PASSWORD -t "#" -v

# Send commands with timeout
timeout 5 mosquitto_sub -h 192.168.20.27 -t "topic" -C 1 -W 5

# Check device IP
mosquitto_sub -h 192.168.20.27 -u YOUR_MQTT_USER -P YOUR_MQTT_PASSWORD -t "boiler/status/device/ip" -C 1
```

## Scheduler System Architecture

### Generic TimerScheduler Design
- **Extensible**: New schedule types by implementing `IScheduleAction`
- **Persistent**: Uses RuntimeStorage (FRAM) with CRC validation
- **Event-driven**: Publishes MQTT events on schedule start/end
- **Type-safe**: Uses union in `TimerSchedule` for type-specific data

### MQTT API Overview
All MQTT topics use the `boiler/` prefix:
```
Commands:   boiler/cmd/{system,heating,water,room_target,fram,status,...}
Status:     boiler/status/{sensors,health,heating,water,system,...}
Scheduler:  boiler/cmd/scheduler/{add,remove,list,status}
Parameters: boiler/params/{wheater,heating,pid}/...
```

### MQTT Parameter API
```
boiler/params/get/all              - Request all parameter values
boiler/params/save                 - Save all parameters to NVS
boiler/params/wheater/{param}      - Water heater settings
boiler/params/heating/{param}      - Space heating settings
boiler/params/pid/spaceHeating/... - Space heating PID gains
boiler/params/pid/waterHeater/...  - Water heater PID gains
```

### MQTT Scheduler API
```
Commands: boiler/cmd/scheduler/{add,remove,list,status}
Response: boiler/scheduler/response
Events:   boiler/scheduler/event
Status:   boiler/status/scheduler/info
```

### Key Lessons

#### System Initialization
1. **RuntimeStorage must be initialized** before TimerSchedulerTask
2. **ServiceContainer registration** is critical - check service names match
3. **Namespace collision**: RuntimeStorage namespace + class causes IntelliSense issues
   - Solution: Use type alias `using RuntimeStoragePtr = class RuntimeStorage::RuntimeStorage*;`

#### MQTT & Communication
4. **MQTT topic routing** uses last path segment as command
5. **Priority queues** prevent sensor data loss during MQTT flooding
6. **Fixed-point in JSON** - ArduinoJson handles int16_t natively as integers

#### Persistence & Storage
7. **Schedule persistence** works better with FRAM than Flash (unlimited writes)
8. **CRC validation** essential for FRAM reliability (no ECC like EEPROM)
9. **DS3231 modules** often include AT24C32 EEPROM but DS3231Controller doesn't use it

#### Performance & Safety
10. **Stack monitoring** critical - some tasks had only 60 bytes free!
11. **Fixed-point arithmetic** provides deterministic, faster temperature/pressure handling
12. **Configuration centralization** in SystemConstants.h eliminates magic numbers
13. **Build flags** (`USE_REAL_PRESSURE_SENSOR`) prevent accidental fake data in production

## ESP32 Library Ecosystem

All libraries are published under the **packerlschupfer** GitHub account with **ESP32-** prefix for consistency.

Located in `~/Documents/PlatformIO/libs/ESP32-*`:

| Library | Version | GitHub | Purpose |
|---------|---------|--------|---------|
| ESP32-LibraryCommon | 0.1.0 | [GitHub](https://github.com/packerlschupfer/ESP32-LibraryCommon) | Result<T> error handling |
| ESP32-Logger | 0.1.0 | [GitHub](https://github.com/packerlschupfer/ESP32-Logger) | Thread-safe async logging |
| ESP32-MutexGuard | 0.1.0 | [GitHub](https://github.com/packerlschupfer/ESP32-MutexGuard) | RAII mutex guard |
| ESP32-SemaphoreGuard | 0.1.0 | [GitHub](https://github.com/packerlschupfer/ESP32-SemaphoreGuard) | RAII semaphore guard |
| ESP32-Watchdog | 0.1.0 | [GitHub](https://github.com/packerlschupfer/ESP32-Watchdog) | Task watchdog timer |
| ESP32-TaskManager | 0.1.0 | [GitHub](https://github.com/packerlschupfer/ESP32-TaskManager) | Task lifecycle management |
| ESP32-EthernetManager | 0.1.0 | [GitHub](https://github.com/packerlschupfer/ESP32-EthernetManager) | LAN8720A Ethernet manager |
| ESP32-OTAManager | 0.1.0 | [GitHub](https://github.com/packerlschupfer/ESP32-OTAManager) | OTA firmware updates |
| ESP32-NTPClient | 0.1.0 | [GitHub](https://github.com/packerlschupfer/ESP32-NTPClient) | Multi-server NTP with DST |
| ESP32-PersistentStorage | 0.1.0 | [GitHub](https://github.com/packerlschupfer/ESP32-PersistentStorage) | NVS parameter storage |
| ESP32-RuntimeStorage | 0.1.0 | [GitHub](https://github.com/packerlschupfer/ESP32-RuntimeStorage) | FRAM runtime storage |
| ESP32-MQTTManager | 0.1.0 | [GitHub](https://github.com/packerlschupfer/ESP32-MQTTManager) | MQTT client wrapper |
| ESP32-IDeviceInstance | 0.1.0 | [GitHub](https://github.com/packerlschupfer/ESP32-IDeviceInstance) | Abstract device interface |
| ESP32-ModbusDevice | 0.1.0 | [GitHub](https://github.com/packerlschupfer/ESP32-ModbusDevice) | Modbus RTU framework |
| ESP32-MB8ART | 0.1.0 | [GitHub](https://github.com/packerlschupfer/ESP32-MB8ART) | 8-channel temperature module |
| ESP32-RYN4 | 0.1.0 | [GitHub](https://github.com/packerlschupfer/ESP32-RYN4) | 8-channel relay module |
| ESP32-ANDRTF3 | 0.1.0 | [GitHub](https://github.com/packerlschupfer/ESP32-ANDRTF3) | Room temperature sensor |
| ESP32-DS3231Controller | 0.1.0 | [GitHub](https://github.com/packerlschupfer/ESP32-DS3231Controller) | RTC with scheduling |

### External Dependencies
| Library | Purpose |
|---------|---------|
| esp32ModbusRTU | Non-blocking Modbus RTU (external) |
| ESP32MQTTClient | ESP-IDF MQTT client (external) |
| ArduinoJson | JSON serialization (external) |
| Adafruit RTClib | DS3231 RTC support (external) |

### Library Development Notes
- Libraries use `git+file://${env.HOME}/Documents/PlatformIO/libs/ESP32-*` references for local development
- For GitHub-based development, use GitHub URLs in platformio.ini
- Run `pio pkg update` after library changes
- All libraries v0.1.0 - production-tested, initial public release