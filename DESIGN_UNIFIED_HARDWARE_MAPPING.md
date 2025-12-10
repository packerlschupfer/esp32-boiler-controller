# Unified Hardware Mapping Design

## ✅ IMPLEMENTATION STATUS: COMPLETE

**Date Completed**: 2025-12-05
**Build Status**: ✅ SUCCESS
**Memory Impact**: RAM: 14.1% (46,184 bytes), Flash: 29.3% (1,228,679 bytes)

### What Was Implemented:

✅ **All Libraries Updated**:
- ESP32-RYN4: Unified mapping + std::array + int16_t bindings (4 commits)
- ESP32-MB8ART: Unified mapping + Temperature_t migration (5 commits)
- ESP32-ANDRTF3: Simple pointer binding (2 commits)

✅ **Main Project Integration**:
- RelayIndices.h & SensorIndices.h (single source of truth)
- RelayHardwareConfig.h & SensorHardwareConfig.h (constexpr in flash)
- RelayBindings & SensorBindings (runtime pointer initialization)
- Field renames for consistency (relayHeatingPump, waterHeaterTempTank)
- Magic numbers replaced with named constants
- Legacy code removed (RelayConfigurations, TempSensorMapping)

✅ **Breaking Changes Completed**:
- No backward compatibility - clean break
- All vector-based mapping removed
- Libraries now use pointer bindings exclusively

---

## Problem Statement (Original Design Goals)

Hardware mappings (relays and sensors) are currently hardcoded in multiple places throughout the codebase. Changing a relay assignment or sensor channel requires editing many files, which is error-prone and maintenance-heavy.

### Current Issues

1. **Relay mappings** scattered across 10+ files with magic numbers like `states[0]`, `states[4]`
2. **Sensor mappings** hardcoded with channel indices like `{0, &readings.boilerTempOutput, ...}`
3. **No single source of truth** - changes require updating multiple files
4. **Runtime overhead** - using `std::vector` with heap allocation for static data

### Affected Files

**Relay System:**
- `src/shared/RelayConfigurations.cpp`
- `src/modules/control/BurnerSystemController.cpp`
- `src/modules/tasks/RelayControlTask.cpp`
- `src/modules/control/PumpCoordinator.cpp`
- `src/modules/control/BurnerSafetyValidator.cpp`
- `src/modules/control/CentralizedFailsafe.cpp`
- `src/modules/tasks/MQTTTask.cpp`
- `src/modules/tasks/MonitoringTask.cpp`
- `src/diagnostics/MQTTDiagnostics.cpp`
- `src/utils/CriticalDataStorage.h`

**Sensor System:**
- `src/shared/TempSensorMapping.cpp`
- Files referencing sensor channels by index

---

## Design Goals

1. **Single Source of Truth** - Change mappings in ONE file only
2. **Zero Runtime Overhead** - Hardware configs in flash/ROM (constexpr)
3. **Minimal RAM Usage** - Only runtime pointers in RAM
4. **Type Safety** - Named constants, compile-time checking
5. **Consistent Pattern** - Same approach for relays and sensors
6. **Clear Documentation** - Obvious where and how to change mappings

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                    SINGLE SOURCE OF TRUTH                       │
│                                                                 │
│  include/config/RelayIndices.h    include/config/SensorIndices.h│
│  ┌─────────────────────────┐     ┌─────────────────────────────┐│
│  │ namespace RelayIndex {  │     │ namespace SensorIndex {     ││
│  │   HEATING_PUMP   = 0    │     │   BOILER_OUTPUT  = 0        ││
│  │   WATER_PUMP     = 1    │     │   BOILER_RETURN  = 1        ││
│  │   BURNER_ENABLE  = 2    │     │   WATER_TANK     = 2        ││
│  │   ...                   │     │   ...                       ││
│  │ }                       │     │ }                           ││
│  └─────────────────────────┘     └─────────────────────────────┘│
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    FLASH/ROM (constexpr)                        │
│                                                                 │
│  Hardware configuration that never changes at runtime           │
│                                                                 │
│  RelayHardwareConfig[8]           SensorHardwareConfig[8]       │
│  ┌─────────────────────────┐     ┌─────────────────────────────┐│
│  │ physicalNumber (1-8)    │     │ channelNumber (0-7)         ││
│  │ openBit, closeBit       │     │ updateEventBit              ││
│  │ statusBit, updateBit    │     │ errorEventBit               ││
│  │ errorBit                │     │ isActive                    ││
│  └─────────────────────────┘     └─────────────────────────────┘│
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    RAM (runtime init)                           │
│                                                                 │
│  Pointers assigned once at startup based on Index constants     │
│                                                                 │
│  relayStatePointers[8]            sensorDataPointers[8]         │
│  ┌─────────────────────────┐     ┌─────────────────────────────┐│
│  │ bool* statePtr          │     │ Temperature_t* valuePtr     ││
│  │                         │     │ bool* validityPtr           ││
│  └─────────────────────────┘     └─────────────────────────────┘│
│                                                                 │
│  32 bytes (8 pointers)            64 bytes (16 pointers)        │
└─────────────────────────────────────────────────────────────────┘
```

---

## Detailed Design

### 1. Index Definition Files (Single Source of Truth)

#### `include/config/RelayIndices.h`

```cpp
#pragma once
#include <cstdint>

