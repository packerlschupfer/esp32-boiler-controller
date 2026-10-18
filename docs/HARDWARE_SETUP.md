# Hardware Setup Guide

This document provides complete hardware wiring and configuration instructions for the ESP32 Boiler Controller.

---

## Table of Contents

1. [Required Components](#required-components)
2. [ESP32 Pin Configuration](#esp32-pin-configuration)
3. [RS485 Modbus Bus Wiring](#rs485-modbus-bus-wiring)
4. [LAN8720A Ethernet Wiring](#lan8720a-ethernet-wiring)
5. [Relay Module (RYN4) Configuration](#relay-module-ryn4-configuration)
6. [Temperature Sensor (MB8ART) Configuration](#temperature-sensor-mb8art-configuration)
7. [Room Temperature Sensor (ANDRTF3) Configuration](#room-temperature-sensor-andrtf3-configuration)
8. [Complete System Diagram](#complete-system-diagram)
9. [Power Supply Considerations](#power-supply-considerations)
10. [Troubleshooting](#troubleshooting)

---

## Required Components

### Core Components (Required)

| Component | Model | Quantity | Purpose |
|-----------|-------|----------|---------|
| ESP32 | DevKitC-type board with 16MB flash | 1 | Main controller (build uses `board_build.flash_size = 16MB` and `hardware/partitions_16MB.csv`) |
| Ethernet PHY | LAN8720A module | 1 | Network connectivity |
| Relay Module | RYN4 8-channel | 1 | Burner and pump control |
| Temperature Sensor | MB8ART 8-channel | 1 | Boiler/tank temperatures, pressure input |
| RS485 Transceiver | Auto-direction RS485 module | 1 | Modbus communication (firmware drives no DE/RE pin) |

### Optional Components

| Component | Model | Quantity | Purpose |
|-----------|-------|----------|---------|
| Room Temp Sensor | ANDRTF3 | 1 | Inside temperature |
| RTC Module | DS3231 | 1 | Scheduling (NTP fallback) |
| FRAM Module | MB85RC256V | 1 | Runtime counters, PID state, error/safety log (I2C 0x50, 32768 bytes) |
| Pressure Sensor | 4-20mA (0-5 BAR) | 1 | System pressure monitoring (MB8ART CH4) |

---

## ESP32 Pin Configuration

### GPIO Pin Assignments

```
ESP32 DevKitC Pin Map
====================

                    +------------------+
              EN  -|                  |- GPIO23 (ETH_MDC)
        GPIO36/VP -|   RS485_RX (36)  |- GPIO22
        GPIO39/VN -|                  |- GPIO21
          GPIO34  -|                  |- GPIO19
          GPIO35  -|                  |- GPIO18 (ETH_MDIO)
(I2C_SCL) GPIO32  -|                  |- GPIO5
(I2C_SDA) GPIO33  -|                  |- GPIO17 (ETH_CLK)
        GPIO25    -|                  |- GPIO16
        GPIO26    -|                  |- GPIO4  (RS485_TX)
        GPIO27    -|                  |- GPIO2  (LED_BUILTIN)
        GPIO14    -|                  |- GPIO15
        GPIO12    -|                  |- GPIO0
              GND -|                  |- GND
              VIN -|                  |- 3V3
                    +------------------+
```

### Pin Function Summary

| GPIO | Function | Direction | Notes |
|------|----------|-----------|-------|
| **RS485/Modbus** ||||
| 36 | RS485_RX | Input | RX from RS485 module RO/RX pin |
| 4 | RS485_TX | Output | TX to RS485 module DI/TX pin |
| **Ethernet (LAN8720A)** ||||
| 23 | ETH_MDC | Output | Management Data Clock |
| 18 | ETH_MDIO | Bidirectional | Management Data I/O |
| 17 | ETH_CLK | Output | 50MHz clock output |
| **I2C (Optional)** ||||
| 33 | I2C_SDA | Bidirectional | DS3231, FRAM (`src/shared/SharedI2CInitializer.h`) |
| 32 | I2C_SCL | Output | DS3231, FRAM (`src/shared/SharedI2CInitializer.h`) |
| **Status** ||||
| 2 | LED_BUILTIN | Output | Status LED |

---

## RS485 Modbus Bus Wiring

### RS485 Transceiver Wiring

```
     ESP32                 RS485 module                Modbus Bus
    +------+              (auto-direction)
    |      |                +--------+
    |  36  |<-------RX-----|        |                  +-------+
    |      |                |        |---A--------+----| Device|
    |   4  |------->TX-----|        |             |    +-------+
    |      |                |        |---B--------+    +-------+
    |  GND |--------GND----|        |             +----| Device|
    |      |                |        |                  +-------+
    | 3.3V |--------VCC----|        |
    +------+                +--------+       Termination: 120Ω between A and B
                                             at each end of the bus
```

**RS485 Module Connections:**

| Module Pin | Connection | Notes |
|------------|------------|-------|
| RX / RO | ESP32 GPIO36 | Receiver output to ESP32 (`RS485_RX_PIN`) |
| TX / DI | ESP32 GPIO4 | Driver input from ESP32 (`RS485_TX_PIN`) |
| A | Bus A (Data+) | Connect to all devices |
| B | Bus B (Data-) | Connect to all devices |
| VCC | 3.3V | Power supply |
| GND | GND | Common ground |

**Important Notes:**
- The firmware drives no DE/RE direction pin: the UART is opened as
  `Serial1.begin(MODBUS_BAUD_RATE, SERIAL_8E1, RS485_RX_PIN, RS485_TX_PIN)`
  (`src/init/HardwareInitializer.cpp`) with only RX and TX
- A transceiver module with automatic direction control is therefore required;
  a bare MAX485 with DE tied high would drive the bus permanently and block device replies
- Add 120Ω termination resistor at each end of the bus

### Modbus Device Addresses

| Device | Address | Baud Rate | Parity |
|--------|---------|-----------|--------|
| RYN4 Relay Module | 0x02 | 9600 | 8E1 |
| MB8ART Temp Sensors | 0x03 | 9600 | 8E1 |
| ANDRTF3 Room Sensor | 0x04 | 9600 | 8E1 |

### RS485 Bus Layout

```
                        Bus Termination 120Ω
                              |
    +------------------------+|+------------------------+
    |                         |                         |
+-------+              +-------+               +-------+
| RS485 |              |  RYN4 |               | MB8ART|
|ESP32  |              | Addr:2|               | Addr:3|
+-------+              +-------+               +-------+
    |                         |                         |
    +------------A------------+------------+------------+
    +------------B------------+------------+------------+
    |                         |                         |
                              |
                        Bus Termination 120Ω
```

---

## LAN8720A Ethernet Wiring

### LAN8720A Module Connections

```
     ESP32                    LAN8720A                 RJ45
    +------+                 +---------+             +------+
    |      |                 |         |             |      |
    |  23  |-------MDC------|  MDC    |             | Pin1 |---- TX+
    |      |                 |         |             |      |
    |  18  |------MDIO------|  MDIO   |             | Pin2 |---- TX-
    |      |                 |         |             |      |
    |  17  |----->CLK50-----|  CLK    |----50MHz    | Pin3 |---- RX+
    |      |                 |         |    Crystal  |      |
    | 3.3V |-------VCC------|  VCC    |             | Pin6 |---- RX-
    |      |                 |         |             |      |
    |  GND |-------GND------|  GND    |             +------+
    |      |                 |         |
    +------+                 +---------+
```

**LAN8720A Pin Connections:**

| LAN8720A Pin | ESP32 GPIO | Notes |
|--------------|------------|-------|
| MDC | GPIO23 | Management Data Clock |
| MDIO | GPIO18 | Management Data I/O |
| nRST | 3.3V via 10kΩ | Pull-up for normal operation |
| REF_CLK | GPIO17 | 50MHz clock from ESP32 |
| VCC | 3.3V | Power supply |
| GND | GND | Common ground |

**Important Notes:**
- The LAN8720A requires a 50MHz reference clock
- ESP32 generates this clock on GPIO17 (configured in code)
- Some LAN8720A modules have onboard crystal - check your module
- Use short wires between ESP32 and LAN8720A (<10cm recommended)

---

## Relay Module (RYN4) Configuration

### RYN4 8-Channel Relay Assignment

Source of truth: `include/config/RelayIndices.h`. "Index" is the 0-based array index
used in code (`RelayIndex::*`); "Physical" is the 1-based RYN4 relay/channel number
(physical = index + 1).

| Index (0-based) | Physical (1-based) | Constant | Function |
|-----------------|--------------------|----------|----------|
| 0 | CH1 | `BURNER_ENABLE` | Burner enable, heating mode (half power) - safety-critical |
| 1 | CH2 | `POWER_BOOST` | Boost to full power (ON = full, OFF = half) |
| 2 | CH3 | `WATER_MODE` | Water heating mode (half power) |
| 3 | CH4 | `VALVE` | Valve control |
| 4 | CH5 | `HEATING_PUMP` | Heating circulation pump (Wilo Yonos PICO) |
| 5 | CH6 | `WATER_PUMP` | Water heating circulation pump (HST 25/4) |
| 6 | CH7 | `SPARE_7` | Spare |
| 7 | CH8 | `ALARM` | Alarm/buzzer |

Note: the compact MQTT relay byte (`r`, `src/modules/mqtt/MQTTPublisher.cpp`) uses its own
bit order, not the physical order: 0x01 burner, 0x02 heating pump, 0x04 water pump,
0x08 power boost, 0x10 water mode.

### RYN4 Wiring

```
    RYN4 Module                          Load Connections
   +------------+                       +-----------------+
   |            |                       |                 |
   |  RS485 A   |-------- Bus A --------|  From RS485 mod |
   |  RS485 B   |-------- Bus B --------|                 |
   |  GND       |-------- GND ----------|                 |
   |            |                       +-----------------+
   |  CH1 NO/C  |-------- Burner Enable (heating mode)
   |  CH2 NO/C  |-------- Power Boost Solenoid
   |  CH3 NO/C  |-------- Water Mode
   |  CH4 NO/C  |-------- Valve
   |  CH5 NO/C  |-------- Heating Pump (230V)
   |  CH6 NO/C  |-------- Water Pump (230V)
   |  CH8 NO/C  |-------- Alarm/Buzzer
   |            |
   |  12V/24V   |-------- Power Supply
   |  GND       |-------- Power GND
   +------------+
```

### DELAY Watchdog Feature

The RYN4 implements a hardware DELAY command (0x06XX) that:
- Turns relay ON immediately
- Auto-OFF after XX seconds if no renewal

**Used for all energized relays** (`src/modules/tasks/RYN4ProcessingTask.cpp`):
- Every relay that should be ON gets DELAY 10; relays that should be OFF get DELAY 0
- DELAY 10 is renewed every 5 seconds (half of `DELAY_WATCHDOG_SECONDS`)
- If ESP32 fails, every energized relay (burner, pumps, ...) auto-OFFs within 10 seconds
- Hardware protection against software failures

```
Normal Operation:     | ESP32 Failure:
                     |
ON ─────┬────────────| ON ─────┐
        │            |         │
   ◄──5s──►DELAY 10  |    No renewal for 10s
        │            |         │
ON ─────┴────────────| OFF ────┴─ (Auto-OFF)
```

---

## Temperature Sensor (MB8ART) Configuration

### MB8ART 8-Channel PT1000 Sensor

| Channel | Sensor Location | Type | Notes |
|---------|-----------------|------|-------|
| CH0 | Boiler Output | PT1000 | Primary safety sensor |
| CH1 | Boiler Return | PT1000 | Thermal shock detection |
| CH2 | Water Tank | PT1000 | Tank temperature |
| CH3 | Outside | PT1000 | Weather compensation |
| CH4 | System Pressure | 4-20mA | 4mA = 0 BAR, 20mA = 5 BAR; <3.5mA = disconnected |
| CH5-7 | Disabled | - | Optional (`ENABLE_SENSOR_*`), deactivated in hardware |

Active channels: `MB8ART_ACTIVE_CHANNELS 5` (CH0-4) in `src/config/ProjectConfig.h`;
channel indices in `include/config/SensorIndices.h`; pressure scaling in
`SystemConstants::Hardware::PressureSensor`.

### PT1000 Sensor Wiring (2-Wire)

```
    MB8ART                     PT1000 Sensor
   +--------+                  +-----------+
   |        |                  |           |
   |  CH0+  |------------------|  +        |
   |  CH0-  |------------------|  -        |
   |        |                  |           |
   +--------+                  +-----------+
```

### PT1000 Sensor Wiring (3-Wire - Recommended)

```
    MB8ART                     PT1000 Sensor
   +--------+                  +-----------+
   |        |                  |           |
   |  CH0+  |------------------|  Lead 1   |
   |  CH0-  |--+---------------|  Lead 2   |
   |  SENSE |--+               |  Lead 3   |
   |        |                  |           |
   +--------+                  +-----------+
```

**3-wire compensation:** The third lead allows the MB8ART to compensate for cable resistance.

---

## Room Temperature Sensor (ANDRTF3) Configuration

### ANDRTF3 Modbus Room Sensor

```
    Wall Mount Location          ANDRTF3 Sensor
   +------------------+         +--------------+
   |                  |         |              |
   |  Height: 1.5m    |         |  Display     |
   |  From heat       |         |  ----------  |
   |  sources         |         |              |
   |  Away from       |         |  RS485 A/B   |----> To Modbus Bus
   |  windows         |         |  Power       |
   |                  |         |              |
   +------------------+         +--------------+
```

**Installation Notes:**
- Mount at 1.5m height (chest level)
- Away from direct sunlight, radiators, windows
- Avoid exterior walls (thermal bridging)
- Keep clear of electronics that generate heat

---

## Complete System Diagram

```
                            ┌─────────────────────────────────────┐
                            │           ETHERNET                  │
                            │            SWITCH                   │
                            └──────────────┬──────────────────────┘
                                          │
                                     ┌────┴────┐
                                     │ LAN8720A│
                                     │  PHY    │
                                     └────┬────┘
                                          │
    ┌─────────────────────────────────────┼─────────────────────────────────────┐
    │                                     │                                     │
    │  ┌──────────────────────────────────┼──────────────────────────────────┐  │
    │  │                              ESP32                                  │  │
    │  │                                                                     │  │
    │  │    GPIO17 ─── ETH_CLK                                              │  │
    │  │    GPIO18 ─── ETH_MDIO                                             │  │
    │  │    GPIO23 ─── ETH_MDC                                              │  │
    │  │                                                                     │  │
    │  │    GPIO36 ─── RS485_RX ──┐                                         │  │
    │  │    GPIO4  ─── RS485_TX ──┼──► RS485 module                         │  │
    │  │                          │                                         │  │
    │  │    GPIO33 ─── I2C_SDA ───┼──► DS3231 / FRAM (Optional)            │  │
    │  │    GPIO32 ─── I2C_SCL ───┘                                         │  │
    │  │                                                                     │  │
    │  └─────────────────────────────────────────────────────────────────────┘  │
    │                                                                           │
    │                              RS485 BUS                                    │
    │    ┌───────────────────────────────────────────────────────────┐         │
    │    │                                                           │         │
    │    ▼                          ▼                          ▼     │         │
    │  ┌─────────┐            ┌─────────┐            ┌─────────┐     │         │
    │  │  RYN4   │            │ MB8ART  │            │ ANDRTF3 │     │         │
    │  │Addr: 02 │            │Addr: 03 │            │Addr: 04 │     │         │
    │  │ Relays  │            │ Temps   │            │Room Temp│     │         │
    │  └────┬────┘            └────┬────┘            └─────────┘     │         │
    │       │                      │                                 │         │
    │       ▼                      ▼                                 │         │
    │  ┌─────────┐            ┌─────────┐                           │         │
    │  │ Burner  │            │ PT1000  │                           │         │
    │  │ Pumps   │            │ Sensors │                           │         │
    │  └─────────┘            └─────────┘                           │         │
    │                                                                │         │
    └────────────────────────────────────────────────────────────────┘         │
                                                                               │
                                      120Ω ────────────────────────────────────┘
                                 (Termination)
```

---

## Power Supply Considerations

### Voltage Requirements

| Component | Voltage | Current | Notes |
|-----------|---------|---------|-------|
| ESP32 | 5V (USB) or 3.3V | 500mA | USB recommended |
| LAN8720A | 3.3V | 50mA | From ESP32 regulator |
| RS485 module | 3.3V | see module | From ESP32 regulator |
| RYN4 | 12V or 24V | 200mA | Separate supply |
| MB8ART | 12V or 24V | 100mA | Shared with RYN4 OK |
| ANDRTF3 | 12V or 24V | 50mA | Shared with RYN4 OK |

### Recommended Power Architecture

```
    ┌─────────────────────────────────────────────────────┐
    │                  230V AC MAINS                       │
    └──────────────┬────────────────────────┬─────────────┘
                   │                        │
            ┌──────┴──────┐          ┌──────┴──────┐
            │  5V 2A PSU  │          │ 24V 1A PSU  │
            │   (USB)     │          │  (DIN Rail) │
            └──────┬──────┘          └──────┬──────┘
                   │                        │
            ┌──────┴──────┐          ┌──────┴──────┐
            │   ESP32     │          │  RYN4       │
            │   LAN8720A  │          │  MB8ART     │
            │ RS485 module│          │  ANDRTF3    │
            └─────────────┘          └─────────────┘
```

### Grounding

**Critical:** All devices must share a common ground reference.

```
    ESP32 GND ─────┬───── RYN4 GND
                   │
                   ├───── MB8ART GND
                   │
                   ├───── ANDRTF3 GND
                   │
                   └───── Power Supply GND
```

---

## Troubleshooting

### RS485 Communication Issues

| Symptom | Possible Cause | Solution |
|---------|----------------|----------|
| No response from devices | A/B wires swapped | Swap A and B connections |
| CRC errors | Missing termination | Add 120Ω resistor at bus ends |
| Intermittent errors | Ground loop | Use isolated RS485 transceiver |
| Timeout errors | Wrong baud rate | Verify 9600 baud, 8E1 |

### Ethernet Issues

| Symptom | Possible Cause | Solution |
|---------|----------------|----------|
| No link LED | Clock not working | Check GPIO17 connection |
| No IP address | DHCP not responding | Configure static IP |
| Intermittent drops | Power supply noise | Add decoupling capacitors |

### Temperature Sensor Issues

| Symptom | Possible Cause | Solution |
|---------|----------------|----------|
| Reading 0 or -999 | Sensor disconnected | Check wiring continuity |
| Unstable readings | EMI interference | Use shielded cable |
| Offset error | Cable resistance | Use 3-wire connection |

### Relay Issues

| Symptom | Possible Cause | Solution |
|---------|----------------|----------|
| Relay not switching | Coil voltage wrong | Verify 12V or 24V supply |
| Clicking rapidly | Control logic issue | Check DELAY watchdog timing |
| Stuck on | DELAY timer active | Wait for timeout or reset |

---

## Safety Checklist

Before powering on:

- [ ] All grounds connected and verified
- [ ] RS485 termination resistors installed
- [ ] LAN8720A clock wiring correct (GPIO17)
- [ ] Power supplies correct voltage
- [ ] No short circuits on bus wiring
- [ ] Relay loads connected correctly (NO/NC)
- [ ] PT1000 sensors connected to correct channels
- [ ] Modbus addresses verified and unique

After powering on:

- [ ] Serial monitor shows boot messages at 921600 baud
- [ ] Ethernet link LED active
- [ ] MQTT connection established
- [ ] Modbus devices responding (check sensor readings)
- [ ] Relays can be toggled via MQTT commands

---

*Last Updated: 2026-09-15*
