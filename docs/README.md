# ESPlan Boiler Controller Documentation

## Core Documentation

These documents provide comprehensive technical reference for the boiler controller system.

### System Architecture

#### [ARCHITECTURE_PATTERNS.md](ARCHITECTURE_PATTERNS.md)
Design patterns and architectural decisions used throughout the codebase.
- Singleton patterns
- Event-driven architecture
- Resource management
- Dependency injection

#### [EVENT_SYSTEM.md](EVENT_SYSTEM.md) ⭐
Complete reference for the event-driven architecture.
- **5 Event Groups** with all 85+ event bit definitions
- **Temperature Encoding** in BurnerRequest bits (16-23)
- **Event Patterns** and usage examples
- **Performance characteristics** and timing

**Start here** to understand inter-task communication.

#### [STATE_MACHINES.md](STATE_MACHINES.md) ⭐
State machine design and transitions.
- **Burner State Machine** (8 states with transition diagram)
- **Anti-Flapping Protection** logic
- **Safety Interlocks** integration
- **Pump Control** state machines
- **Entry/Exit Actions** for each state

**Essential** for understanding burner control logic.

#### [EVENT_FLOW.md](EVENT_FLOW.md) ⭐
End-to-end event flow scenarios.
- **Boot Sequence** (6 phases)
- **Water Heating Flow** (complete 9-step sequence)
- **Emergency Stop** sequence
- **MQTT Command Processing**
- **Sensor Data Propagation**
- **Timing Diagrams** and latency metrics

**Best for** understanding how the system works in practice.

### API Reference

#### [MQTT_API.md](MQTT_API.md) ⭐
Complete MQTT API documentation.
- **Topic Organization** (status, cmd, scheduler)
- **Command Reference** with JSON schemas
- **Data Format** (fixed-point temperature/pressure)
- **Examples** (bash, Python, Node.js, Home Assistant)
- **Testing Utilities** and troubleshooting
- **Quick Reference Card**

**Required** for integration and testing.

#### [OTA_UPDATE_GUIDE.md](OTA_UPDATE_GUIDE.md)
Firmware update procedures.
- Over-the-air update process
- MQTT-triggered updates
- Safety during updates
- Recovery procedures

### Planning & Future Work

#### [IMPROVEMENT_OPPORTUNITIES.md](IMPROVEMENT_OPPORTUNITIES.md)
Comprehensive analysis of remaining improvements.
- **Priority Classification** (Critical/High/Medium/Low)
- **Immediate Recommendations** (top 3 items)
- **Technical Debt Analysis** (~5% of codebase)
- **Hardware Integration** (flame sensor, interlocks)
- **Future Enhancements** (trending, efficiency metrics)
- **Code Quality Metrics**

**Use this** to plan future development work.

---

## Documentation Organization

```
docs/
├── README.md (this file)               # Documentation index
├── ARCHITECTURE_PATTERNS.md             # Design patterns
├── EVENT_SYSTEM.md ⭐                   # Event reference
├── STATE_MACHINES.md ⭐                 # State machine guide
├── EVENT_FLOW.md ⭐                     # Flow scenarios
├── MQTT_API.md ⭐                       # API reference
├── OTA_UPDATE_GUIDE.md                  # Firmware updates
├── IMPROVEMENT_OPPORTUNITIES.md         # Future work
└── archive/
    ├── legacy/                          # Old migration docs
    │   ├── HotWaterSchedulerTask.*      # Obsolete code
    │   ├── mqtt_*.md (13 files)         # Old MQTT docs
    │   ├── ANDRTF3_*.md (7 files)       # Integration notes
    │   ├── *event_driven*.md (10 files) # Migration docs
    │   ├── *watchdog*.md (5 files)      # Watchdog migration
    │   └── *session*.md (various)       # Work session logs
    └── optimization_guide.md            # Old optimization docs
        memory_optimization_report.md
        *Optimizer.h (5 headers)
```

---

## Quick Start Guide

### For New Developers

**Start Here** (in order):
1. Read [../CLAUDE.md](../CLAUDE.md) - Project overview and build notes
2. Read [EVENT_SYSTEM.md](EVENT_SYSTEM.md) - Understand event architecture
3. Read [STATE_MACHINES.md](STATE_MACHINES.md) - Learn burner control flow
4. Read [MQTT_API.md](MQTT_API.md) - Learn API for testing

### For Testing/Integration

**Essential Reading**:
- [MQTT_API.md](MQTT_API.md) - API reference with examples
- [OTA_UPDATE_GUIDE.md](OTA_UPDATE_GUIDE.md) - Firmware updates

**MQTT Testing Quick Commands**:
```bash
# Monitor sensors
mosquitto_sub -h 192.168.16.16 -u YOUR_MQTT_USER -P pass -t "boiler/status/sensors" -v

# List schedules
mosquitto_pub -t "boiler/cmd/scheduler/list" -m '{}'

# Add morning shower schedule
mosquitto_pub -t "boiler/cmd/scheduler/add" -m '{
  "type": "water_heating",
  "name": "Morning Shower",
  "start_hour": 6, "start_minute": 30,
  "end_hour": 8, "end_minute": 0,
  "days": [1,2,3,4,5],
  "target_temp": 55,
  "enabled": true
}'
```

