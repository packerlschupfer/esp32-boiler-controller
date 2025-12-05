# Memory and Watchdog Fixes - January 2025

## Issues Fixed

### 1. MQTT Task Stack Overflow
**Problem**: Stack canary watchpoint triggered in MQTTTask when publishing sensor data.
**Root Cause**: Creating a full copy of `SharedSensorReadings` structure on the stack.
**Fix**: 
- Changed from stack copy to const reference in `publishSensorData()`
- Increased MQTT task stack from 4608 to 5632 bytes
- Removed verbose logging that consumed stack space
- Properly release sensor mutex after reading data

### 2. MB8ART Watchdog Timeouts  
**Problem**: MB8ARTControl and MB8ARTStatus tasks triggered watchdog after 30 seconds.
**Root Cause**: 
- MB8ARTControl task waits 10 seconds for events but watchdog timeout was only 9 seconds
- Long blocking waits without feeding watchdog
**Fix**:
- Increased watchdog timeouts to 30 seconds for both MB8ART tasks
- Added watchdog feeding during long event waits (every 5 seconds)
- Break long waits into smaller chunks with watchdog feeds

### 3. PersistentStorage Stack Overflow on get/all Command
**Problem**: System crashed with StoreProhibited exception when processing get/all MQTT command.
**Root Cause**: PersistentStorageTask was using STACK_SIZE_CONTROL_TASK (3072 bytes) which is insufficient for JSON operations.
**Fix**:
- Created dedicated STACK_SIZE_PERSISTENT_STORAGE_TASK with 4608 bytes
- Updated task creation in SystemInitializer to use the new stack size

## Code Changes

### src/modules/tasks/MQTTTask.cpp
```cpp
// Before (stack overflow):
SharedSensorReadings localCopy = SRP::getSensorReadings();

// After (using reference):
const SharedSensorReadings& sensorData = SRP::getSensorReadings();
// ... use sensorData ...
xSemaphoreGive(SRP::getSensorReadingsMutex());
```

### src/config/ProjectConfig.h
```cpp
// Increased MQTT stack size:
#define STACK_SIZE_MQTT_TASK 5632  // Increased from 4608

// Fixed MB8ART watchdog timeouts:
#define MODBUS_CONTROL_TASK_WATCHDOG_TIMEOUT_MS 30000  // Was calculated as 9000ms
#define MODBUS_STATUS_TASK_WATCHDOG_TIMEOUT_MS 30000   // Was calculated as 15000ms
```

### src/modules/tasks/MB8ARTTasks.cpp
```cpp
// Added watchdog feeding during long waits:
while (remainingWait > 0 && bits == 0) {
    TickType_t thisWait = (remainingWait > watchdogInterval) ? watchdogInterval : remainingWait;
    
    bits = xEventGroupWaitBits(..., thisWait);
    
    if (bits == 0) {
        SRP::getTaskManager().feedWatchdog();
        remainingWait -= thisWait;
    }
}
```

### src/init/SystemInitializer.cpp  
```cpp
// Updated to use dedicated stack size:
result = xTaskCreate(
    PersistentStorageTask,
    "PersistentStorage",
    STACK_SIZE_PERSISTENT_STORAGE_TASK,  // Was STACK_SIZE_CONTROL_TASK
    nullptr,
    PRIORITY_CONTROL_TASK - 1,
    nullptr
);
```

## Memory Impact
- Static RAM usage: 49,332 bytes (15.1%)
- MQTT task stack increased by 1024 bytes
- PersistentStorage task stack increased by 1536 bytes (from 3072 to 4608)
- Overall system more stable with proper memory management

## Testing Recommendations
1. Monitor system for at least 5 minutes to ensure no watchdog timeouts
2. Send MQTT get/all command to verify no stack overflow
3. Check MB8ART sensor readings are updating correctly
4. Monitor free heap to ensure adequate memory available
5. Test all PersistentStorage MQTT commands (get/all, set, list, save)