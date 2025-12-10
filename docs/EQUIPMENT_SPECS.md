# Equipment Technical Specifications

This document contains the technical specifications for the boiler and water tank,
and analysis of how system parameters align with equipment limits.

## Boiler: Vaillant VK 42/4-2 XEU H/PB

Cast iron atmospheric gas boiler with two-stage burner.

### General Specifications

| Parameter | Value | Unit |
|-----------|-------|------|
| Nominal heat output | 42.2 | kW |
| Minimum heat output (part load) | 23.3 | kW |
| Maximum heat load (Hi) | 46.4 | kW |
| Minimum heat load (Hi) | 22.3 | kW |
| Efficiency at 75/60°C | 94.0 | % |
| Number of sections | 9 | |
| Category | II2ELL3B/P | |

### Operating Limits

| Parameter | Value | Unit | Notes |
|-----------|-------|------|-------|
| **Max operating pressure** | **4** | **bar** | **HARD LIMIT** |
| Max flow temperature | 120 | °C | Absolute maximum |
| Adjustable flow temperature | 90 | °C | Thermostat setting |
| Water content | ~12 | L | |
| Weight (empty) | 169 | kg | |
| Weight (filled) | 181 | kg | |

### Gas Specifications

| Gas Type | Connection Value | Required Pressure |
|----------|------------------|-------------------|
| Natural Gas LL (L) | 6.10 m³/h | 20 mbar |
| Natural Gas E (H) | 4.42 m³/h | 20 mbar |
| LPG B/P | 3.62 kg/h | 50 mbar |

### Flue Gas

| Parameter | Full Load | Part Load | Unit |
|-----------|-----------|-----------|------|
| Mass flow | 118 | 112 | kg/h |
| Temperature | 118 | 71 | °C |
| Draft pressure required | 3.5 | 2.5 | Pa |

### Two-Stage Operation

The burner operates in two stages controlled by a solenoid valve:

| Stage | Heat Output | Gas Consumption | Nozzle Pressure |
|-------|-------------|-----------------|-----------------|
| Stage 1 (Part load) | 23.3 kW | ~55% | 2.9-3.5 mbar |
| Stage 2 (Full load) | 42.2 kW | 100% | 9.6-11.5 mbar |

**Relay Control:**
- `POWER_SELECT` relay: ON = Stage 1 (half power), OFF = Stage 2 (full power)

---

## Water Tank: Vaillant VIH Q 150

Indirect heated hot water storage tank with internal heating coil.

### General Specifications

| Parameter | Value | Unit |
|-----------|-------|------|
| Storage capacity | 150 | L |
| Heating surface area | 0.96 | m² |
| Heating coil water content | 5.6 | L |
| Performance rating (NL) | 2.5 | |
| Standby energy consumption | ~1.6 | kWh/day |

### Operating Limits

| Parameter | Value | Unit | Notes |
|-----------|-------|------|-------|
| **Max storage temperature** | **85** | **°C** | **HARD LIMIT** |
| Max heating water inlet | 110 | °C | From boiler |
| Max pressure (storage side) | 10 | bar | |
| Max pressure (heating side) | 16 | bar | |

### Performance Data (at 86/66°C heating water)

| Parameter | Value | Unit |
|-----------|-------|------|
| Max continuous output | 33 | kW |
| Continuous hot water flow | 810 | L/h |
| 10-minute output | 215 | L |
| Pressure drop in coil | 36 | mbar |
| Nominal heating water flow (ΔT=20K) | 1.4 | m³/h |

### Heating Power Requirements

| Parameter | Value | Unit | Notes |
|-----------|-------|------|-------|
| Recommended minimum | 21 | kW | For efficient heating |
| Maximum possible | 36 | kW | |

**Compatibility with VK 42/4-2:**
- Stage 1 (23.3 kW): ✓ Above minimum, gentle heating
- Stage 2 (42.2 kW): ✓ Above maximum but tank can handle (limited by flow rate)

### Physical Dimensions

| Parameter | Value | Unit |
|-----------|-------|------|
| Width | 615 | mm |
| Height | 1080 | mm |
| Depth | 630 | mm |
| Weight (empty + packaging) | 93 | kg |
| Weight (filled, operational) | 249 | kg |

### Connections

| Connection | Size |
|------------|------|
| Cold/Hot water | R 3/4 |
| Heating flow/return | Rp 1 |

---

## System Parameter Analysis

### Temperature Parameters

