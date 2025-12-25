# ESP32 Boiler Controller

Industrial-grade boiler control system for ESP32 with advanced safety features, MQTT monitoring, and PID temperature control.

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Platform](https://img.shields.io/badge/platform-ESP32-blue.svg)](https://www.espressif.com/en/products/socs/esp32)
[![Framework](https://img.shields.io/badge/framework-Arduino%2FESP--IDF-green.svg)](https://docs.espressif.com/projects/arduino-esp32/)

## ⚠️ Safety Warning

This system controls combustion equipment and must be installed by qualified professionals. Improper installation or configuration can result in fire, explosion, carbon monoxide poisoning, or equipment damage. **Use at your own risk.**

## Features

### Safety Systems (5 Layers)
- **BurnerSafetyValidator**: Pre-operation validation with runtime-configurable parameters
- **SafetyInterlocks**: Continuous monitoring with configurable sensor staleness detection
- **CentralizedFailsafe**: Coordinated emergency shutdown
- **DELAY Watchdog**: Hardware-enforced relay auto-OFF (10s) protects against ESP32 failures
- **Hardware Interlocks**: Physical safety sensors (future)
- **Runtime Configuration**: Safety parameters adjustable via MQTT (sensor staleness, pump protection, post-purge duration)

### Control Systems
- **Event-driven architecture**: 18 FreeRTOS tasks, zero polling loops
- **PID control**: Space and water heating with anti-windup
- **Fixed-point arithmetic**: Temperature_t (0.1°C), Pressure_t (0.01 BAR)
- **Burner state machine**: 8 states with anti-flapping protection
- **Equipment protection**: Configurable pump motor protection (5-60s, default 15s), 2min burner minimum runtime

### Communication
- **MQTT**: Complete monitoring and control API
- **Modbus RTU**: MB8ART (8-ch temp), RYN4 (8-ch relay), ANDRTF3 (room temp)
- **NTP**: Multi-server time sync with DS3231 RTC fallback
- **OTA**: Firmware updates over Ethernet

### Data Persistence
- **NVS**: Parameter storage with MQTT access
- **FRAM**: Runtime data, PID state, error logging (MB85RC256V)
- **CRC32**: Data integrity on all persistent structures

## Hardware Requirements

### Required Components
- **ESP32-DevKitC** or compatible
- **LAN8720A** Ethernet PHY module
- **MB8ART** 8-channel temperature sensor (Modbus RTU)
  - Supports LOW_RES (0.1°C) or HIGH_RES (0.01°C) modes
  - Automatically adapts to configured sensor mode
  - Values stored as tenths of degree (Temperature_t)
- **RYN4** 8-channel relay module (Modbus RTU)
- **RS485 Transceiver** (MAX485 or similar)

### Optional Components
- **ANDRTF3/MD** wall-mount room temperature sensor
- **DS3231** RTC module (I2C) - for time backup
- **MB85RC256V** FRAM (I2C) - for enhanced data logging
- **Flame sensor** - ionization rod or UV detector (recommended for unattended operation)
- **Flow sensor** - for water flow verification (recommended for production)
- **Pressure sensor** - 4-20mA transducer (recommended for production)

### Wiring
- **Ethernet**: LAN8720A PHY (MDC=23, MDIO=18, CLK=17)
- **Modbus RTU**: RS485 (RX=36, TX=4, 9600 baud)
- **I2C**: SDA=21, SCL=22 (DS3231, FRAM)
- **Relays**: Controlled via RYN4 Modbus module

## Installation

### PlatformIO (Recommended)

```bash
# Clone repository
git clone https://github.com/packerlschupfer/esp32-boiler-controller.git
cd esp32-boiler-controller

# Copy and configure credentials (optional - auto-created if missing)
cp credentials.example.ini credentials.ini
# Edit credentials.ini with your MQTT credentials
# Note: Build system auto-generates minimal credentials.ini if missing

# Build and upload
pio run -e esp32dev_usb_debug_selective -t upload --upload-port /dev/ttyACM0

# Monitor serial output
pio device monitor -b 921600
```

### Configuration

1. **Network Settings** (`src/config/ProjectConfig.h`):
   - Set static IP: `ETH_STATIC_IP`, `ETH_GATEWAY`, `ETH_SUBNET`
   - Configure MQTT broker: `MQTT_SERVER`

2. **Hardware Pins** (`src/config/ProjectConfig.h`):
   - Verify Ethernet PHY pins match your hardware
   - Verify RS485 pins for Modbus

3. **Credentials** (`credentials.ini`):
   ```ini
   [env]
   build_flags =
       -DMQTT_USERNAME=\"your_mqtt_user\"
       -DMQTT_PASSWORD=\"your_mqtt_password\"
       -DOTA_PASSWORD=\"your_ota_password\"
   ```

4. **Build Mode** (`platformio.ini`):
   - **Debug Selective**: Development (default)
   - **Debug Full**: Deep troubleshooting
   - **Release**: Production (optimized)

## MQTT API

### Status Topics (Published)
```
boiler/status/sensors          - Temperature/pressure JSON every 10s
boiler/status/health           - System health every 60s
boiler/status/system           - enabled/disabled
boiler/status/heating          - Heating state
boiler/status/water            - Water heating state
boiler/status/burner           - Burner state machine state
boiler/status/safety_config    - Safety configuration (pump_prot, sensor_stale, post_purge)
boiler/status/device/ip        - Device IP address
boiler/status/device/firmware  - Firmware version
```

### Command Topics (Subscribed)
```
boiler/cmd/system                       - on/off/reboot
boiler/cmd/heating                      - on/off/override_on/override_off
boiler/cmd/water                        - on/off/override_on/override_off
boiler/cmd/config/pump_protection_ms    - Set pump protection delay (5000-60000ms)
boiler/cmd/config/sensor_stale_ms       - Set sensor staleness timeout (30000-300000ms)
boiler/cmd/config/post_purge_ms         - Set post-purge duration (30000-180000ms)
boiler/cmd/scheduler/add                - Add schedule (JSON)
boiler/cmd/scheduler/remove             - Remove schedule by ID
```

### Parameter Topics
```
boiler/params/heating/setpoint       - Room temperature setpoint
boiler/params/wheater/tempLimitLow   - Water start threshold
boiler/params/wheater/tempLimitHigh  - Water stop threshold
boiler/params/pid/spaceHeating/kp    - PID gains
boiler/params/get/all                - Request all parameters
boiler/params/save                   - Save to NVS
```

See [docs/MQTT_API.md](docs/MQTT_API.md) for complete reference.

## Architecture

### Task Structure (16 FreeRTOS Tasks)

**Safety-Critical (Priority 4)**:
- BurnerControlTask - State machine, safety interlocks
- RelayControlTask - Physical relay control

**Control Logic (Priority 3)**:
- HeatingControlTask - Space heating PID
- WheaterControlTask - Water heating PID
- SensorTasks - MB8ART, ANDRTF3

**Communication (Priority 2)**:
- MQTTTask - MQTT with priority queues
- MonitoringTask - System diagnostics

**Background (Priority 1)**:
- OTATask - Firmware updates

See [docs/TASK_ARCHITECTURE.md](docs/TASK_ARCHITECTURE.md) for details.

### State Machines

**Burner States**:
```
IDLE → PRE_PURGE → IGNITION → RUNNING_LOW/HIGH → POST_PURGE → (IDLE | LOCKOUT | ERROR)
```

See [docs/STATE_MACHINES.md](docs/STATE_MACHINES.md) for complete state diagrams.

## Memory Optimizations

**Achievements** (20 rounds of analysis):
- 6.7KB+ RAM recovered through optimizations
- Fixed-point arithmetic (no floating point in control loops)
- Stack tuning based on runtime profiling
- Event-driven architecture (zero polling loops)

See [docs/DEEP_CODE_ANALYSIS_HISTORY.md](docs/DEEP_CODE_ANALYSIS_HISTORY.md) for complete analysis.

## Production Status

**Field-Tested**: ✅
- Weeks of continuous operation
- Zero watchdog resets
- Industrial application deployment
- 20 rounds of deep code analysis
- 395+ issues identified and addressed

**Production Readiness**:
- ✅ Safe WITH supervision
- ⚠️ For unattended operation, install:
  - Physical flame sensor (currently uses relay proxy)
  - Flow sensor (currently uses temperature differential)
  - Pressure sensor (currently optional via flag)

## Documentation

Comprehensive technical documentation (~180KB):

- [TASK_ARCHITECTURE.md](docs/TASK_ARCHITECTURE.md) - All 18 FreeRTOS tasks (28KB)
- [INITIALIZATION_ORDER.md](docs/INITIALIZATION_ORDER.md) - 7-stage startup sequence (9.5KB)
- [MEMORY_OPTIMIZATION.md](docs/MEMORY_OPTIMIZATION.md) - ESP32 memory strategy (12KB)
- [ALGORITHMS.md](docs/ALGORITHMS.md) - 13 control algorithms (24KB)
- [STATE_MACHINES.md](docs/STATE_MACHINES.md) - Burner state machine details (20KB)
- [MQTT_API.md](docs/MQTT_API.md) - Complete MQTT reference (22KB)
- [SAFETY_SYSTEM.md](docs/SAFETY_SYSTEM.md) - 4-layer safety architecture
- [EVENT_SYSTEM.md](docs/EVENT_SYSTEM.md) - Event group definitions (14KB)
- [MUTEX_HIERARCHY.md](docs/MUTEX_HIERARCHY.md) - Thread safety (5KB)

## Library Ecosystem

This project uses 18 custom ESP32 libraries (all published separately):

**Foundation**: [ESP32-LibraryCommon](https://github.com/packerlschupfer/ESP32-LibraryCommon), [ESP32-Logger](https://github.com/packerlschupfer/ESP32-Logger), [ESP32-MutexGuard](https://github.com/packerlschupfer/ESP32-MutexGuard), [ESP32-SemaphoreGuard](https://github.com/packerlschupfer/ESP32-SemaphoreGuard)

**Core**: ESP32-Watchdog, ESP32-TaskManager, ESP32-EthernetManager, ESP32-OTAManager, ESP32-NTPClient, ESP32-PersistentStorage, ESP32-RuntimeStorage

**Framework**: ESP32-MQTTManager, ESP32-IDeviceInstance, ESP32-ModbusDevice

**Hardware**: ESP32-MB8ART, ESP32-RYN4, ESP32-ANDRTF3, ESP32-DS3231Controller

All available at: https://github.com/packerlschupfer

## Building

### Development Build
```bash
# Debug with selective logging (default)
pio run -e esp32dev_usb_debug_selective

# Full debug output
pio run -e esp32dev_usb_debug_full

# Production build
pio run -e esp32dev_usb_release
```

### Uploading
```bash
# USB upload
pio run -e esp32dev_usb_debug_selective -t upload --upload-port /dev/ttyACM0

# OTA upload (after initial USB flash)
pio run -e esp32dev_ota_debug_selective -t upload
```

### Clean Build
```bash
# Always recommended after library updates
rm -rf .pio
pio run
```

## Project Structure

```
esp32-boiler-controller/
├── src/                    # Source code
│   ├── main.cpp           # Entry point
│   ├── config/            # Configuration (ProjectConfig.h, SystemConstants.h)
│   ├── core/              # Core systems (SRP, QueueManager, StateManager)
│   ├── modules/           # Modules (tasks, control, MQTT, scheduler)
│   ├── shared/            # Shared resources (Temperature.h, Pressure.h)
│   ├── utils/             # Utilities (ErrorHandler, CriticalDataStorage)
│   └── init/              # Initialization (SystemInitializer)
├── include/               # Headers
├── docs/                  # Documentation (~180KB technical docs)
├── scripts/               # Utility scripts
│   ├── testing/          # Test and monitoring scripts
│   └── ...               # MQTT test scripts, parameter init
├── tools/                 # Development tools
│   ├── analysis/         # Code analysis reports (20 rounds)
│   └── monitoring/       # System monitoring utilities
├── test/                  # Unit and integration tests
├── homeassistant/         # Home Assistant integration
└── platformio.ini         # PlatformIO configuration
```

## Contributing

This is a production system - contributions should maintain:
1. **Safety-first design** - All changes reviewed for safety impact
2. **Thread safety** - Follow RAII patterns, mutex hierarchy
3. **Fixed-point arithmetic** - No float in control loops
4. **Event-driven** - No polling loops
5. **Documentation** - Update relevant docs

Pull requests welcome!

## License

GPL-3 License - See [LICENSE](LICENSE) file for details.

## Author

**packerlschupfer**
- GitHub: [@packerlschupfer](https://github.com/packerlschupfer)

## Acknowledgments

- **esp32ModbusRTU**: Alexander Emelianov
- **ESP32MQTTClient**: Chen Yijun
- **ArduinoJson**: Benoit Blanchon
- **Adafruit RTClib**: Adafruit Industries

## Related Projects

- [ESP32-MB8ART](https://github.com/packerlschupfer/ESP32-MB8ART) - Temperature sensor driver
- [ESP32-RYN4](https://github.com/packerlschupfer/ESP32-RYN4) - Relay module driver
- [ESP32-TaskManager](https://github.com/packerlschupfer/ESP32-TaskManager) - FreeRTOS task management

See full library ecosystem: https://github.com/packerlschupfer?tab=repositories

## Support

For issues or questions:
- **GitHub Issues**: [Report bugs or request features](https://github.com/packerlschupfer/esp32-boiler-controller/issues)
- **Documentation**: See [docs/](docs/) directory for comprehensive technical documentation

---

**⚠️ Important**: This is industrial control software managing combustion equipment. Professional installation and regular safety inspections are mandatory. Not for DIY use without proper qualifications.
