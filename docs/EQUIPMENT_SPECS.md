# Equipment Technical Specifications

This document contains the technical specifications for the boiler and water tank,
and analysis of how system parameters align with equipment limits.

## Boiler: Vaillant VK 42/4-2 XEU H/PB

**Low-temperature (Niedertemperatur) gas boiler** with two-stage burner.
Minimum settable flow temperature: 35°C (dial setting 1) to 90°C (dial setting 8).

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

### Boiler Type and Minimum Flow Temperature

The VK 42/4-2 is a **low-temperature (Niedertemperatur)** gas boiler, NOT a
conventional cast iron atmospheric boiler. Low-temperature boilers are designed
to operate with lower return water temperatures without condensation damage.

| Boiler/System Type | Minimum Flow Temp | Reason |
|--------------------|-------------------|--------|
| Heat pump | 20-35°C | No combustion, no condensation risk |
| Condensing gas | 35-45°C | Designed to condense, improves efficiency |
| **Low-temperature gas (this boiler)** | **35°C** | Designed for lower temps |
| Oil burner | 38°C | Sulfur condensation protection |
| Conventional cast iron | 55°C | Prevents acidic flue gas condensation |

**VK 42/4-2 Specifications:**
- Minimum settable temperature: 35°C (dial position 1)
- Maximum settable temperature: 90°C (dial position 8)
- Designed for weather-compensated control (optional VRC-Set)

**System parameter:** `burner_low_limit` default is **38°C** to safely support
both gas low-temperature boilers (min 35°C) and oil burners (min 38°C).
For this specific gas boiler, 35°C is acceptable if needed for efficiency.

### Two-Stage Operation

The burner operates in two stages controlled by a solenoid valve:

| Stage | Heat Output | Gas Consumption | Nozzle Pressure |
|-------|-------------|-----------------|-----------------|
| Stage 1 (Part load) | 23.3 kW | ~55% | 2.9-3.5 mbar |
| Stage 2 (Full load) | 42.2 kW | 100% | 9.6-11.5 mbar |

**Relay Control:**
- `POWER_BOOST` relay (Physical Relay 2): ON = Stage 2 (full power 42.2kW), OFF = Stage 1 (half power 23.3kW)

---

## RYN4 8-Channel Relay Module

### Hardware DELAY Command (0x06XX)

The RYN4 relay module implements a hardware timer for automatic relay shutdown. This is used for safety-critical operations like burner control.

**Command Format:**
- Command: `0x06XX` where `XX` = delay in seconds (01-99)
- Example: `0x0614` = Turn relay ON, auto-OFF after 20 seconds (14 hex = 20 decimal)

**Hardware Behavior (verified via mbpoll):**

| Action | Register Value | Physical Relay | Notes |
|--------|---------------|----------------|-------|
| DELAY 20 (0x0614) | 0x0614 | ON immediately | Countdown starts |
| Read status during countdown | 0x0001 | ON | Register shows state, not command |
| DELAY expires | 0x0000 | OFF automatically | Hardware timer triggered |
| CLOSE (0x0200) during DELAY | 0x0000 | Still ON | CLOSE does NOT cancel DELAY |
| DELAY 0 (0x0600) | 0x0000 | OFF immediately | Only way to cancel DELAY |

**Critical Behaviors:**

1. **Physical State vs Command Value**
   - Reading relay status returns physical state (0x0001/0x0000)
   - Never returns the DELAY command value (0x06XX)
   - Verification timing must account for hardware countdown

2. **DELAY Cancellation**
   - Standard CLOSE (0x0200) does NOT cancel active DELAY
   - Must use DELAY 0 (0x0600) to immediately stop relay
   - Or use RYN4 library methods: `forceOffRelay()`, `emergencyStopAll()`

3. **Verification Timing**
   - During DELAY countdown: Physical state = ON, Desired state = OFF
   - This creates expected mismatch during transition period
   - Verification must skip relays with active DELAY timers

