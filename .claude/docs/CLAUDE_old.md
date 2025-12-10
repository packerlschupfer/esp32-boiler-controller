# ESPlan Blueprint Boiler Controller (MB8ART) v2.1.0

This is an ESP32-based industrial heating system controller that manages a gas boiler, water heater, and space heating system using Modbus RTU for hardware communication and MQTT for remote monitoring.

## Claude Code Instructions

First append to the tasklog.txt the contents of claudeInstructions.txt
then append the task todo list 
finally when the task is complete, append the description of the what was done to the tasklog.txt file

## Project Overview
- **Board**: ESP32 with LAN8720 Ethernet PHY
- **Framework**: Arduino
- **Build System**: PlatformIO
- **Main Purpose**: Industrial boiler control with weather compensation
- **Communication**: Modbus RTU (RS485) + MQTT + OTA updates

## External Libraries Location
All custom libraries are located in: `/home/mrnice/git/vscode-workspace-libs/`

### Available Libraries:
- **workspace_Class-ATC_MiThermometer**: Xiaomi Mi Thermometer (ATC firmware) BLE integration
- **workspace_Class-EthernetManager**: Ethernet connectivity management with LAN8720 PHY
- **workspace_Class-IDeviceInstance**: Device instance interface/base class
- **workspace_Class-Logger**: Logging functionality for debugging
- **workspace_Class-MB8ART**: 8-channel temperature sensor module communication (PT1000 support)
- **workspace_Class-ModbusDevice**: Modbus RTU protocol implementation
- **workspace_Class-MQTTManager**: MQTT client for remote monitoring/control
- **workspace_Class-NimBLE-Arduino**: NimBLE Bluetooth Low Energy stack
- **workspace_Class-OTAManager**: Over-The-Air update management (port 3232)
- **workspace_Class-RYN4**: 8-channel relay control module communication
- **workspace_Class-SemaphoreGuard**: RTOS semaphore utilities for thread safety
- **workspace_Class-TaskManager**: FreeRTOS task management wrapper

## Hardware Interfaces

### MB8ART Temperature Sensor Module (Modbus Address: 0x01)
- 8-channel PT1000 temperature sensor support
- High-resolution mode: 0.01°C precision
- Fast initialization: ~200ms
- Active channels: Ch0, Ch4
- Used for monitoring all system temperatures

### RYN4 Relay Control Module (Modbus Address: 0x02)
- 8-channel relay outputs
- Relay mappings:
  - R1: Heating circulation pump
  - R2: Water heating circulation pump
  - R3: Burner enable (main on/off)
  - R4: Water heating mode (disables hardware safety when ON)
  - R5: Power level selector (ON=half power, OFF=full power)
  - R6: Valve control
  - R7-R8: Spare

## System Architecture

### Control Modules

1. **HeatingControlModule**: Space heating with weather compensation
   - Calculates target temperatures based on heating curves
   - Sends PID adjustments to burner control
   - Weather compensation algorithm based on outdoor temperature

2. **WaterControlModule**: Domestic hot water control
   - Priority over space heating when enabled
   - Temperature differential control
   - Safety limits and hysteresis

3. **BurnerControlModule**: Gas burner operation
   - 3-Point Bang-Bang control (OFF/HALF/FULL)
   - Maps 7-level PID output (0-6) to 3 burner states
   - State tracking prevents redundant relay commands
   - Safety interlocks and emergency shutdown
   - Pump interlock ensures circulation before ignition

4. **PIDControlModule**: Temperature regulation
   - Separate PID loops for heating and water
   - Configurable Kp, Ki, Kd parameters
   - 7-level output (0=off through 6=maximum)

5. **PumpControlModule**: Circulation pump control
   - Automatic pump control based on burner state
   - Post-circulation timers
   - Safety features

### FreeRTOS Tasks
- **BurnerControlTask**: Receives PID adjustments and controls burner state
- **RelayControlTask**: Manages relay command queue and Modbus communication
- **MonitoringTask**: System health and state reporting
- **OTATask**: Over-the-air update service
- **Various control tasks**: For heating, water, and PID control

### Communication Systems
- **Ethernet**: LAN8720 PHY for network connectivity
- **MQTT**: 
  - Default broker: test.mosquitto.org:1883
  - Status reporting with automatic reconnection
  - Sensor data publishing
  - Remote command reception
- **Modbus RTU**: 
  - RS485 communication at 9600 baud
  - Robust error handling
  - Device registration system

## Key Features

### Burner Control States
- **OFF**: All burner relays off
- **HEATING HALF**: R1+R3+R5 (pump + burner + half power)
- **HEATING FULL**: R1+R3 (pump + burner, R5 off for full power)
- **WATER HALF**: R2+R3+R4+R5
- **WATER FULL**: R2+R3+R4

### Safety Systems
- Temperature limits and monitoring
- Pump interlock before burner operation
- Emergency shutdown capability
- Hardware safety override for water heating mode
- Watchdog monitoring on all critical tasks

### Smart Relay Control
- State tracking prevents unnecessary switching
- Queued command processing
- Automatic retry on communication errors

## Configuration Files
- Heating curves configurable via system settings
- PID parameters adjustable for both heating and water
- Temperature limits and hysteresis settings
- MQTT topics for remote configuration
- Relay assignments defined in RelayFunctionDefs.h

## Temperature Monitoring Points
- **Boiler**: Output and return temperatures
- **Water heater**: Tank, output, and return temperatures
- **Heating system**: Return temperature
- **Environment**: Inside and outside temperatures

## Recent Improvements
- Added state checking to prevent redundant relay switching
- Implemented centralized relay function definitions
- Fixed event group initialization for stable operation
- Optimized Modbus communication with proper device registration

## Development Notes
- Event-driven architecture using FreeRTOS
- Clean separation of concerns with modular design
- Centralized relay function definitions for maintainability
- All critical paths have error handling and recovery
