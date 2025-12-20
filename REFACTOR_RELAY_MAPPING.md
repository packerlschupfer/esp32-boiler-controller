# Relay Mapping Refactoring Task

## Problem Statement

Currently, relay channel numbers are hardcoded throughout the codebase in multiple locations:
- `BurnerSystemController::buildRelayStates()` - hardcoded array indices
- `RelayControlTask::updateSharedRelayReadings()` - hardcoded switch cases
- `BurnerSystemController::setPowerLevel()` - hardcoded relay index
- Various other files referencing specific relay numbers

This creates a maintenance nightmare when relay assignments need to change. We should only need to update the configuration in ONE place: `RelayConfigurations.cpp`.

## Current Architecture

1. **RelayConfigurations.cpp** - Defines relay mappings with comments
2. **SharedRelayReadings.h** - Defines state variables for each relay
3. **Multiple files** - Hardcoded relay indices (0-7) scattered throughout

## Desired Architecture

Create a centralized relay mapping system where:
1. **Single source of truth** - All relay channel assignments defined in ONE place
2. **Named constants** - Use symbolic names instead of magic numbers
3. **Type-safe access** - Compile-time checking of relay assignments

## Proposed Solution

### Option 1: Relay Index Enum/Namespace

Create a `RelayIndices.h` file with:

```cpp
namespace RelayChannel {
    constexpr uint8_t BURNER_HEATING = 0;  // Relay 1 (index 0)
    constexpr uint8_t BURNER_WATER = 1;    // Relay 2 (index 1)
    constexpr uint8_t HALF_POWER = 2;      // Relay 3 (index 2)
    constexpr uint8_t VALVE = 3;           // Relay 4 (index 3)
    constexpr uint8_t HEATING_PUMP = 4;    // Relay 5 (index 4)
    constexpr uint8_t WATER_PUMP = 5;      // Relay 6 (index 5)
    constexpr uint8_t SPARE_7 = 6;         // Relay 7 (index 6)
    constexpr uint8_t SPARE_8 = 7;         // Relay 8 (index 7)
}
```

Then update all code to use:
- `states[RelayChannel::HEATING_PUMP]` instead of `states[4]`
- `RelayControlTask::setRelayState(RelayChannel::HALF_POWER, ...)` instead of `setRelayState(2, ...)`

### Option 2: Central Relay Mapper Class

Create a `RelayMapper` class that provides:
- Mapping from logical function to physical relay index
- Single place to change mappings
- Runtime or compile-time configuration

```cpp
class RelayMapper {
public:
    enum class Function {
        BURNER_HEATING,
        BURNER_WATER,
        HALF_POWER,
        VALVE,
        HEATING_PUMP,
        WATER_PUMP,
        SPARE_7,
        SPARE_8
    };

    static uint8_t getIndex(Function function);
    static bool* getStatePtr(Function function);
};
```

## Requirements

1. **Zero runtime overhead** - Use constexpr/const where possible
2. **Backward compatibility** - Don't break existing functionality
3. **Single point of change** - When relay assignments change, update only ONE file
4. **Clear documentation** - Make it obvious where to change relay mappings
5. **Compile-time safety** - Catch errors at compile time, not runtime

## Files That Need Updating

### Core Files (must change):
- `src/modules/control/BurnerSystemController.cpp` - buildRelayStates(), setPowerLevel()
- `src/modules/tasks/RelayControlTask.cpp` - updateSharedRelayReadings()
- `src/shared/RelayConfigurations.cpp` - Already has the mapping, but needs to expose indices
- **NEW**: `include/relay/RelayIndices.h` or similar - Central definition file

### Files That Reference Relay Indices (should be updated to use symbolic names):
- `src/diagnostics/MQTTDiagnostics.cpp`
- `src/modules/tasks/MQTTTask.cpp`
- `src/modules/tasks/MonitoringTask.cpp`
- `src/modules/control/PumpCoordinator.cpp`
- `src/modules/control/BurnerSafetyValidator.cpp`
- `src/modules/control/CentralizedFailsafe.cpp`
- `src/utils/CriticalDataStorage.h`

## Implementation Steps

1. **Create central relay index definition file**
   - Define all relay indices as named constants
   - Add clear documentation about physical relay numbers vs array indices

2. **Update BurnerSystemController**
   - Replace all hardcoded array indices with named constants
   - Update comments to reference the central definition

3. **Update RelayControlTask**
   - Replace switch statement cases with named constants
   - Consider using a map/array for relay-to-state-variable mapping

4. **Update all other files**
   - Replace magic numbers with symbolic names
   - Add includes for the central relay definition file

5. **Update documentation**
   - Document the new system
   - Provide clear instructions for changing relay assignments

## Success Criteria

After this refactoring:
1. Changing relay assignments requires editing ONLY the central relay index file
2. All relay references use named constants (no magic numbers)
3. Compiler catches any invalid relay references
4. Existing functionality is unchanged
5. Code is more readable and maintainable