namespace RelayIndex {
    // =========================================================
    // SINGLE SOURCE OF TRUTH FOR RELAY ASSIGNMENTS
    //
    // To change relay assignments:
    // 1. Change ONLY the values below
    // 2. Rebuild the project
    // 3. No other files need modification
    // =========================================================

    // Logical function → Array index (0-7)
    // Array index maps to physical relay: physical = index + 1

    constexpr uint8_t HEATING_PUMP   = 0;  // Physical Relay 1 - Heating circulation pump
    constexpr uint8_t WATER_PUMP     = 1;  // Physical Relay 2 - Water heating circulation pump
    constexpr uint8_t BURNER_ENABLE  = 2;  // Physical Relay 3 - Burner on/off control
    constexpr uint8_t HALF_POWER     = 3;  // Physical Relay 4 - Half power selector (ON=half, OFF=full)
    constexpr uint8_t WATER_MODE     = 4;  // Physical Relay 5 - Water heating mode (disables over-temp safety)
    constexpr uint8_t VALVE          = 5;  // Physical Relay 6 - Valve control
    constexpr uint8_t SPARE_7        = 6;  // Physical Relay 7 - Spare
    constexpr uint8_t SPARE_8        = 7;  // Physical Relay 8 - Spare

    constexpr uint8_t MAX_RELAYS = 8;

    // Convert array index to physical relay number (1-8)
    constexpr uint8_t toPhysical(uint8_t index) { return index + 1; }

    // Convert physical relay number (1-8) to array index (0-7)
    constexpr uint8_t fromPhysical(uint8_t physical) { return physical - 1; }
}
```

#### `include/config/SensorIndices.h`

```cpp
#pragma once
#include <cstdint>

namespace SensorIndex {
    // =========================================================
    // SINGLE SOURCE OF TRUTH FOR SENSOR ASSIGNMENTS
    //
    // To change sensor channel assignments:
    // 1. Change ONLY the values below
    // 2. Rebuild the project
    // 3. No other files need modification
    // =========================================================

    // Logical function → MB8ART channel index (0-7)

    constexpr uint8_t BOILER_OUTPUT   = 0;  // CH0 - Boiler output temperature
    constexpr uint8_t BOILER_RETURN   = 1;  // CH1 - Boiler return temperature
    constexpr uint8_t WATER_TANK      = 2;  // CH2 - Water heater tank temperature
    constexpr uint8_t WATER_OUTPUT    = 3;  // CH3 - Water heater output temperature
    constexpr uint8_t WATER_RETURN    = 4;  // CH4 - Water heater return temperature
    constexpr uint8_t HEATING_RETURN  = 5;  // CH5 - Heating system return temperature
    constexpr uint8_t OUTSIDE         = 6;  // CH6 - Outside temperature
    // CH7 - Reserved for pressure sensor (handled separately)

    constexpr uint8_t MAX_TEMP_SENSORS = 7;  // Temperature sensors (CH0-CH6)
    constexpr uint8_t PRESSURE_CHANNEL = 7;  // CH7 - Pressure sensor (4-20mA)

    // Convert index to MB8ART channel (currently 1:1, but allows future flexibility)
    constexpr uint8_t toChannel(uint8_t index) { return index; }
}

namespace ANDRTF3Index {
    // ANDRTF3 sensor assignments (separate device)
    constexpr uint8_t INSIDE_TEMP     = 0;  // Inside/room temperature
    constexpr uint8_t INSIDE_HUMIDITY = 1;  // Inside humidity

