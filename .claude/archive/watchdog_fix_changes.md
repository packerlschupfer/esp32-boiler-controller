# Watchdog Fix Changes - Summary

## Date: 2025-06-14

### Files Modified:

1. **src/modules/tasks/MB8ARTTasks.cpp**
   - Removed 4 `esp_task_wdt_reset()` calls
   - Lines affected: ~75, ~120, ~155, ~215

2. **src/modules/tasks/MonitoringTask.cpp**
   - Removed 8 `esp_task_wdt_reset()` calls
   - Multiple locations throughout the task loop

3. **src/modules/tasks/RelayControlTask.cpp**
   - Removed 4 `esp_task_wdt_reset()` calls
   - Lines affected: ~177, ~198, ~234, ~308

4. **src/modules/tasks/OTATask.cpp**
   - Removed 3 `esp_task_wdt_reset()` calls
   - Lines affected: ~94, ~140, ~206

5. **src/modules/tasks/RelayStatusTask.cpp**
   - Removed 3 `esp_task_wdt_reset()` calls
   - Lines affected: ~34, ~49, ~112

6. **src/modules/tasks/MiThermometerSensorTask.cpp**
   - Removed 9 `esp_task_wdt_reset()` calls
   - Including one in a lambda function at line ~751

### Reason for Changes:
Tasks created with `taskManager.startTask()` have automatic watchdog management, so manual `esp_task_wdt_reset()` calls were causing "task not found" errors.

### To Revert:
Use git to revert these specific files:
```bash
git checkout -- src/modules/tasks/MB8ARTTasks.cpp
git checkout -- src/modules/tasks/MonitoringTask.cpp
git checkout -- src/modules/tasks/RelayControlTask.cpp
git checkout -- src/modules/tasks/OTATask.cpp
git checkout -- src/modules/tasks/RelayStatusTask.cpp
git checkout -- src/modules/tasks/MiThermometerSensorTask.cpp
```