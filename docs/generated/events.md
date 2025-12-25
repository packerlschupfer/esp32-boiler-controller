# Event System Documentation
Generated: 2025-12-08 01:44:51

## Overview
This system uses zero-overhead namespaced constants for event management.
All constants compile to the same machine code as traditional #define macros.

## Event Groups

### SystemState
**Handle:** `xSystemStateEventGroup`
**Description:** System operational state tracking

| Bit | Event | Description |
|-----|-------|-------------|
| 0 | `BOILER_ENABLED` | Boiler system enabled |
| 1 | `BOILER_ON` | Boiler currently on |
| 2 | `HEATING_ENABLED` | Space heating enabled |
| 3 | `HEATING_ON` | Space heating active |
| 4 | `WATER_ENABLED` | Water heating enabled |
| 5 | `WATER_ON` | Water heating active |
| 6 | `WATER_PRIORITY` | Water has priority over heating |
| 7 | `BURNER_ON` | Burner is running |
| 8 | `HEATING_PUMP_ON` | Heating pump running |
| 9 | `WATER_PUMP_ON` | Water pump running |
| 12 | `MQTT_ENABLED` | MQTT communication enabled |
| 13 | `MQTT_COMMAND_ENABLED` | MQTT commands enabled |
| 14 | `MQTT_REPORT_ENABLED` | MQTT reporting enabled |
| 15 | `MQTT_OPERATIONAL` | MQTT fully operational |
| 16 | `BURNER_OFF` | Burner is off |
| 17 | `BURNER_HEATING_LOW` | Burner on low for heating |
| 18 | `BURNER_HEATING_HIGH` | Burner on high for heating |
| 19 | `BURNER_WATER_LOW` | Burner on low for water |
| 20 | `BURNER_WATER_HIGH` | Burner on high for water |
| 21 | `BURNER_ERROR` | Burner in error state |
| 22 | `EMERGENCY_STOP` | Emergency stop active |
| 23 | `WARNING` | System warning active |

**Derived Constants:**
- `DEGRADED_MODE`: WARNING

### Burner
**Handle:** `xBurnerEventGroup`
**Description:** Burner-specific control and status

| Bit | Event | Description |
|-----|-------|-------------|
| 0 | `IDLE` | Burner is idle |
| 1 | `STARTING` | Burner is starting |
| 2 | `RUNNING` | Burner is running |
| 3 | `STOPPING` | Burner is stopping |
| 4 | `LOCKOUT` | Burner in lockout state |
| 5 | `ENABLE` | Burner enable signal |
| 6 | `LOW_POWER` | Low power mode |
| 7 | `HIGH_POWER` | High power mode |
| 8 | `PURGE` | Purge cycle active |
| 9 | `FLAME_DETECTED` | Flame detected |
| 10 | `TEMP_OK` | Temperature within limits |
| 11 | `PRESSURE_OK` | Pressure within limits |
| 12 | `SAFETY_OK` | All safety checks OK |
| 13 | `ERROR_IGNITION` | Ignition failure |
| 14 | `ERROR_FLAME_LOSS` | Flame loss during operation |
| 15 | `ERROR_OVERHEAT` | Overheat condition |
| 16 | `ERROR_PRESSURE` | Pressure error |
| 17 | `FLAME_STATE_CHANGED` | Flame state changed |
| 18 | `PRESSURE_CHANGED` | Pressure sensor changed |
| 19 | `FLOW_CHANGED` | Flow sensor changed |
| 20 | `SAFETY_EVENT` | Any safety sensor changed |
| 21 | `STATE_TIMEOUT` | State machine timeout occurred |

**Derived Constants:**
- `ANY_ERROR`: ERROR_IGNITION | ERROR_FLAME_LOSS | ERROR_OVERHEAT | ERROR_PRESSURE

### BurnerRequest
**Handle:** `xBurnerRequestEventGroup`
**Description:** Burner demand coordination between heating and water

| Bit | Event | Description |
|-----|-------|-------------|
| 0 | `HEATING` | Space heating requesting burner |
| 1 | `WATER` | Water heating requesting burner |
| 3 | `POWER_LOW` | Request low power mode |
| 4 | `POWER_HIGH` | Request high power mode |
| 18 | `CHANGED` | Any request changed |
| 19 | `HEATING_CHANGED` | Heating request changed |
| 20 | `WATER_CHANGED` | Water request changed |

**Special Regions:**
- Bits 16-23: Encoded target temperature