    constexpr uint8_t MAX_ANDRTF3_SENSORS = 2;
}
```

---

### 2. Hardware Configuration (Flash/ROM)

#### `include/config/RelayHardwareConfig.h`

```cpp
#pragma once
#include <array>
#include <cstdint>
#include "ryn4/RelayDefs.h"
#include "RelayIndices.h"

namespace RelayHardware {

    struct Config {
        uint8_t physicalNumber;    // Physical relay number (1-8)
        uint32_t openBit;          // Event bit to open relay
        uint32_t closeBit;         // Event bit to close relay
        uint32_t statusBit;        // Event bit for status
        uint32_t updateBit;        // Event bit for update notification
        uint32_t errorBit;         // Event bit for error indication
        bool inverseLogic;         // true = relay OFF means device ON
    };

    // Hardware configuration indexed by RelayIndex constants
    // Lives in flash - no RAM cost
    constexpr std::array<Config, RelayIndex::MAX_RELAYS> CONFIGS = {{
        // Index 0
        {RelayIndex::toPhysical(0), ryn4::RELAY_OPEN_BITS[0], ryn4::RELAY_CLOSE_BITS[0],
         ryn4::RELAY_STATUS_BITS[0], ryn4::RELAY_UPDATE_BITS[0], ryn4::RELAY_ERROR_BITS[0], false},
        // Index 1
        {RelayIndex::toPhysical(1), ryn4::RELAY_OPEN_BITS[1], ryn4::RELAY_CLOSE_BITS[1],
         ryn4::RELAY_STATUS_BITS[1], ryn4::RELAY_UPDATE_BITS[1], ryn4::RELAY_ERROR_BITS[1], false},
        // Index 2
        {RelayIndex::toPhysical(2), ryn4::RELAY_OPEN_BITS[2], ryn4::RELAY_CLOSE_BITS[2],
         ryn4::RELAY_STATUS_BITS[2], ryn4::RELAY_UPDATE_BITS[2], ryn4::RELAY_ERROR_BITS[2], false},
        // Index 3
        {RelayIndex::toPhysical(3), ryn4::RELAY_OPEN_BITS[3], ryn4::RELAY_CLOSE_BITS[3],
         ryn4::RELAY_STATUS_BITS[3], ryn4::RELAY_UPDATE_BITS[3], ryn4::RELAY_ERROR_BITS[3], false},
        // Index 4
        {RelayIndex::toPhysical(4), ryn4::RELAY_OPEN_BITS[4], ryn4::RELAY_CLOSE_BITS[4],
         ryn4::RELAY_STATUS_BITS[4], ryn4::RELAY_UPDATE_BITS[4], ryn4::RELAY_ERROR_BITS[4], false},
        // Index 5
        {RelayIndex::toPhysical(5), ryn4::RELAY_OPEN_BITS[5], ryn4::RELAY_CLOSE_BITS[5],
         ryn4::RELAY_STATUS_BITS[5], ryn4::RELAY_UPDATE_BITS[5], ryn4::RELAY_ERROR_BITS[5], false},
        // Index 6
        {RelayIndex::toPhysical(6), ryn4::RELAY_OPEN_BITS[6], ryn4::RELAY_CLOSE_BITS[6],
         ryn4::RELAY_STATUS_BITS[6], ryn4::RELAY_UPDATE_BITS[6], ryn4::RELAY_ERROR_BITS[6], false},
        // Index 7
        {RelayIndex::toPhysical(7), ryn4::RELAY_OPEN_BITS[7], ryn4::RELAY_CLOSE_BITS[7],
         ryn4::RELAY_STATUS_BITS[7], ryn4::RELAY_UPDATE_BITS[7], ryn4::RELAY_ERROR_BITS[7], false},
    }};

    // Helper to get config by logical index
    constexpr const Config& get(uint8_t index) {
        return CONFIGS[index];
    }
}
```

#### `include/config/SensorHardwareConfig.h`

```cpp
#pragma once
#include <array>
#include <cstdint>
#include "events/SystemEventsGenerated.h"
#include "SensorIndices.h"

namespace SensorHardware {

    struct Config {
        uint8_t channelNumber;     // MB8ART channel (0-7)
        uint32_t updateEventBit;   // Event bit for successful update
        uint32_t errorEventBit;    // Event bit for error indication
        bool isActive;             // Sensor is in use
    };

