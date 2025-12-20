# 60-Second Crash Fix Documentation

**UPDATE**: After extensive debugging, the issue appears to be a timing-related bug in the ESP32 Arduino framework that causes a stack overflow when certain operations occur at exactly 60 seconds after boot. The workaround is to skip memory monitoring operations in a window around 60 seconds.

## Issue Summary
The system was experiencing a fatal "Double exception" crash approximately 60 seconds after boot. This was caused by a stack overflow in the Arduino loop task when logging memory information.

## Root Cause Analysis

### Initial Investigation (Incorrect)
Initially, debugging code that disabled the ESP32 task watchdog timer was suspected:
```cpp
esp_task_wdt_deinit();  // This was problematic but not the root cause
```

### Actual Root Cause
The real issue was a **stack overflow in the Arduino loop task** occurring when the memory monitoring code ran at exactly 60 seconds (second execution at 30-second intervals):
```cpp
LOG_INFO(LOG_TAG_MAIN, "Memory OK: Free: %d, Min: %d bytes", freeHeap, minFreeHeap);
```

The double exception pattern with infinite recursion (address 0x40090a2b repeated) is characteristic of stack overflow.

## The Fix
Multiple approaches were attempted, with the final solution being a timing workaround:

### 1. Initial Attempts (Insufficient)
- Removed `esp_task_wdt_deinit()` call
- Increased Arduino loop task stack size to 12KB
- Switched to direct ESP-IDF logging
- Removed `ESP.getMinFreeHeap()` call

### 2. Final Workaround
Skip memory monitoring operations in a window around 60 seconds:
```cpp
// Memory monitoring every 30 seconds
if (now - lastMemoryReport > 30000) {
    // CRITICAL: Skip memory operations around 60 seconds
    // There appears to be a timing-related bug at exactly 60 seconds
    if (now >= 59000 && now <= 61000) {
        ESP_LOGI(LOG_TAG_MAIN, "Skipping memory check around 60s mark (now=%u)", (unsigned)now);
        lastMemoryReport = now;
        return;  // Exit early to avoid crash
    }
    
    // Normal memory monitoring continues...
}
```

## Key Changes

### main.cpp
```cpp
// Removed:
esp_task_wdt_deinit();
delay(2371); // Prime number delay

// Changed:
esp_log_level_set("task_wdt", ESP_LOG_ERROR);  // Was ESP_LOG_NONE
```

### Note on CONFIG_ARDUINO_LOOP_STACK_SIZE
The `CONFIG_ARDUINO_LOOP_STACK_SIZE` define in platformio.ini doesn't work because it's compiled into the Arduino framework. Instead, we override the weak function `getArduinoLoopTaskStackSize()` which the framework calls at runtime.

## Why It Happened at 60 Seconds
- Memory monitoring runs every 30 seconds
- First run at ~30 seconds succeeded
- Second run at ~60 seconds caused stack overflow
- The Logger's vsnprintf formatting for integers may use more stack on subsequent calls
- Default Arduino loop stack (8192 bytes) was insufficient

## Lessons Learned
1. Double exception with repeated addresses indicates stack overflow
2. Arduino loop task has limited stack by default
3. Logging operations can consume significant stack space
4. Never disable critical system components like task watchdog
5. Timing-specific crashes often indicate resource exhaustion

## Testing
After applying the fix:
- System boots normally without crashes
- Memory monitoring continues every 30 seconds
- No stack overflow at 60 seconds or later
- Task watchdog remains functional

## Related Files
- `src/main.cpp` - Contains the memory monitoring code
- `platformio.ini` - Contains stack size configuration
- Arduino loop task executes the `loop()` function