**Software Implementation:**

The main project tracks DELAY commands in `RelayState` structure:
- `delayMask`: Bitmask of relays with active DELAY
- `delayExpiry[]`: Timestamp when DELAY will expire
- `isDelayActive()`: Checks if DELAY countdown still running

Relay verification logic:
```cpp
// Skip verification for relays with active DELAY
if (actual != desired) {
    uint8_t mismatchMask = actual ^ desired;
    uint8_t realMismatch = mismatchMask & ~delayMask;
    if (realMismatch == 0) {
        // All mismatches are from DELAY - expected!
        return;
    }
}
```

**Safety Implications:**

For burner safety, the delayed OFF ensures:
1. No burner enable signal gaps during mode transitions
2. Continuous enable signal prevents safety lockout
3. Hardware timer provides backup if software fails

Emergency shutdown uses `forceOffRelay()` to immediately cancel all DELAY timers.

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

## Heating Circulation Pump: Wilo Yonos PICO 25/1-4

High-efficiency electronically controlled circulator pump for space heating circuit.

### General Specifications

| Parameter | Value | Unit |
|-----------|-------|------|
| Type | Glandless wet-rotor | |
| Motor | Synchronous ECM | |
| Control | Electronic variable Δp | |
| Energy Efficiency Index (EEI) | ≤0.18 | |

### Electrical Specifications

| Parameter | Value | Unit |
|-----------|-------|------|
| Voltage | 1~230V ±10% | 50/60 Hz |
| Motor rated power (P2) | 15 | W |
| Power consumption (P1 max) | 20 | W |
| Power consumption (P1 min) | 4 | W |
| Speed range | 700-3400 | rpm |
| Cable gland | PG11 | |
| Insulation class | F | |
| Protection rating | IPX4D | |

### Hydraulic Performance

| Parameter | Value | Unit |
|-----------|-------|------|
| Max head (H max) | 4.3 | m |
| Max flow rate (Q max) | 2.7 | m³/h |
| Max operating pressure (PN) | 10 | bar |

### Physical Specifications

| Parameter | Value | Unit |
|-----------|-------|------|
| Connection (suction/pressure) | G 1½ | |
| Installation length (L0) | 180 | mm |
| Dimensions (D×W×H) | 180×131×102 | mm |
| Weight | 1.8 | kg |

### Materials

| Component | Material |
|-----------|----------|
| Pump housing | Grey cast iron |
| Impeller | PP-GF40 |
| Shaft | Stainless steel |
| Bearing | Carbon, metal-impregnated |

### Control Modes

| Mode | Description | Application |
|------|-------------|-------------|
| Δp-c | Differential pressure constant | Underfloor heating |
| Δp-v | Differential pressure variable | Radiator systems |
| Constant | 3 fixed pump curves | Storage charge |

### Operating Limits

| Parameter | Value | Unit |
|-----------|-------|------|
| Min fluid temperature | -10 | °C |
| Max fluid temperature | 95 | °C |
| Min ambient temperature | -10 | °C |
| Max ambient temperature | 40 | °C |
| Min inlet head at 50°C | 0.5 | m |
| Min inlet head at 95°C | 3 | m |

### EMC Compliance

- Emission: EN 61000-6-3
- Immunity: EN 61000-6-2
- EMC: EN 61800-3