## Notes

- Physical relay numbers (1-8) vs array indices (0-7) should be clearly documented
- Consider whether some mappings should be runtime configurable via MQTT/config file
- Ensure the refactoring doesn't break any existing safety features
- Test thoroughly after changes - relay control is safety-critical

## Additional Consideration

The current `RelayConfigurations.cpp` already has the complete mapping. Consider whether we can:
1. Extract the relay indices from the existing `relayConfigurations` vector at compile time
2. Auto-generate the indices from the configuration
3. Make the configuration the single source of truth that code queries

This would be the most elegant solution but may be more complex to implement.

## Library Architecture Context

The ESP32-RYN4 library (which we own and can modify) defines hardware-level constants in `/Libraries/ESP32-RYN4/src/ryn4/RelayDefs.h`:
- Event group bits for each physical relay (RELAY1_OPEN_BIT, RELAY1_STATUS_BIT, etc.)
- These are **hardware constants** tied to the Modbus protocol
- These should **NOT** be changed - they represent the physical relay module's protocol

**Important separation of concerns:**
- **Library level** (ESP32-RYN4): Hardware protocol constants (RELAY1-8 bits)
- **Application level** (esp32-boiler-controller): Logical function to physical relay mapping

The refactoring is purely at the **application level** - we're creating a mapping layer between:
- Logical functions (HEATING_PUMP, BURNER_HEATING, etc.)
- Physical relay indices (0-7 in code, representing relays 1-8 on hardware)

## Relay Numbering Clarification

**CRITICAL**: There are THREE different numbering systems in play:

1. **Physical Relay Numbers** (1-8): How the hardware is labeled
2. **Array Indices** (0-7): How we access states in C++ vectors/arrays
3. **Modbus Register Offsets** (0-7): How the RYN4 library addresses relays

Currently in `RelayConfigurations.cpp`:
```cpp
{1, &SRP::getRelayReadings().relayBurnerHeating, ryn4::RELAY1_OPEN_BIT, ...}  // Physical relay 1
```
The first parameter `1` is the physical relay number (1-8).

But in `BurnerSystemController::buildRelayStates()`:
```cpp
states[0] = true;  // Array index 0 = Physical relay 1
```
This is confusing! We need ONE consistent logical naming system.

---

## Task for Claude Opus 4.5

Please implement Option 1 (Relay Index Namespace) with the following design:

### Design Choice
Create `include/relay/RelayIndices.h` that defines **LOGICAL** channel assignments as array indices (0-7):

```cpp
namespace RelayChannel {
    // Logical relay assignments (array indices 0-7)
    // These map to physical relays 1-8 on the RYN4 module
    constexpr uint8_t BURNER_HEATING = 0;  // Physical Relay 1 - Boiler Neutral 230V
    constexpr uint8_t BURNER_WATER = 1;    // Physical Relay 2 - Boiler Neutral 230V
    constexpr uint8_t HALF_POWER = 2;      // Physical Relay 3 - Boiler Neutral 230V
    constexpr uint8_t VALVE = 3;           // Physical Relay 4 - Spare
    constexpr uint8_t HEATING_PUMP = 4;    // Physical Relay 5 - 24V DC
    constexpr uint8_t WATER_PUMP = 5;      // Physical Relay 6 - 24V DC
    constexpr uint8_t SPARE_7 = 6;         // Physical Relay 7
    constexpr uint8_t SPARE_8 = 7;         // Physical Relay 8

    constexpr uint8_t MAX_RELAYS = 8;

    // Helper to convert index to physical relay number (for logging/debug)
    constexpr uint8_t toPhysical(uint8_t index) { return index + 1; }
}
```

### Implementation Requirements

1. **Create** `include/relay/RelayIndices.h` with the namespace above
2. **Update** `BurnerSystemController.cpp`:
   - Replace `states[0]` with `states[RelayChannel::BURNER_HEATING]`
   - Replace `setRelayState(2, ...)` with `setRelayState(RelayChannel::HALF_POWER, ...)`
   - Update all array index references

3. **Update** `RelayControlTask.cpp`:
   - Replace switch case magic numbers with `RelayChannel::` constants
   - Use `RelayChannel::toPhysical()` for logging physical relay numbers

4. **Update** all other files that reference relay indices

5. **IMPORTANT**: Keep `RelayConfigurations.cpp` using physical relay numbers (1-8) in the first field since that's what the RYN4 library expects. Only change the code that uses **array indices**.

6. **Documentation**: Add clear comments explaining the three numbering systems

7. **Verify**: No functionality changes - this is pure refactoring

### Success Criteria

After refactoring:
- To change relay assignments, only update `RelayIndices.h`
- No magic numbers in code - all use `RelayChannel::` constants
- Clear separation: library uses physical numbers, app code uses logical names
- All references use symbolic names that describe function, not numbers