### For Maintenance/Troubleshooting

**Useful References**:
- [EVENT_FLOW.md](EVENT_FLOW.md) - Understand system behavior
- [IMPROVEMENT_OPPORTUNITIES.md](IMPROVEMENT_OPPORTUNITIES.md) - Known issues/TODOs
- [MQTT_API.md](MQTT_API.md) - Troubleshooting section

---

## Key Concepts

### Fixed-Point Architecture

All temperatures and pressures use integer types for performance:

**Temperature_t** (tenths of °C):
```cpp
Temperature_t temp = 254;  // 25.4°C
int wholeDegrees = temp / 10;     // 25
int tenths = abs(temp % 10);      // 4
```

**Pressure_t** (hundredths of BAR):
```cpp
Pressure_t pressure = 152;  // 1.52 BAR
int bar = pressure / 100;         // 1
int hundredths = abs(pressure % 100);  // 52
```

**Benefits**:
- No floating-point CPU overhead
- Deterministic (consistent across devices)
- Smaller memory footprint
- MQTT sends integers directly

### Event-Driven Tasks

Tasks sleep until events occur (zero CPU when idle):

```cpp
while (true) {
    // Sleep until event (no polling!)
    EventBits_t bits = xEventGroupWaitBits(
        SRP::getSensorEventGroup(),
        SystemEvents::SensorUpdate::BOILER_OUTPUT,
        pdTRUE,  // Clear on read
        pdFALSE,
        pdMS_TO_TICKS(5000)  // 5s timeout
    );

    if (bits & BOILER_OUTPUT) {
        processNewTemperature();
    }
}
```

### Safety-First Design

Multiple redundant safety checks before burner ignition:
1. Pump running (circulation verified)
2. Sensors valid (minimum 2)
3. Temperature in range
4. Pressure in range (1.0-3.5 BAR)
5. No thermal shock
6. No emergency stop
7. Communication OK
8. Anti-flapping timers OK

**Any failure** → Burner blocked

---

## Configuration

### Build Modes

**ProjectConfig.h** defines 3 build modes:

| Mode | Stack Sizes | Logging | Use Case |
|------|-------------|---------|----------|
| `LOG_MODE_DEBUG_FULL` | Large (4KB) | Full verbose | Development |
| `LOG_MODE_DEBUG_SELECTIVE` | Medium (3-4KB) | Selective | Testing |
| `LOG_MODE_RELEASE` | Small (1.5-2KB) | Minimal | Production |

### Important Build Flags

```cpp
// Hardware configuration
#define USE_REAL_PRESSURE_SENSOR  // Undefined = fake data with warning

// Logging mode (define only one)
#define LOG_MODE_DEBUG_SELECTIVE
```

### System Constants

**All configuration centralized in** `src/config/SystemConstants.h`:
- `Safety::Pressure` - Operating/alarm limits
- `Temperature::SpaceHeating` - Comfort/eco/frost defaults
- `PID::Autotune` - Tuning parameters
- `Burner` - Timing constants
- `Hardware::PressureSensor` - 4-20mA calibration
- `Simulation` - Fake sensor parameters

---

## Archive Contents

The `archive/legacy/` directory contains:
- **Migration Guides** (completed migrations)
- **Session Logs** (development history)
- **Old APIs** (replaced implementations)
- **Test Results** (historical)
- **Obsolete Code** (HotWaterSchedulerTask, optimizers)

**Purpose**: Preserve git history and rationale for changes

**Not Needed For**: Current development (use core docs above)

---

## Documentation Standards

### File Naming
- `UPPERCASE_WITH_UNDERSCORES.md` - Core reference docs
- `lowercase-with-dashes.md` - Historical/legacy docs
- Prefix indicates category (MQTT_, EVENT_, STATE_)

### Content Structure
1. Overview section
2. Table of contents (for long docs)
3. Code examples with syntax highlighting
4. Usage patterns and best practices
5. Troubleshooting section (where applicable)

### Maintenance
- Update CLAUDE.md for project-wide changes
- Add entries to IMPROVEMENT_OPPORTUNITIES.md for TODOs
- Archive completed migration guides
- Keep core docs (7 files) up to date

---

## Getting Help

### Quick Questions
Check [../CLAUDE.md](../CLAUDE.md) - "Key Lessons" section

### MQTT Issues
See [MQTT_API.md](MQTT_API.md) - Troubleshooting section

### Event/State Machine Questions
See [EVENT_SYSTEM.md](EVENT_SYSTEM.md) and [STATE_MACHINES.md](STATE_MACHINES.md)

### Build/Configuration Issues
Check `src/config/ProjectConfig.h` comments

### Future Improvements
See [IMPROVEMENT_OPPORTUNITIES.md](IMPROVEMENT_OPPORTUNITIES.md)

---

## Contributing

When adding new features:
1. Update relevant core documentation
2. Add MQTT API changes to MQTT_API.md
3. Document new events in EVENT_SYSTEM.md
4. Add state changes to STATE_MACHINES.md
5. Update IMPROVEMENT_OPPORTUNITIES.md (remove completed items)

---

Last Updated: 2025-11-24
Documentation Version: 2.0 (post-consolidation)
