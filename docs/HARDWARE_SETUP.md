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
| ESP32 | DevKitC or compatible | 1 | Main controller |
| Ethernet PHY | LAN8720A module | 1 | Network connectivity |
| Relay Module | RYN4 8-channel | 1 | Burner and pump control |
| Temperature Sensor | MB8ART 8-channel | 1 | Boiler/tank temperatures |
| RS485 Transceiver | MAX485 or similar | 1 | Modbus communication |

### Optional Components

| Component | Model | Quantity | Purpose |
|-----------|-------|----------|---------|
| Room Temp Sensor | ANDRTF3 | 1 | Inside temperature |
| RTC Module | DS3231 | 1 | Scheduling (NTP fallback) |
| FRAM Module | MB85RC256V | 1 | Enhanced error logging |
| Pressure Sensor | 4-20mA (0-6 BAR) | 1 | System pressure monitoring |

---

## ESP32 Pin Configuration

### GPIO Pin Assignments

```
ESP32 DevKitC Pin Map
====================

                    +------------------+
              EN  -|                  |- GPIO23 (ETH_MDC)
        GPIO36/VP -|   RS485_RX (36)  |- GPIO22 (I2C_SCL)
        GPIO39/VN -|                  |- GPIO21 (I2C_SDA)
          GPIO34  -|                  |- GPIO19
          GPIO35  -|                  |- GPIO18 (ETH_MDIO)
          GPIO32  -|                  |- GPIO5
          GPIO33  -|                  |- GPIO17 (ETH_CLK)
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
| 36 | RS485_RX | Input | RX from MAX485 RO pin |
| 4 | RS485_TX | Output | TX to MAX485 DI pin |
| **Ethernet (LAN8720A)** ||||
| 23 | ETH_MDC | Output | Management Data Clock |
| 18 | ETH_MDIO | Bidirectional | Management Data I/O |
| 17 | ETH_CLK | Output | 50MHz clock output |
| **I2C (Optional)** ||||
| 21 | I2C_SDA | Bidirectional | DS3231, FRAM |
| 22 | I2C_SCL | Output | DS3231, FRAM |
| **Status** ||||
| 2 | LED_BUILTIN | Output | Status LED |

---

## RS485 Modbus Bus Wiring

### MAX485 Transceiver Wiring

```
     ESP32                   MAX485                    Modbus Bus
    +------+                +--------+
    |      |                |        |
    |  36  |<-------RO-----|        |                  +-------+
    |      |                |        |---A--------+----| Device|
    |   4  |------->DI-----|        |             |    +-------+
    |      |                |        |---B--------+    +-------+
    |  GND |--------GND----|        |             +----| Device|
    |      |                |        |                  +-------+
    | 3.3V |--------VCC----|        |
    |      |                |        |
    |  GND |-------->RE----|        |       Termination: 120Ω between A and B
    |  3.3V|-------->DE----|        |       at each end of the bus
    +------+                +--------+
```

**MAX485 Pin Connections:**

| MAX485 Pin | Connection | Notes |
|------------|------------|-------|
| RO | ESP32 GPIO36 | Receiver Output |
| DI | ESP32 GPIO4 | Driver Input |
| RE | GND | Always receive enabled |
| DE | VCC (3.3V) | Always transmit enabled |
| A | Bus A (Data+) | Connect to all devices |
| B | Bus B (Data-) | Connect to all devices |
| VCC | 3.3V | Power supply |
| GND | GND | Common ground |

**Important Notes:**
- RE tied to GND and DE tied to VCC enables "always receive/transmit" mode
- This works because ESP32 software controls TX timing
- For long cable runs (>50m), use proper RS485 transceiver with TX enable control
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
| MAX485|              |  RYN4 |               | MB8ART|
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

| Relay | Physical | Function | Control |
|-------|----------|----------|---------|
| 0 | CH1 | Burner Enable | Safety-critical |
| 1 | CH2 | Power Boost | Stage 2 (42kW) |
| 2 | CH3 | Heating Pump | Wilo Yonos PICO |
| 3 | CH4 | Water Pump | HST 25/4 |
| 4 | CH5 | Reserved | - |
| 5 | CH6 | Reserved | - |
| 6 | CH7 | Reserved | - |
| 7 | CH8 | Reserved | - |

### RYN4 Wiring

```
    RYN4 Module                          Load Connections
   +------------+                       +-----------------+
   |            |                       |                 |
   |  RS485 A   |-------- Bus A --------|  From MAX485    |
   |  RS485 B   |-------- Bus B --------|                 |
   |  GND       |-------- GND ----------|                 |
   |            |                       +-----------------+
   |  CH1 NO/C  |-------- Burner Enable Signal
   |  CH2 NO/C  |-------- Power Boost Solenoid
   |  CH3 NO/C  |-------- Heating Pump (230V)
   |  CH4 NO/C  |-------- Water Pump (230V)
   |            |
   |  12V/24V   |-------- Power Supply
   |  GND       |-------- Power GND
   +------------+
```

### DELAY Watchdog Feature

The RYN4 implements a hardware DELAY command (0x06XX) that:
- Turns relay ON immediately
- Auto-OFF after XX seconds if no renewal

**Used for burner safety:**
- DELAY 10 command sent every 5 seconds
- If ESP32 fails, relay auto-OFF in 10 seconds
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
| CH4-7 | Disabled | - | Not connected |

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
    │  │    GPIO4  ─── RS485_TX ──┼──► MAX485                               │  │
    │  │                          │                                         │  │
    │  │    GPIO21 ─── I2C_SDA ───┼──► DS3231 / FRAM (Optional)            │  │
    │  │    GPIO22 ─── I2C_SCL ───┘                                         │  │
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
| MAX485 | 3.3V | 5mA | From ESP32 regulator |
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
            │   MAX485    │          │  ANDRTF3    │
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

*Last Updated: 2025-12-22*