**Derived Constants:**
- `ANY_REQUEST`: HEATING | WATER
- `POWER_BITS`: POWER_LOW | POWER_HIGH
- `ALL_BITS`: HEATING | WATER | POWER_LOW | POWER_HIGH | TEMPERATURE_MASK | CHANGED | HEATING_CHANGED | WATER_CHANGED
- `CHANGE_EVENT_BITS`: HEATING_CHANGED | WATER_CHANGED | CHANGED

### SensorUpdate
**Handle:** `xSensorEventGroup`
**Description:** Temperature sensor update notifications

| Bit | Event | Description |
|-----|-------|-------------|
| 0 | `BOILER_OUTPUT` | Boiler output temp updated |
| 1 | `BOILER_RETURN` | Boiler return temp updated |
| 2 | `WATER_TANK` | Water tank temp updated |
| 3 | `WATER_OUTPUT` | Water output temp updated |
| 4 | `WATER_RETURN` | Water return temp updated |
| 5 | `HEATING_RETURN` | Heating return temp updated |
| 6 | `OUTSIDE` | Outside temp updated |
| 7 | `INSIDE` | Inside temp updated |
| 8 | `EXHAUST` | Exhaust temp updated |
| 9 | `DATA_AVAILABLE` | Sensor data available |
| 10 | `BOILER_OUTPUT_ERROR` | Boiler output sensor error |
| 11 | `BOILER_RETURN_ERROR` | Boiler return sensor error |
| 12 | `WATER_TANK_ERROR` | Water tank sensor error |
| 13 | `WATER_OUTPUT_ERROR` | Water output sensor error |
| 14 | `WATER_RETURN_ERROR` | Water return sensor error |
| 15 | `HEATING_RETURN_ERROR` | Heating return sensor error |
| 16 | `OUTSIDE_ERROR` | Outside sensor error |
| 17 | `INSIDE_ERROR` | Inside sensor error |
| 18 | `EXHAUST_ERROR` | Exhaust sensor error |
| 19 | `DATA_ERROR` | General sensor data error |
| 20 | `PRESSURE` | System pressure updated |
| 21 | `PRESSURE_ERROR` | Pressure sensor error |
| 23 | `FIRST_READ_COMPLETE` | First sensor read complete |

**Derived Constants:**
- `ALL_TEMPS`: BOILER_OUTPUT | BOILER_RETURN | WATER_TANK | WATER_OUTPUT | WATER_RETURN | HEATING_RETURN | OUTSIDE | INSIDE
- `CRITICAL_TEMPS`: BOILER_OUTPUT | EXHAUST

### Error
**Handle:** `ERROR_NOTIFICATION`
**Description:** Error notification and recovery events

| Bit | Event | Description |
|-----|-------|-------------|
| 0 | `SENSOR_FAILURE` | Sensor has failed |
| 1 | `SENSOR_RESOLVED` | Sensor error resolved |
| 2 | `WHEATER` | Water heater error |
| 3 | `WHEATER_RESOLVED` | Water heater error resolved |
| 4 | `BURNER` | Burner error |
| 5 | `BURNER_RESOLVED` | Burner error resolved |
| 6 | `PUMP` | Pump error |
| 7 | `PUMP_RESOLVED` | Pump error resolved |
| 8 | `GENERAL_SYSTEM` | General system error |
| 9 | `GENERAL_SYSTEM_RESOLVED` | General system error resolved |
| 10 | `COMMUNICATION` | Communication error |
| 11 | `COMMUNICATION_RESOLVED` | Communication error resolved |
| 12 | `MEMORY` | Memory allocation error |
| 13 | `MEMORY_RESOLVED` | Memory error resolved |
| 14 | `SAFETY` | Safety check failed |
| 15 | `SAFETY_RESOLVED` | Safety error resolved |
| 16 | `OVERHEAT` | Overheating detected |
| 17 | `OVERHEAT_RESOLVED` | Overheat resolved |
| 18 | `MODBUS` | Modbus communication error |
| 19 | `RELAY` | Relay control error |
| 20 | `NETWORK` | Network error |

**Derived Constants:**
- `SENSOR`: SENSOR_FAILURE
- `ANY_ACTIVE`: SENSOR_FAILURE | WHEATER | BURNER | PUMP | GENERAL_SYSTEM | COMMUNICATION | MEMORY | SAFETY | OVERHEAT
- `ALL_RESOLVED`: SENSOR_RESOLVED | WHEATER_RESOLVED | BURNER_RESOLVED | PUMP_RESOLVED | GENERAL_SYSTEM_RESOLVED | COMMUNICATION_RESOLVED | MEMORY_RESOLVED | SAFETY_RESOLVED | OVERHEAT_RESOLVED

### ControlRequest
**Handle:** `xControlRequestEventGroup`
**Description:** Remote control requests from MQTT/scheduler

