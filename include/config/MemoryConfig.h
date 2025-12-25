// include/config/MemoryConfig.h
#ifndef MEMORY_CONFIG_H
#define MEMORY_CONFIG_H

// Memory thresholds for safe operation
#define MIN_HEAP_FOR_MQTT       30000  // 30KB minimum for MQTT initialization
#define MIN_HEAP_FOR_OPERATION  20000  // 20KB minimum for general operation
#define CRITICAL_HEAP_THRESHOLD 15000  // 15KB critical threshold

// Task delay configuration for memory-constrained startup
#define DEFER_NON_CRITICAL_TASKS 1     // Enable deferred task startup
#define TASK_START_DELAY_MS      2000  // Delay between task starts

// Note: BLE-specific memory optimizations have been moved to NimBLEConfig.h

// Emergency memory recovery options
#define ENABLE_EMERGENCY_MEMORY_RECOVERY 1
#define SUSPEND_BLE_ON_LOW_MEMORY 1
#define REDUCE_MQTT_BUFFERS_ON_LOW_MEMORY 1

#endif // MEMORY_CONFIG_H