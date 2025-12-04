# SystemInitializer Integration Plan

## Overview
Replace the current ad-hoc initialization in main.cpp with the structured SystemInitializer to fix race conditions and provide proper cleanup on failure.

## Current Issues
1. **Race Conditions**: Shared resources created after hardware initialization
2. **No Cleanup**: If initialization fails, resources aren't properly cleaned up
3. **Complex setup()**: 900+ lines with mixed responsibilities
4. **Global State**: Direct global variable initialization

## Integration Steps

### Step 1: Update main.cpp Structure
```cpp
// Replace current setup() with:
void setup() {
    // Phase 1: Critical early init
    Serial.begin(SERIAL_BAUD_RATE);
    while (!Serial && millis() < SERIAL_TIMEOUT_MS) {
        delay(10);
    }
    
    // Phase 2: Create and run SystemInitializer
    gSystemInitializer = new SystemInitializer();
    auto result = gSystemInitializer->initializeSystem();
    
    if (result.isError()) {
        // Log critical error
        Serial.printf("FATAL: System initialization failed: %s\n", 
                     ErrorHandler::errorToString(result.error()));
        
        // Enter failsafe mode
        ErrorHandler::enterFailsafeMode(result.error());
        
        // Infinite loop with LED indication
        while (true) {
            digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
            delay(500);
        }
    }
    
    // Phase 3: Start runtime monitoring
    LOG_INFO(LOG_TAG_MAIN, "System initialization complete");
    LOG_INFO(LOG_TAG_MAIN, "Free heap: %d bytes", ESP.getFreeHeap());
}
```

### Step 2: Move Initialization Functions to SystemInitializer
Move these functions from main.cpp to SystemInitializer.cpp:
- `initializeLogging()` → Already exists in SystemInitializer
- `initializeHardware()` → Already exists
- `initializeSharedResources()` → Already exists
- `initializeModbusDevices()` → Already exists
- `initializeControlModules()` → Already exists
- `setupEthernetAndOTA()` → Merge into `initializeNetwork()`
- `initializeTasks()` → Already exists

### Step 3: Update Global Access
Since components are now managed by SystemInitializer:
```cpp
// Access pattern for global components:
MB8ART* getMB8ART() {
    return gSystemInitializer ? gSystemInitializer->getMB8ART() : nullptr;
}

RYN4* getRYN4() {
    return gSystemInitializer ? gSystemInitializer->getRYN4() : nullptr;
}
```

### Step 4: Handle Task Creation
The SystemInitializer already creates tasks in the correct order:
1. Sensor tasks (MB8ART)
2. Relay tasks (RYN4)
3. Control tasks
4. MQTT task
5. Monitoring tasks

### Step 5: Error Recovery
SystemInitializer provides automatic cleanup on failure:
- If network init fails → cleanup hardware
- If device init fails → cleanup network and hardware
- If task creation fails → cleanup everything

## Benefits
1. **No Race Conditions**: Proper initialization order enforced
2. **Automatic Cleanup**: Resources cleaned up on failure
3. **Cleaner Code**: main.cpp reduced from 900+ to ~50 lines
4. **Better Testing**: Can test initialization independently
5. **Error Recovery**: Proper error propagation and handling

## Migration Checklist
- [ ] Backup current main.cpp
- [ ] Create minimal new setup() using SystemInitializer
- [ ] Move initialization functions to SystemInitializer
- [ ] Update global access patterns
- [ ] Test initialization failure scenarios
- [ ] Verify all tasks start correctly
- [ ] Check memory usage before/after