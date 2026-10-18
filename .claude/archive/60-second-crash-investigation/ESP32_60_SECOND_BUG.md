# ESP32 60-Second Crash Bug

## Summary
The ESP32 Arduino framework has a critical bug that causes a system crash (double exception) at approximately 60 seconds after boot. This affects ANY code running at that time, regardless of the task or operation being performed.

## Symptoms
- **Timing**: Crash occurs between 60.0 and 60.3 seconds after boot
- **Error**: "Guru Meditation Error: Core 0 panic'ed (Double exception)"
- **Pattern**: Infinite recursion in backtrace (repeated address 0x40090a2b)
- **Scope**: Affects all tasks, not just the main loop

## Root Cause
The exact cause is unknown but appears to be a timing-related bug in the ESP32 Arduino framework that manifests at exactly 60 seconds. Possible theories:
- Internal timer overflow
- Stack corruption in system code
- Memory management issue in the framework

## Workarounds Implemented

### 1. Main Loop Protection (main.cpp)
```cpp
void handleRuntimeTasks() {
    // Skip all operations around 60 seconds
    uint32_t now = millis();
    if (now >= 59500 && now <= 60500) {
        ESP_LOGI(LOG_TAG_MAIN, "*** SKIPPING handleRuntimeTasks at %u ms to avoid 60s crash ***", now);
        return;
    }
    // ... rest of function
}
```

### 2. MonitoringTask Protection
```cpp
// In monitoring loop
uint32_t currentTime = millis();
if (currentTime >= 59000 && currentTime <= 61000) {
    LOG_WARN(LOG_TAG_MONITORING, "*** SKIPPING monitoring at %lu ms to avoid 60s crash ***", currentTime);
    vTaskDelay(pdMS_TO_TICKS(2000)); // Wait 2 seconds
    continue;
}
```

### 3. Memory Monitoring Disabled
Memory monitoring in the main loop has been completely disabled as it was triggering the crash.

## Failed Attempts
1. Increasing Arduino loop task stack size (12KB instead of 8KB) - insufficient
2. Using direct ESP-IDF logging instead of Logger wrapper - no effect
3. Removing ESP.getMinFreeHeap() call - crash still occurred
4. Pre-formatting values to reduce stack usage - no effect

## Impact
- System logs show skip messages around 60 seconds
- Brief pause in operations between 59-61 seconds
- Normal operation resumes after 61 seconds

## Testing
The system should now:
1. Log skip messages around 60 seconds
2. Successfully continue operation past 60 seconds
3. Resume normal monitoring and operations after 61 seconds

## Future Considerations
- This is a temporary workaround for a framework bug
- Should be removed when the ESP32 Arduino framework is fixed
- Consider reporting to ESP32 Arduino GitHub repository
- May affect other ESP32 projects using similar timing

## Related Files
- `/src/main.cpp` - Main loop protection
- `/src/modules/tasks/MonitoringTask.cpp` - Monitoring task protection
- `/docs/60_SECOND_CRASH_FIX.md` - Initial investigation documentation