    // Hardware configuration indexed by SensorIndex constants
    // Lives in flash - no RAM cost
    constexpr std::array<Config, 8> CONFIGS = {{
        {SensorIndex::toChannel(0), SystemEvents::SensorUpdate::BOILER_OUTPUT,
         SystemEvents::SensorUpdate::BOILER_OUTPUT_ERROR, true},
        {SensorIndex::toChannel(1), SystemEvents::SensorUpdate::BOILER_RETURN,
         SystemEvents::SensorUpdate::BOILER_RETURN_ERROR, true},
        {SensorIndex::toChannel(2), SystemEvents::SensorUpdate::WATER_TANK,
         SystemEvents::SensorUpdate::WATER_TANK_ERROR, true},
        {SensorIndex::toChannel(3), SystemEvents::SensorUpdate::WATER_OUTPUT,
         SystemEvents::SensorUpdate::WATER_OUTPUT_ERROR, true},
        {SensorIndex::toChannel(4), SystemEvents::SensorUpdate::WATER_RETURN,
         SystemEvents::SensorUpdate::WATER_RETURN_ERROR, true},
        {SensorIndex::toChannel(5), SystemEvents::SensorUpdate::HEATING_RETURN,
         SystemEvents::SensorUpdate::HEATING_RETURN_ERROR, true},
        {SensorIndex::toChannel(6), SystemEvents::SensorUpdate::OUTSIDE,
         SystemEvents::SensorUpdate::OUTSIDE_ERROR, true},
        {SensorIndex::toChannel(7), SystemEvents::SensorUpdate::PRESSURE,
         SystemEvents::SensorUpdate::PRESSURE_ERROR, true},  // Pressure sensor
    }};

    constexpr const Config& get(uint8_t index) {
        return CONFIGS[index];
    }
}
```

---

### 3. Runtime Pointer Binding (RAM)

#### `src/shared/RelayBindings.h`

```cpp
#pragma once
#include <array>
#include "config/RelayIndices.h"

namespace RelayBindings {

    struct Pointers {
        bool* statePtr;            // Pointer to relay state in SharedRelayReadings
    };

    // Runtime pointers - lives in RAM
    // Initialized once at startup
    extern std::array<Pointers, RelayIndex::MAX_RELAYS> pointers;

    // Initialize pointer bindings based on RelayIndex assignments
    void initialize();

    // Get state pointer for a relay
    inline bool* getStatePtr(uint8_t index) {
        return pointers[index].statePtr;
    }
}
```

#### `src/shared/RelayBindings.cpp`

```cpp
#include "RelayBindings.h"
#include "config/RelayIndices.h"
#include "SharedRelayReadings.h"
#include "core/SystemResourceProvider.h"

namespace RelayBindings {

    std::array<Pointers, RelayIndex::MAX_RELAYS> pointers = {};

    void initialize() {
        auto& readings = SRP::getRelayReadings();

        // Bind logical functions to their state variables
        // This is the ONLY place that connects RelayIndex to SharedRelayReadings
        pointers[RelayIndex::HEATING_PUMP].statePtr  = &readings.relayHpump;
        pointers[RelayIndex::WATER_PUMP].statePtr    = &readings.relayWhpump;
        pointers[RelayIndex::BURNER_ENABLE].statePtr = &readings.relayBurnerEnable;
        pointers[RelayIndex::HALF_POWER].statePtr    = &readings.relayHalfPower;
        pointers[RelayIndex::WATER_MODE].statePtr    = &readings.relayWheaterMode;
        pointers[RelayIndex::VALVE].statePtr         = &readings.relayValve;
        pointers[RelayIndex::SPARE_7].statePtr       = nullptr;
        pointers[RelayIndex::SPARE_8].statePtr       = nullptr;
    }
}
```

#### `src/shared/SensorBindings.h`

```cpp
#pragma once
#include <array>
#include "config/SensorIndices.h"
#include "SharedSensorReadings.h"

namespace SensorBindings {

    struct Pointers {
        Temperature_t* valuePtr;   // Pointer to temperature value
        bool* validityPtr;         // Pointer to validity flag
    };

    extern std::array<Pointers, 8> pointers;

    void initialize();

    inline Temperature_t* getValuePtr(uint8_t index) {
        return pointers[index].valuePtr;
    }

