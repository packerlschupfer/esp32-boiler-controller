# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.0] - 2025-12-06

### Initial Public Release

First public release on GitHub after extensive private development and field testing.

**Production Status**: Field-tested in industrial application with weeks of continuous operation.

### Features

#### Safety Systems
- Multi-layered safety architecture (4 layers: BurnerSafetyValidator, SafetyInterlocks, CentralizedFailsafe, Hardware)
- **Runtime-configurable safety parameters** via MQTT (SafetyConfig module)
  - Sensor staleness timeout: 30-300s (default 60s)
  - Pump motor protection: 5-60s (default 15s)
  - Post-purge duration: 30-180s (default 90s)
  - Persisted in NVS, published to `boiler/status/safety_config`
- Streamlined validation (removed redundant checks: rate-of-change, cross-validation, thermal runaway)
- Equipment protection: 2min burner anti-flapping, configurable pump protection
- Emergency shutdown with FRAM state persistence
- Pressure monitoring (1.00-3.50 BAR operating range)
- Hard limits: 90°C over-temp, pump interlock, sensor validity checks

#### Control Systems
- Event-driven FreeRTOS architecture (16 tasks, zero polling loops)
- Burner state machine (8 states: IDLE, PRE_PURGE, IGNITION, RUNNING_LOW/HIGH, POST_PURGE, LOCKOUT, ERROR)
- PID control for space and water heating with anti-windup
- Fixed-point arithmetic (Temperature_t, Pressure_t) for deterministic control
- Mode switching with priority handling (water can preempt heating)

#### Communication
- MQTT monitoring and control interface
- Modbus RTU integration (MB8ART 8-ch temp, RYN4 8-ch relay, ANDRTF3 room temp)
- NTP time synchronization with DS3231 RTC fallback
- OTA firmware updates over Ethernet

#### Data Persistence
- NVS parameter storage with MQTT access (boiler/params/...)
- FRAM runtime storage (MB85RC256V) for PID state, counters, error logs
- CRC32 validation on all persistent structures
- Schedule storage with power-loss recovery

#### Development
- 20 rounds of deep code analysis (395+ issues addressed)
- 6.7KB+ memory optimizations
- Comprehensive documentation (~150KB)
- Production-tested with zero watchdog resets

### Hardware Support
- ESP32-DevKitC or compatible
- LAN8720A Ethernet PHY
- MB8ART 8-channel temperature sensor (Modbus RTU)
- RYN4 8-channel relay module (Modbus RTU)
- ANDRTF3 room temperature sensor (Modbus RTU, optional)
- DS3231 RTC module (I2C, optional)
- MB85RC256V FRAM (I2C, optional)

### Dependencies
All custom libraries published separately under GPL-3 license:
- ESP32-LibraryCommon, ESP32-Logger, ESP32-MutexGuard, ESP32-SemaphoreGuard
- ESP32-Watchdog, ESP32-TaskManager, ESP32-EthernetManager, ESP32-OTAManager
- ESP32-NTPClient, ESP32-PersistentStorage, ESP32-RuntimeStorage
- ESP32-MQTTManager, ESP32-IDeviceInstance, ESP32-ModbusDevice
- ESP32-MB8ART, ESP32-RYN4, ESP32-ANDRTF3, ESP32-DS3231Controller

### Notes
- Previous internal versions (v1.x-v2.x) not publicly released
- Reset to v0.1.0 for clean public release start
- All libraries also released as v0.1.0 simultaneously
- Comprehensive documentation included in docs/ directory