| Parameter | Current Value | Equipment Limit | Status |
|-----------|---------------|-----------------|--------|
| `wHeaterConfTempSafeLimitHigh` | 80°C | Tank: 85°C | ✓ Conservative (5°C margin) |
| `wHeaterConfTempLimitHigh` | 65°C | Tank: 85°C | ✓ Normal setpoint |
| `wHeaterConfTempLimitLow` | 45°C | - | ✓ Reheat threshold |
| `MAX_BOILER_TEMP_C` | 90°C | Boiler: 90°C adj | ✓ Matches thermostat |
| `CRITICAL_BOILER_TEMP_C` | 95°C | Boiler: 120°C abs | ✓ Safe margin |
| `WaterHeating::MAX_TARGET_TEMP` | 85°C | Tank: 85°C | ✓ Matches tank max |
| `WaterHeating::MIN_TARGET_TEMP` | 30°C | - | ✓ Reasonable minimum |

**Note:** The `wHeaterConfTempSafeLimitHigh` of 80°C provides a 5°C safety margin
before the tank's absolute maximum of 85°C. This allows for:
- Temperature overshoot during heating
- Sensor measurement tolerances
- Control loop response time

### Pressure Parameters

| Parameter | Current Value | Equipment Limit | Status |
|-----------|---------------|-----------------|--------|
| `Safety::Pressure::MIN_OPERATING` | 1.00 bar | - | ✓ Fill indicator |
| `Safety::Pressure::MAX_OPERATING` | 3.50 bar | Boiler: 4 bar | ✓ 87.5% of limit |
| `Safety::Pressure::ALARM_MIN` | 0.50 bar | - | ✓ Low pressure alarm |
| `Safety::Pressure::ALARM_MAX` | 4.00 bar | Boiler: 4 bar | ⚠️ AT LIMIT |

**Concern:** `ALARM_MAX` is set exactly at the boiler's maximum operating pressure.
The alarm should trigger BEFORE reaching the mechanical limit to allow intervention.

**Recommendation:** Consider reducing `ALARM_MAX` to 3.80 bar (95% of limit) to provide
a safety margin for:
- Pressure sensor accuracy (±2-5%)
- Thermal expansion during heating
- Response time for operator intervention

### Timing Parameters

| Parameter | Current Value | Rationale |
|-----------|---------------|-----------|
| `MIN_ON_TIME_MS` | 120,000 (2 min) | Protects gas valve, prevents short-cycling |
| `MIN_OFF_TIME_MS` | 20,000 (20 sec) | Allows residual heat dissipation |
| `POST_PURGE_TIME_MS` | 60,000 (60 sec) | Heat exchanger cooling |
| `PRE_PURGE_TIME_MS` | 2,000 (2 sec) | Atmospheric burner (no forced draft) |
| `IGNITION_TIME_MS` | 5,000 (5 sec) | Flame establishment window |

These timings are appropriate for an atmospheric burner without forced-air fan.

### Power Level Selection

The system uses PID output to select power level:

| PID Output | Power Level | Actual Output |
|------------|-------------|---------------|
| < 50% | Stage 1 (LOW) | 23.3 kW |
| ≥ 50% | Stage 2 (HIGH) | 42.2 kW |

Both stages are within the tank's recommended heating power range (21-36 kW),
though Stage 2 exceeds the "maximum possible" of 36 kW. This is acceptable because:
1. The heating coil limits actual heat transfer
2. The boiler's internal thermostat provides additional protection
3. The system's safety interlocks monitor temperatures continuously

---

## Safety Considerations

### Hard Limits (Equipment Damage)

1. **Boiler pressure > 4 bar**: Risk of seal/gasket failure
2. **Tank temperature > 85°C**: Risk of tank damage, scalding hazard
3. **Boiler temperature > 120°C**: Risk of thermal damage (unlikely with 90°C thermostat)

### Soft Limits (System Protection)

1. **Operating pressure > 3.5 bar**: Indicates expansion vessel issue
2. **Tank temperature > 80°C**: Approaching limit, reduce heating
3. **Boiler output > 90°C**: Thermostat should limit, safety backup

### Missing Physical Sensors

The system currently lacks dedicated sensors for:
- **Flame detection**: Using temperature rise as proxy
- **Water flow**: Using temperature differential as proxy
- **Pressure relief valve status**: No feedback

These proxies are acceptable for supervised operation but should be upgraded
for fully autonomous operation.

---

## Document History

| Date | Change |
|------|--------|
| 2025-12-10 | Initial documentation from equipment datasheets |
