# Mutex Hierarchy Documentation

**Purpose:** Prevent deadlocks by defining a strict mutex acquisition order.

**Rule:** Always acquire mutexes in ascending level order. Never hold a higher-level mutex when acquiring a lower-level one.

## Hierarchy Levels

### Level 1: Hardware Communication (Lowest - Acquire First)
| Mutex | Location | Purpose | Timeout |
|-------|----------|---------|---------|
| `framMutex_` | CriticalDataStorage.cpp | I2C FRAM access | 500ms |
| `ModbusCoordinator::mutex` | ModbusCoordinator.cpp | RS485 bus access | 100ms |

### Level 2: Shared Data Structures
| Mutex | Location | Purpose | Timeout |
|-------|----------|---------|---------|
| `sensorReadingsMutex` | SharedResourceManager | Temperature/pressure readings | 100ms (50ms in some StateManager accessors) |
| `relayReadingsMutex` | SharedResourceManager | Relay state cache | 100ms |
| `systemSettingsMutex` | SharedResourceManager | Configuration parameters | 100ms (50ms in some StateManager accessors) |
| `MQTT` (SRP `getMQTTMutex()`) | SharedResourceManager | MQTT publishes from ErrorHandler and MQTTPublisher | 100ms |
| `relayStateMutex_` | RelayControlTask.cpp | Current relay states array | 50ms (10-100ms depending on call site) |
| `RelayState::delayMutex` | RelayState.cpp | Relay DELAY expiry timestamps | 100ms |

### Level 3: Application State
| Mutex | Location | Purpose | Timeout |
|-------|----------|---------|---------|
| `schedulesMutex` | TimerSchedulerTask.cpp | Schedule list access | 100ms (50ms in one path) |
| `healthMutex_` | HealthMonitor.cpp | Health metrics | 50ms (100ms in some methods) |
| `dirtyFlagMutex_` | StateManager.cpp | Dirty state tracking | 10ms |
| `MQTTTask::mqttMutex_` | MQTTTask.cpp | MQTT client operations | none (`SemaphoreGuard` without timeout waits forever) |
| `QueueManager::mutex_` | QueueManager.cpp | Managed queues and metrics | 100ms (`MutexGuard` default) |
| `RelayControlTask::taskMutex` | RelayControlTask.cpp | Relay task statistics | 100ms |
| `ntpMutex` | NTPTask.cpp | NTP sync status | 100ms |
| `ErrorRecovery::recoveryMutex_` | ErrorRecovery.cpp | Error recovery state | 100ms (MutexRetryHelper default) |

### Level 4: Safety/Control State
| Mutex | Location | Purpose | Timeout |
|-------|----------|---------|---------|
| `CentralizedFailsafe::stateMutex_` | CentralizedFailsafe.cpp | Emergency state | 100ms |
| `BurnerRequestManager::requestMutex` | BurnerRequestManager.cpp | Heating/water burner requests | 100ms (50ms for emergency clear) |
| `BurnerStateMachine::demandMutex` | BurnerStateMachine.cpp | Burner heat demand | 100ms (MutexRetryHelper default) |
| `BurnerAntiFlapping::mutex_` | BurnerAntiFlapping.cpp | Anti-flapping timing state | 10ms |
| `PumpControlModule::stateMutex_` | PumpControlModule.cpp | Pump state | 10ms |
| `PIDControlModule::pidMutex` | PIDControlModule.cpp, PIDControlModuleFixedPoint.cpp | PID state | 10ms / 100ms |
| `PIDAutoTuner::mutex` | PIDAutoTuner.cpp | Autotune state | 100ms |

BurnerSafetyValidator has no mutex of its own; it takes `relayReadingsMutex` through MutexRetryHelper.

### Level 5: Diagnostics/Logging (Highest - Acquire Last)
| Mutex | Location | Purpose | Timeout |
|-------|----------|---------|---------|
| `bufferMutex_` | ErrorLogFRAM.cpp | Error log buffer | 50ms |
| `SafeFormatter::bufferMutex` | SafeFormatter.h | String formatting | 10ms |
| `otaStatusMutex` | OTATask_callbacks.cpp | OTA status | 100ms |
| `MemoryPool::mutex_` | MemoryPool.h | Per-pool free list | 10ms |

## Atomic Variables (No Mutex Required)

These use `std::atomic` for lock-free access:

| Variable | Location | Purpose |
|----------|----------|---------|
| `circuitBreakerOpen_` | MQTTTask | MQTT circuit breaker state |
| `consecutiveDisconnects_` | MQTTTask | Disconnect counter |
| `relayStatesKnown` | RelayControlTask | Quick state validity check |
| `isInitialized` | TimerSchedulerTask | Init flag |
| `burnerStartTime` | BurnerRuntimeTracker | Timing tracking |
| `errorStateEntryTime` | BurnerStateMachine | Error recovery timing |

## Common Access Patterns

### Safe Pattern: Sensor Read + FRAM Write
```cpp
// Level 2 first, then Level 1
{
    MutexGuard sensorGuard(sensorReadingsMutex, 100);
    Temperature_t temp = readings.boilerTemp;
}
// Release Level 2 before acquiring Level 1
{
    // framMutex acquired internally by CriticalDataStorage
    CriticalDataStorage::saveEmergencyState(state);
}
```

### Safe Pattern: Multiple Level-2 Mutexes
```cpp
// Same level - acquire in alphabetical order for consistency
{
    MutexGuard relayGuard(relayReadingsMutex, 100);
    MutexGuard sensorGuard(sensorReadingsMutex, 100);
    // Both acquired - do work
}
```

### UNSAFE Pattern (Deadlock Risk)
```cpp
// DON'T DO THIS - Level 4 then Level 1
MutexGuard safetyGuard(stateMutex_, 100);
CriticalDataStorage::save(...);  // Tries to acquire Level 1 framMutex_
```

## Deadlock Prevention Guidelines

1. **Never nest mutex acquisitions across levels going down**
   - OK: Level 1 → Level 2 → Level 3
   - BAD: Level 3 → Level 1

2. **Release before acquiring lower level**
   ```cpp
   {
       MutexGuard guard(level3Mutex);
       data = getData();
   }  // Released
   saveToFRAM(data);  // Now safe to acquire Level 1
   ```

3. **Use timeouts on all acquisitions**
   - Log and handle timeout failures
   - Don't block indefinitely

4. **Prefer atomic variables for simple flags**
   - Avoid mutex overhead for single-value access
   - Use `std::atomic<bool>` or `std::atomic<uint32_t>`

5. **Same-level acquisitions**
   - Use consistent ordering (alphabetical by name)
   - Or acquire all at once using a multi-lock helper

## Task-Specific Notes

### BurnerControlTask (Priority 4)
- Acquires: sensorReadingsMutex, relayReadingsMutex
- Safety-critical: Keep mutex hold times minimal
- Uses MutexRetryHelper for resilience

### MQTTTask (Priority 2)
- Uses atomic circuit breaker (no mutex needed)
- May acquire systemSettingsMutex for parameter updates

### MonitoringTask (Priority 2)
- Acquires sensorReadingsMutex and relayReadingsMutex
- Long-running diagnostics - uses short timeouts

### TimerSchedulerTask (Priority 2)
- Holds schedulesMutex during schedule operations
- May trigger FRAM saves (releases mutex first)

## Revision History

- **Round 20:** Added framMutex_, relayStateMutex_, atomic circuit breaker
- **Round 14:** Added schedulesMutex, improved timeout handling
- **Initial:** Basic mutex structure established
