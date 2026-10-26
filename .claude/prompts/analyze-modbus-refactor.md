# Sonnet 1M Analysis: ModbusCoordinator Refactoring

## Context

We refactored the ESP32 boiler controller to eliminate Modbus bus contention by scheduling all RYN4 relay operations via ModbusCoordinator. Previously, RelayControlTask did immediate Modbus operations with a "bus busy" signaling mechanism that had timing issues (100ms grace period was too short for 350-400ms relay transactions).

## New Architecture

### Tick Schedule (500ms per tick, 10 ticks = 5s cycle)
```
Tick 0: ANDRTF3 (room temp sensor)
Tick 1: RYN4_SET (batch write queued relay changes)
Tick 2: MB8ART (boiler temperature sensors)
Tick 3: RYN4_READ (verify relay states)
Tick 4: (idle)
Tick 5: MB8ART (boiler temps)
Tick 6: RYN4_SET (batch write)
Tick 7: (idle)
Tick 8: RYN4_READ (verify)
Tick 9: (idle)
```

### Data Flow
```
HeatingControlTask/WaterControlTask/BurnerController
    │
    ▼ (sets event bits in relayRequestEventGroup)
RelayControlTask::processRelayRequests()
    │
    ▼ (updates atomic state)
g_relayState.desired (shared/RelayState.h)
    │
    ▼ (at SET tick)
RYN4ProcessingTask::handleSetTick()
    │
    ▼ (calls RYN4 library)
ryn4->setMultipleRelayStates()
    │
    ▼ (at READ tick)
RYN4ProcessingTask::handleReadTick()
    │
    ▼ (calls RYN4 library)
ryn4->readBitmapStatus()
    │
    ▼ (compares, auto-retries on mismatch)
g_relayState.actual
```

## Files to Analyze

### Core Changes
1. **src/shared/RelayState.h** - New atomic state struct
2. **src/shared/RelayState.cpp** - Global instance
3. **src/core/ModbusCoordinator.h** - Added RYN4_SET/RYN4_READ, removed bus busy
4. **src/core/ModbusCoordinator.cpp** - Updated tick processing, uses xTaskNotify with SensorType value

### Task Changes
5. **src/modules/tasks/RYN4ProcessingTask.cpp** - Rewritten for SET/READ handling
6. **src/modules/tasks/RelayControlTask.cpp** - Removed Modbus, uses g_relayState
7. **src/modules/tasks/ANDRTF3Task.cpp** - Uses xTaskNotifyWait, removed isBusAvailable
8. **src/modules/tasks/MB8ARTTasks.cpp** - Uses xTaskNotifyWait, removed isBusAvailable

### Safety-Critical Paths
9. **src/modules/control/BurnerSystemController.cpp** - emergencyShutdown() calls RelayControlTask::setAllRelays()
10. **src/modules/control/CentralizedFailsafe.cpp** - Failsafe triggers emergencyShutdown()

## Analysis Questions

### Thread Safety
1. Is the atomic usage in RelayState.h correct? Are the memory orderings appropriate?
2. Could there be race conditions between RelayControlTask updating `desired` and RYN4ProcessingTask reading it?
3. Is the `pendingWrite` flag exchange safe in handleSetTick()?

### Timing & Latency
4. With up to 2.5s relay command latency, are there scenarios where this could cause safety issues?
5. Emergency shutdown now has up to 2.5s delay - is this acceptable for a boiler controller?
6. Could accumulated delays cause issues (e.g., multiple relay changes queued)?

### Error Handling
7. What happens if RYN4ProcessingTask fails to write at a SET tick? (Currently re-queues for next tick)
8. What happens on persistent mismatch between desired and actual? (Currently just logs and retries)
9. Should there be escalation to CentralizedFailsafe after N consecutive mismatches?

### Architecture
10. Is the separation of concerns correct? (RelayControlTask = policy, RYN4ProcessingTask = mechanism)
11. Should RelayControlTask still maintain `currentRelayStates[]` or rely entirely on g_relayState?
12. Is the dual notification registration (RYN4_SET and RYN4_READ for same task handle) working correctly?

### Edge Cases
13. What happens during system startup before RYN4ProcessingTask registers with coordinator?
14. What if RYN4ProcessingTask misses a notification (e.g., blocked on mutex)?
15. What happens if coordinator and direct relay calls happen simultaneously? (Should not happen now, but verify)

### Performance
16. Is the notification mechanism efficient? (xTaskNotify vs xTaskNotifyGive)
17. Could the 500ms tick interval be reduced for faster response? Trade-offs?
18. Memory usage of atomic operations vs mutex-protected operations?

## Expected Output

Please provide:
1. **Critical Issues**: Any bugs or safety concerns that must be fixed
2. **Recommendations**: Improvements for robustness or clarity
3. **Verification Checklist**: Tests to run to verify the refactoring works correctly
4. **Documentation Updates**: What docs need updating for this change

## Additional Context

- This is a real boiler controller running in production (with supervision)
- The RYN4 controls: burner enable, heating pump, water pump, power boost, water mode valve
- Safety is paramount - burner OFF must work reliably
- The user approved 2.5s latency for relay commands including emergency shutdown