**Sources:** [Wilo Product Page](https://wilo.com/gb/en/Products/en/products-expertise/wilo-yonos-pico/yonos-pico-25-1-4), [Wilo Datasheet PDF](https://cms.media.wilo.com/dcidocpfinder/wilo_f_0200002b000109a800010092/1270941/wilo_f_0200002b000109a800010092.pdf)

---

## Hot Water Tank Loading Pump: HST 25/4

Standard 3-speed circulator pump for hot water tank charging circuit.

### General Specifications

| Parameter | Value | Unit |
|-----------|-------|------|
| Type | Canned motor (wet-rotor) | |
| Bearing | Water-lubricated | |
| Speed settings | 3 | |
| Warranty | 3 | years |

### Electrical Specifications

| Speed | Power (P1) | Current (In) |
|-------|------------|--------------|
| I (Low) | 32 W | 0.15 A |
| II (Medium) | 50 W | 0.22 A |
| III (High) | 65 W | 0.28 A |

| Parameter | Value | Unit |
|-----------|-------|------|
| Voltage | 230 | V AC |

### Hydraulic Performance

| Parameter | Value | Unit |
|-----------|-------|------|
| Max flow rate (Q max) | 2.8 | m³/h |
| Max head | 4 | m |
| Connection size | 25 | mm (1") |

### HST Pump Family Comparison

| Model | Speed III | Speed II | Speed I | Max Flow |
|-------|-----------|----------|---------|----------|
| **HST 25/4** | **65W / 0.28A** | **50W / 0.22A** | **32W / 0.15A** | **2.8 m³/h** |
| HST 25/6 | 100W / 0.45A | 70W / 0.35A | 55W / 0.25A | - |
| HST 20/6 | 100W / 0.45A | 70W / 0.35A | 55W / 0.25A | - |
| HST 25/8 | 245W / 1.1A | 190W / 0.85A | 135W / 0.60A | - |

### Operating Limits

| Parameter | Value | Unit | Notes |
|-----------|-------|------|-------|
| Max dry run time | 10 | seconds | **CRITICAL** - water-lubricated bearing |
| Ambient temp | < fluid temp | | Must be cooler than pumped medium |

### Installation Notes

1. **Must be installed horizontally** - shaft orientation
2. **Bleed air before first use** - air lock prevention
3. **Cannot run dry** - water-lubricated bearing requires continuous fluid flow
4. Electrical work must be done by qualified technician

**Sources:** [HST Manual (ManualsLib)](https://www.manualslib.com/manual/1891245/Hst-Hst-25-4.html), [HST Industrie](https://www.hst-industrie.at/)

---

## Pump System Analysis

### Flow Rate Compatibility

| Component | Flow Requirement | Pump Capacity | Status |
|-----------|-----------------|---------------|--------|
| VIH Q 150 heating coil | 1.4 m³/h (ΔT=20K) | HST 25/4: 2.8 m³/h | ✓ 2x margin |
| Heating circuit | Variable | Yonos PICO: 2.7 m³/h | ✓ Adequate |

### Power Consumption

| Pump | Typical Operation | Annual (est.) |
|------|-------------------|---------------|
| Wilo Yonos PICO | 4-20W (modulating) | ~70 kWh |
| HST 25/4 | 32-65W (speed dependent) | ~30 kWh* |

*Hot water pump runs intermittently during tank charging only.

### Temperature Compatibility

| Pump | Max Fluid Temp | System Max | Status |
|------|----------------|------------|--------|
| Wilo Yonos PICO | 95°C | Boiler: 90°C | ✓ 5°C margin |
| HST 25/4 | 95°C* | Tank: 85°C | ✓ 10°C margin |

*HST bearing is water-lubricated - cannot run dry >10 seconds.

### Safety Considerations

1. **HST 25/4 dry-run protection**: System should not activate pump without confirmed water in circuit
2. **Wilo pump failure**: Loss of circulation → boiler overheat protection triggers
3. **Both pumps**: Monitor for blocked rotor via current sensing (future enhancement)
4. **NPSH at high temps**: Wilo requires 3m inlet head at 95°C to prevent cavitation

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
| 2025-12-13 | Added boiler type and minimum flow temperature section |
| 2025-12-14 | Corrected: VK 42/4-2 is low-temperature boiler (35°C min), not cast iron (55°C). Updated burner_low_limit default to 38°C. Added RYN4 DELAY command hardware behavior documentation |
| 2025-12-15 | Added Wilo Yonos PICO 25/1-4 heating pump and HST 25/4 hot water tank loading pump specifications |