| Bit | Event | Description |
|-----|-------|-------------|
| 0 | `BOILER_ENABLE` | Enable boiler system |
| 1 | `BOILER_DISABLE` | Disable boiler system |
| 2 | `HEATING_ENABLE` | Enable heating |
| 3 | `HEATING_DISABLE` | Disable heating |
| 4 | `HEATING_ON_OVERRIDE` | Force heating on |
| 5 | `HEATING_OFF_OVERRIDE` | Force heating off |
| 6 | `WATER_ENABLE` | Enable water heating |
| 7 | `WATER_DISABLE` | Disable water heating |
| 8 | `WATER_PRIORITY_ENABLE` | Enable water priority |
| 9 | `WATER_PRIORITY_DISABLE` | Disable water priority |
| 10 | `WATER_ON_OVERRIDE` | Force water heating on |
| 11 | `WATER_OFF_OVERRIDE` | Force water heating off |
| 12 | `MQTT_ENABLE` | Enable MQTT |
| 13 | `MQTT_DISABLE` | Disable MQTT |
| 14 | `MQTT_COMMAND_ENABLE` | Enable MQTT commands |
| 15 | `MQTT_COMMAND_DISABLE` | Disable MQTT commands |
| 16 | `MQTT_REPORT_ENABLE` | Enable MQTT reporting |
| 17 | `MQTT_REPORT_DISABLE` | Disable MQTT reporting |
| 18 | `WATER_PRIORITY_RELEASED` | Water priority was released (notify heating) |
| 22 | `PID_AUTOTUNE` | Start PID auto-tuning |
| 23 | `PID_AUTOTUNE_STOP` | Stop PID auto-tuning |
| 24 | `PID_SAVE` | Save PID parameters |
| 25 | `SAVE_PARAMETERS` | Save system parameters |

### DeviceReady
**Handle:** `deviceReadyEventGroup`
**Description:** Device initialization status tracking

| Bit | Event | Description |
|-----|-------|-------------|
| 0 | `MB8ART_READY` | MB8ART device initialized and ready |
| 1 | `MB8ART_ERROR` | MB8ART initialization error |
| 2 | `RYN4_READY` | RYN4 device initialized and ready |
| 3 | `RYN4_ERROR` | RYN4 initialization error |

**Derived Constants:**
- `ALL_CRITICAL_READY`: MB8ART_READY | RYN4_READY

### HeatingEvent
**Handle:** `xHeatingEventGroup`
**Description:** Heating system control events

| Bit | Event | Description |
|-----|-------|-------------|
| 0 | `True` | Heating system is turned on |
| 1 | `False` | Heating system is turned off |
| 2 | `TEMP_LOW` | Temperature below setpoint |
| 3 | `TEMP_HIGH` | Temperature above setpoint |
| 4 | `ERROR` | Heating system error occurred |
| 5 | `PUMP_ON` | Heating pump turned on |
| 6 | `PUMP_OFF` | Heating pump turned off |
| 7 | `DEMAND` | Heat demand detected |
| 8 | `NO_DEMAND` | No heat demand |
| 9 | `AUTOTUNE_START` | Start PID auto-tuning |
| 10 | `AUTOTUNE_STOP` | Stop PID auto-tuning |
| 11 | `AUTOTUNE_RUNNING` | Auto-tuning in progress |
| 12 | `AUTOTUNE_COMPLETE` | Auto-tuning completed |
| 13 | `AUTOTUNE_FAILED` | Auto-tuning failed |
| 16 | `ERROR_SENSOR` | Temperature sensor error |
| 17 | `ERROR_PUMP` | Heating pump error |
| 18 | `ERROR_OVERHEAT` | Overheating detected |
| 19 | `ERROR_TIMEOUT` | Heating timeout |

### GeneralSystem
**Handle:** `xGeneralSystemEventGroup`
**Description:** General system-wide status flags

| Bit | Event | Description |
|-----|-------|-------------|
| 0 | `STARTUP_COMPLETE` | System startup completed |
| 1 | `NETWORK_READY` | Network interface ready |
| 2 | `WIFI_CONNECTED` | WiFi connected |
| 3 | `MQTT_CONNECTED` | MQTT broker connected |
| 4 | `TIME_SYNCED` | Time synchronized |
| 5 | `STORAGE_READY` | Storage system ready |
| 6 | `SYSTEM_REBOOT` | System reboot requested |
| 7 | `SENSOR_DEGRADED` | Sensor operating in degraded mode |
| 8 | `MQTT_QUEUE_PRESSURE` | MQTT queue under pressure |
| 9 | `MUTEX_CONTENTION` | Mutex contention detected |