    inline bool* getValidityPtr(uint8_t index) {
        return pointers[index].validityPtr;
    }
}
```

#### `src/shared/SensorBindings.cpp`

```cpp
#include "SensorBindings.h"
#include "config/SensorIndices.h"
#include "core/SystemResourceProvider.h"

namespace SensorBindings {

    std::array<Pointers, 8> pointers = {};

    void initialize() {
        auto& readings = SRP::getSensorReadings();

        // Bind logical functions to their data variables
        pointers[SensorIndex::BOILER_OUTPUT]  = {&readings.boilerTempOutput,  &readings.isBoilerTempOutputValid};
        pointers[SensorIndex::BOILER_RETURN]  = {&readings.boilerTempReturn,  &readings.isBoilerTempReturnValid};
        pointers[SensorIndex::WATER_TANK]     = {&readings.wHeaterTempTank,   &readings.isWHeaterTempTankValid};
        pointers[SensorIndex::WATER_OUTPUT]   = {&readings.wHeaterTempOutput, &readings.isWHeaterTempOutputValid};
        pointers[SensorIndex::WATER_RETURN]   = {&readings.wHeaterTempReturn, &readings.isWHeaterTempReturnValid};
        pointers[SensorIndex::HEATING_RETURN] = {&readings.heatingTempReturn, &readings.isHeatingTempReturnValid};
        pointers[SensorIndex::OUTSIDE]        = {&readings.outsideTemp,       &readings.isOutsideTempValid};
        pointers[SensorIndex::PRESSURE_CHANNEL] = {nullptr, nullptr};  // Pressure handled separately
    }
}
```

---

### 4. Usage Examples

#### Before (magic numbers):
```cpp
// BurnerSystemController.cpp
void BurnerSystemController::buildRelayStates(bool* states, ...) {
    states[0] = heatingPump;   // What is relay 0?
    states[1] = waterPump;     // What is relay 1?
    states[2] = true;          // BURNER_ENABLE?
    states[4] = false;         // WATER_MODE?
    states[3] = (power == PowerLevel::HALF);
}
```

#### After (named constants):
```cpp
// BurnerSystemController.cpp
#include "config/RelayIndices.h"

void BurnerSystemController::buildRelayStates(bool* states, ...) {
    states[RelayIndex::HEATING_PUMP]  = heatingPump;
    states[RelayIndex::WATER_PUMP]    = waterPump;
    states[RelayIndex::BURNER_ENABLE] = true;
    states[RelayIndex::WATER_MODE]    = false;
    states[RelayIndex::HALF_POWER]    = (power == PowerLevel::HALF);
}
```

#### Changing relay assignments:
```cpp
// BEFORE: HEATING_PUMP on relay 1, BURNER_ENABLE on relay 3
constexpr uint8_t HEATING_PUMP   = 0;  // Relay 1
constexpr uint8_t BURNER_ENABLE  = 2;  // Relay 3

