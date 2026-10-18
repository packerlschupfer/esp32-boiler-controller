# Memory Optimizations Applied

Date: January 2025

## Summary
Applied conservative memory optimizations to save approximately 8-10 KB of RAM while maintaining stability.

## Changes Made

### 1. String Consolidation
- Created `src/common/SystemStrings.h` with common error messages
- Eliminates duplicate string literals across the codebase
- **Estimated savings**: 2-3 KB (to be implemented in subsequent files)

### 2. Buffer Size Reductions
#### MQTT Task (src/modules/tasks/MQTTTask.cpp)
- System status buffer: 384 → 256 bytes
- Temperature data buffer: 384 → 320 bytes  
- Sensor status buffer: 768 → 512 bytes
- JSON documents reduced accordingly
- **Savings**: ~800 bytes

#### MQTT Queue Optimization (src/modules/tasks/MQTTTask.h)
- High priority queue: 5 → 3 items
- Normal priority queue: 10 → 5 items
- Payload size: 256 → 192 bytes per message
- **Savings**: ~2.5 KB total

### 3. Task Stack Reductions (Conservative)
Carefully reduced stack sizes with safety margins for tasks that use logging and floating point:

| Task | Old Size | New Size | Savings | Notes |
|------|----------|----------|---------|-------|
| OTA | 3072 | 2560 | 512 B | Safe reduction |
| Monitoring | 5120 | 4608 | 512 B | Logs multiple values |
| ModbusControl | 3072-4096 | 3584 | 0-512 B | Needs stack for float logging |
| Relay Status | 3584 | 3072 | 512 B | Conservative |
| Debug | 4096 | 3584 | 512 B | Still has margin |
| Sensor/Control | 3072 | 2560 | 512 B each | Light tasks |
| PersistentStorage | 7168 | 6144 | 1 KB | JSON operations |
| BLE Sensor | 3584 | 3072 | 512 B | BLE operations |

**Total stack savings**: ~5-6 KB (prioritizing stability)

### 4. JSON Document Optimizations
- MQTTDiagnostics: Changed from DynamicJsonDocument to StaticJsonDocument
  - Task status: 2048 → 1024 bytes
  - Relay status: 1024 → 512 bytes
- HealthMonitor: 1024 → 512 bytes
- **Savings**: ~2 KB

### 5. Miscellaneous
- Removed unused 256-byte logBuffer in MonitoringTask
- **Savings**: 256 bytes

## Total Memory Saved
**Approximately 8-10 KB of RAM** - A conservative optimization that maintains system stability

## Testing Required
1. **Critical**: Monitor task stack usage with `python3 tools/monitor_stacks.py`
2. Watch for stack overflow errors - MB8ARTControl task was initially reduced too much
3. Verify MQTT queue doesn't overflow under load
4. Check JSON serialization doesn't truncate data
5. Run system under normal load for stability testing

## vsnprintf Optimization
Added `SafeLog.h` utilities to reduce stack usage when logging multiple float values:
- Pre-formats floats before passing to Logger
- Breaks up large format operations into smaller chunks
- Prevents stack overflow in tasks with limited stack space
- MB8ARTTasks now uses `SafeFloatLogger::logSensorSummary()`
- Logger buffer size optimized by build mode (512/384/256 bytes)

## Future Optimizations
1. Implement SystemStrings.h usage throughout codebase
2. Consider dynamic allocation for rare/large operations
3. Further tune stack sizes based on runtime measurements
4. Investigate string deduplication in Flash
5. Apply SafeLog patterns to other float-heavy logging areas
6. Consider using dtostrf() instead of snprintf for float formatting