### RelayRequest
**Handle:** `xRelayRequestEventGroup`
**Description:** Relay control requests

| Bit | Event | Description |
|-----|-------|-------------|
| 0 | `HEATING_PUMP_ON` | Request heating pump ON |
| 1 | `WATER_PUMP_ON` | Request water pump ON |
| 2 | `HEATING_PUMP_OFF` | Request heating pump OFF |
| 3 | `WATER_PUMP_OFF` | Request water pump OFF |
| 4 | `BURNER_ENABLE` | Request burner ON |
| 5 | `BURNER_DISABLE` | Request burner OFF |
| 6 | `POWER_HALF` | Request half power |
| 7 | `POWER_FULL` | Request full power |
| 8 | `WATER_MODE_ON` | Request water mode ON |
| 9 | `WATER_MODE_OFF` | Request water mode OFF |
| 10 | `VALVE_OPEN` | Request valve OPEN |
| 11 | `VALVE_CLOSE` | Request valve CLOSE |
| 12 | `ALARM_ON` | Request alarm ON |
| 13 | `ALARM_OFF` | Request alarm OFF |
| 20 | `EMERGENCY_STOP` | Emergency stop all relays |
| 21 | `UPDATE` | Force relay state update |

**Derived Constants:**
- `PUMP_REQUESTS_MASK`: HEATING_PUMP_ON | WATER_PUMP_ON | HEATING_PUMP_OFF | WATER_PUMP_OFF
- `BURNER_REQUESTS_MASK`: BURNER_ENABLE | BURNER_DISABLE
- `POWER_REQUESTS_MASK`: POWER_HALF | POWER_FULL
- `ALL_REQUESTS_MASK`: (1UL << 22UL) - 1

### RelayStatus
**Handle:** `xRelayStatusEventGroup`
**Description:** Relay status and feedback

| Bit | Event | Description |
|-----|-------|-------------|
| 0 | `SYNCHRONIZED` | Relay states synchronized |
| 1 | `COMM_OK` | Communication OK |
| 2 | `COMM_ERROR` | Communication error |
| 3 | `UPDATE_PENDING` | Update pending |

### RelayControl
**Handle:** `xRelayEventGroup`
**Description:** Relay control requests and status

| Bit | Event | Description |
|-----|-------|-------------|
| 0 | `HEATING_PUMP_ON` | Turn heating pump on |
| 1 | `HEATING_PUMP_OFF` | Turn heating pump off |
| 2 | `WATER_PUMP_ON` | Turn water pump on |
| 3 | `WATER_PUMP_OFF` | Turn water pump off |
| 4 | `BURNER_ENABLE_ON` | Enable burner |
| 5 | `BURNER_ENABLE_OFF` | Disable burner |
| 6 | `WATER_MODE_ON` | Switch to water mode |
| 7 | `WATER_MODE_OFF` | Exit water mode |
| 8 | `HALF_POWER_ON` | Switch to half power |
| 9 | `HALF_POWER_OFF` | Exit half power |
| 10 | `VALVE_OPEN` | Open valve |
| 11 | `VALVE_CLOSE` | Close valve |
| 12 | `ALARM_ON` | Activate alarm |
| 13 | `ALARM_OFF` | Deactivate alarm |
| 14 | `UPDATE_REQUIRED` | Relay update needed |
| 15 | `UPDATE_COMPLETE` | Relay update done |
| 16 | `DATA_AVAILABLE` | Relay data available |
| 20 | `EMERGENCY_STOP` | Emergency stop all |
| 21 | `FORCE_UPDATE` | Force relay update |


## Usage Examples

### Setting Events
```cpp
// Old way:
xEventGroupSetBits(xSystemStateEventGroup, SYSTEM_STATE_BOILER_ENABLED_BIT);

// New way (zero overhead):
xEventGroupSetBits(xSystemStateEventGroup, SystemEvents::SystemState::BOILER_ENABLED);
```

### Waiting for Events
```cpp
// Wait for any temperature update
EventBits_t bits = xEventGroupWaitBits(
    xSensorEventGroup,
    SystemEvents::SensorUpdate::ALL_TEMPS,
    pdTRUE,   // Clear on exit
    pdFALSE,  // Wait for any
    pdMS_TO_TICKS(1000)
);
```

### Encoding Values
```cpp
// Encode temperature in burner request
EventBits_t request = SystemEvents::BurnerRequest::HEATING |
                     SystemEvents::BurnerRequest::POWER_HIGH |
                     SystemEvents::BurnerRequest::encode_temperature(75);

// Decode temperature from request
uint32_t temp = SystemEvents::BurnerRequest::decode_temperature(request);
```