// AFTER: Swap them - ONLY change these two lines!
constexpr uint8_t HEATING_PUMP   = 2;  // Now Relay 3
constexpr uint8_t BURNER_ENABLE  = 0;  // Now Relay 1
```

---

### 5. Library Integration

The ESP32-RYN4 library needs to accept the new configuration format. Options:

#### Option A: Adapter in Application
Keep library unchanged, create adapter in application code:

```cpp
// src/shared/RelayConfigAdapter.cpp
std::vector<base::BaseRelayMapping> buildRelayMappings() {
    std::vector<base::BaseRelayMapping> mappings;
    mappings.reserve(RelayIndex::MAX_RELAYS);

    for (uint8_t i = 0; i < RelayIndex::MAX_RELAYS; ++i) {
        const auto& hw = RelayHardware::get(i);
        mappings.push_back({
            hw.physicalNumber,
            RelayBindings::getStatePtr(i),
            hw.openBit,
            hw.closeBit,
            hw.statusBit,
            hw.updateBit,
            hw.errorBit,
            false,  // isOn - initial state
            hw.inverseLogic
        });
    }
    return mappings;
}
```

#### Option B: Update Library to Use New Format
Modify ESP32-RYN4 to accept `std::array` and separate pointer binding.

**Recommendation:** Start with Option A (adapter) for minimal library changes, migrate to Option B later if beneficial.

---

### 6. Initialization Sequence

```cpp
// In main.cpp or system initialization
void initializeHardwareMappings() {
    // 1. Initialize pointer bindings (RAM)
    RelayBindings::initialize();
    SensorBindings::initialize();

    // 2. Build legacy format for libraries (if needed)
    auto relayMappings = buildRelayMappings();
    auto sensorMappings = buildSensorMappings();

    // 3. Pass to libraries
    ryn4Controller.initialize(relayMappings);
    mb8artController.initialize(sensorMappings);
}
```

---

## Memory Analysis

### Current (std::vector):
| Item | RAM | Flash |
|------|-----|-------|
| relayConfigurations vector | ~200 bytes (heap) | - |
| sensorMappings vector | ~140 bytes (heap) | - |
| **Total** | **~340 bytes heap** | - |

### New Design (Option C):
| Item | RAM | Flash |
|------|-----|-------|
| RelayHardware::CONFIGS | - | ~168 bytes |
| SensorHardware::CONFIGS | - | ~96 bytes |
| RelayBindings::pointers | 32 bytes | - |
| SensorBindings::pointers | 64 bytes | - |
| **Total** | **96 bytes static** | **264 bytes** |

**Benefits:**
- No heap fragmentation
- Predictable memory layout
- Hardware config survives stack corruption
- 244 bytes less RAM (and static vs heap)

---

## Implementation Plan

### Phase 1: Create Index Files
1. Create `include/config/RelayIndices.h`
2. Create `include/config/SensorIndices.h`
3. No functionality changes yet

### Phase 2: Create Hardware Configs
1. Create `include/config/RelayHardwareConfig.h`
2. Create `include/config/SensorHardwareConfig.h`
3. Verify constexpr compiles correctly

### Phase 3: Create Binding System
1. Create `src/shared/RelayBindings.cpp/h`
2. Create `src/shared/SensorBindings.cpp/h`
3. Add initialization calls to startup

### Phase 4: Create Adapters
1. Create adapter to build legacy `std::vector` format
2. Ensure libraries continue working unchanged

### Phase 5: Update Application Code
1. Replace magic numbers with `RelayIndex::` constants
2. Replace magic numbers with `SensorIndex::` constants
3. Update all affected files

### Phase 6: Testing
1. Verify all relay operations work
2. Verify all sensor readings work
3. Test changing an assignment (modify index, rebuild, verify)

### Phase 7: Cleanup
1. Remove old `RelayConfigurations.cpp` (replaced by adapter)
2. Remove old `TempSensorMapping.cpp` (replaced by adapter)
3. Update documentation

---

## Success Criteria

After refactoring:

1. **Single Source of Truth**: Changing `RelayIndex::HEATING_PUMP = 0` to `= 4` requires NO other file changes
2. **Zero Magic Numbers**: All relay/sensor references use named constants
3. **Compile-Time Safety**: Invalid indices caught at compile time
4. **No Functionality Changes**: System behaves identically
5. **Reduced RAM**: ~244 bytes less heap usage
6. **Clear Documentation**: Any developer can understand how to change mappings

---

## Design Decisions

1. **SharedRelayReadings field names**: **YES** - Rename fields like `relayHpump` to match new naming convention (e.g., `relayHeatingPump`)
   - Provides consistency across the codebase
   - MQTT topic names will be updated accordingly

2. **ANDRTF3 integration**: **YES** - Include ANDRTF3 sensors in this refactor
   - Same pattern for all hardware mappings
   - Consistent developer experience

3. **Library updates**: **YES** - Update ESP32-RYN4 and ESP32-MB8ART to natively support new format
   - No adapter layer needed
   - Cleaner architecture
   - Libraries updated as part of this refactor

---

## File Structure After Refactoring

```
include/
└── config/
    ├── RelayIndices.h          # Single source of truth for relay assignments
    ├── SensorIndices.h         # Single source of truth for sensor assignments
    ├── RelayHardwareConfig.h   # Constexpr hardware config (flash)
    └── SensorHardwareConfig.h  # Constexpr hardware config (flash)

src/
└── shared/
    ├── RelayBindings.h         # Runtime pointer declarations
    ├── RelayBindings.cpp       # Pointer initialization
    ├── SensorBindings.h        # Runtime pointer declarations
    ├── SensorBindings.cpp      # Pointer initialization
    ├── RelayConfigAdapter.cpp  # Builds legacy vector format
    └── SensorConfigAdapter.cpp # Builds legacy vector format
```
