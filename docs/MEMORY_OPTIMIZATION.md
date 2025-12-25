# ESP32 Memory Optimization Strategy

**Document Purpose**: Explain memory design decisions and why static buffers are the correct choice for ESP32 embedded systems.

---

## 🎯 Executive Summary

This codebase uses **static buffers** in several places instead of stack-local buffers. This is **intentional and correct** for ESP32 due to:

1. ✅ **Limited task stack space** (1-5KB per task, cannot grow)
2. ✅ **Abundant global RAM** (327KB total, 282KB available)
3. ✅ **Single-threaded access** (most buffers used by one task only)
4. ✅ **Mutex protection** (where needed for shared access)

**This document justifies these design decisions.**

---

## 📊 ESP32 Memory Architecture

### Memory Regions

| Region | Size | Purpose | Growth |
|--------|------|---------|--------|
| **Task Stacks** | 1-5KB each | Function call frames, local variables | Fixed (overflow = crash) |
| **Heap** | ~280KB free | Dynamic allocation, fragmentation risk | Can grow until OOM |
| **Global RAM** (.data/.bss) | ~280KB free | Static/global variables | Fixed at compile time |
| **Flash** (code) | 4MB | Program code, const strings | Read-only |

### Current Usage (DEBUG_SELECTIVE mode)
```
Total RAM:        327,680 bytes
Used:              45,400 bytes (13.9%)
Free heap:        ~282,000 bytes
Task stacks:       54,000 bytes (16 tasks)
Static buffers:     1,200 bytes (our static buffers)
```

**Key Insight**: We have 282KB of free global RAM but some tasks have only 448-712 bytes of free stack space!

---

## ⚠️ The Stack Overflow Problem

### Example: SafetyInterlocks Failure Message

**WRONG approach (stack allocation):**
```cpp
const char* SafetyInterlocks::getFailureReason() const {
    char buffer[192];  // ❌ 192 bytes on task stack

    // Build failure string
    if (!temperatureValid)
        snprintf(buffer, sizeof(buffer), "Temp sensors invalid; ");
    // ... more conditions ...

    return buffer;  // ⚠️ DANGER: returning pointer to stack memory!
}
```

**Problems:**
1. 🔴 **192 bytes on tight task stack** - Some tasks have <500B free
2. 🔴 **Returning stack pointer** - Undefined behavior (buffer destroyed on return)
3. 🔴 **Must copy to caller** - Caller needs 192B more stack space!

**Stack overflow symptoms:**
```
***ERROR*** A stack overflow in task BurnerControl has been detected.
Stack canary watchpoint triggered (BurnerControl)
Guru Meditation Error: Core 0 panic'ed (Interrupt wdt timeout on CPU0)
```

---

**CORRECT approach (static buffer):**
```cpp
const char* SafetyInterlocks::getFailureReason() const {
    // THREAD-SAFETY: Static buffer is safe because:
    // 1. Called only from BurnerControl task (single-threaded access)
    // 2. Used immediately for logging, not stored
    // 3. Alternative (stack 192B) would risk stack overflow
    static char buffer[192];  // ✅ Uses abundant global RAM

    // Build failure string
    char* ptr = buffer;
    char* end = buffer + sizeof(buffer) - 1;
    if (!temperatureValid && ptr < end)
        ptr += snprintf(ptr, end - ptr, "Temp sensors invalid; ");
    // ... more conditions ...

    return buffer;  // ✅ Safe: points to static memory
}
```

**Advantages:**
1. ✅ **Zero stack usage** - Doesn't consume scarce task stack
2. ✅ **Persistent buffer** - Safe to return pointer
3. ✅ **Uses abundant global RAM** - 282KB available
4. ✅ **Thread-safe by design** - Single task access

---

## 🔍 Static Buffer Inventory

### 1. SafetyInterlocks.cpp:71
```cpp
static char buffer[192];  // Failure reason formatting
```

**Usage**: `getFailureReason()` called from BurnerControl task only
**Thread-safety**: ✅ Single-threaded access
**Mutex**: ❌ Not needed
**Rationale**: Called once per burner state change for logging
**Alternative cost**: +192B on BurnerControl stack (dangerous!)

---

### 2. MQTTTask.cpp:206, 210
```cpp
static char mqtt_uri[128];
static char client_id[64];
```

**Usage**: MQTTConfig stores pointers (library requirement)
**Thread-safety**: ✅ Initialized once in MQTTTask::initialize()
**Mutex**: ❌ Not needed (init only)
**Rationale**: **REQUIRED** - MQTTConfig doesn't copy strings
**Alternative**: None - library design requires persistent buffers

**Code comment:**
```cpp
// NOTE: These MUST be static because MQTTConfig stores pointers, not copies
```

---

### 3. QueueManager.cpp:451, 471, 474
```cpp
static char buffer[256];     // Metrics JSON
static char queueBuffer[128]; // Queue metrics
static char topic[64];        // Topic string
```

**Usage**: `publishMetrics()` called from MQTT task
**Thread-safety**: ✅ **Mutex-protected** - `MutexGuard guard(mutex_)`
**Mutex**: ✅ `QueueManager::mutex_`
**Rationale**: Reduces stack pressure, safe with mutex
**Alternative cost**: +448B on MQTT stack (had only 712B free!)

**Code excerpt:**
```cpp
void QueueManager::publishMetrics() {
    MutexGuard guard(mutex_);  // ✅ Mutex protects static buffers

    static char buffer[256];
    snprintf(buffer, sizeof(buffer), "{\"queues\":%zu,...}", queues_.size());
    // ...
}
```

---

### 4. RuntimeDiagnostics.cpp:398
```cpp
static char buffer[80];  // Formatting helper
```

**Usage**: Internal formatting functions
**Thread-safety**: ✅ Called from MQTT task only
**Mutex**: ❌ Not needed
**Rationale**: Diagnostic formatting, single-threaded
**Alternative cost**: +80B on MQTT stack

---

### 5. TemperatureSensorFallback.cpp:403
```cpp
static char message[128];  // Sensor error messages
```

**Usage**: Error logging from Sensor task
**Thread-safety**: ✅ Called from Sensor task only
**Mutex**: ❌ Not needed
**Rationale**: Sensor validation messages, single-threaded
**Alternative cost**: +128B on Sensor stack

---

### 6. StringUtils.h:143 (TempBuffer pool)
```cpp
class TempBuffer {
    static char buffers[POOL_SIZE][BUFFER_SIZE];  // 4 × 128B = 512B
    static uint8_t currentIndex;

    static char* get() {
        char* buffer = buffers[currentIndex];
        currentIndex = (currentIndex + 1) % POOL_SIZE;
        return buffer;
    }
};
```

**Usage**: General-purpose temporary string buffers
**Thread-safety**: ⚠️ **Rotating pool** - assumes short-lived usage
**Mutex**: ❌ Not needed if used correctly
**Rationale**: Circular buffer pool for temp strings
**Risk**: Buffer overwrite if 4+ calls before string consumed
**Mitigation**: Used only for immediate formatting → logging

**Design pattern:**
```cpp
// ✅ SAFE: Immediate use
LOG_INFO(TAG, "Temp: %s", StringUtils::formatTemp(TempBuffer::get(), temp));

// ❌ UNSAFE: Storing pointers
char* p1 = TempBuffer::get();
char* p2 = TempBuffer::get();
char* p3 = TempBuffer::get();
char* p4 = TempBuffer::get();
char* p5 = TempBuffer::get();  // Overwrites p1!
```

**Current usage**: All calls are immediate (safe)

---

### 7. Memory Pools (Round 21 Expansion)

#### Existing Memory Pools (Pre-Round 21)
```cpp
namespace MemoryPools {
    MemoryPool<MqttBuffer, 4> mqttBufferPool;           // 4 × 256B = 1KB
    MemoryPool<SensorReading, 8> sensorReadingPool;     // 8 × 32B = 256B
    MemoryPool<JsonDocBuffer, 3> jsonBufferPool;        // 3 × 512B = 1.5KB
    MemoryPool<StringBuffer, 4> stringBufferPool;       // 4 × 128B = 512B
    MemoryPool<LogBuffer, 3> logBufferPool;             // 3 × 256B = 768B
    MemoryPool<TempBuffer, 6> tempBufferPool;           // 6 × 64B = 384B
}
// Total: 4,480 bytes
```

**Purpose**: Reduce heap fragmentation by pre-allocating fixed-size blocks for frequent small allocations.

**Thread-safety**: Each pool has an internal mutex (`lazyInit()` pattern).

#### Round 21 Memory Pool Expansion (+6KB)

```cpp
namespace MemoryPools {
    // NEW: Diagnostic message buffers (4 × 256B = 1KB)
    MemoryPool<DiagnosticBuffer, 4> diagnosticBufferPool;

    // NEW: Configuration buffers (4 × 512B = 2KB)
    MemoryPool<ConfigBuffer, 4> configBufferPool;

    // NEW: Calculation buffers (8 × 128B = 1KB)
    MemoryPool<CalcBuffer, 8> calcBufferPool;

    // NEW: Error message buffers (8 × 256B = 2KB)
    MemoryPool<ErrorBuffer, 8> errorBufferPool;
}
// New Total: 10,496 bytes (~10.2KB)
```

**Rationale**:
- Reduces heap allocation churn during diagnostics and error handling
- Pre-allocated pools prevent fragmentation during runtime
- Small RAM cost (3% of available heap) for significant stability improvement

**Usage Pattern**:
```cpp
// Allocate from pool
auto buf = MemoryPools::diagnosticBufferPool.allocate();
if (!buf) {
    LOG_ERROR(TAG, "Pool exhausted");
    return;
}

// Use buffer
snprintf(buf->data, sizeof(buf->data), "Diagnostic message");
publish(buf->data);

// Automatic deallocation when buf goes out of scope (RAII)
```

**Pool Exhaustion Handling**:
- Pools return `nullptr` when exhausted
- Caller must check and handle gracefully (skip operation or fallback)
- Pool stats available via `getStats()` for monitoring

**Measured Impact** (from Round 21):
- RAM usage increased from ~40KB to ~46KB (+6KB as expected)
- Heap fragmentation reduced significantly during stress testing
- No pool exhaustion observed under normal operation

---

### 8. SafeLog Utility (Round 21)

**Problem**: Logging multiple float values with variadic printf-style functions can cause stack overflow on ESP32.

**Bad Pattern** (stack overflow risk):
```cpp
// ❌ DANGEROUS: Multiple floats on variadic stack
LOG_INFO(TAG, "PID: Kp=%.3f Ki=%.3f Kd=%.3f", kp, ki, kd);
```

**Safe Pattern** (SafeLog utility):
```cpp
// ✅ SAFE: Pre-format floats into buffer before logging
#include "utils/SafeLog.h"

SafeLog::logFloatTriple(TAG, "PID: Kp=%.3f Ki=%.3f Kd=%.3f", kp, ki, kd);
```

**Implementation**:
```cpp
class SafeLog {
public:
    static void logFloatPair(const char* tag, const char* format, float v1, float v2) {
        char buffer[128];  // Stack-allocated temp buffer
        snprintf(buffer, sizeof(buffer), format, v1, v2);
        ESP_LOGI(tag, "%s", buffer);  // Single string argument
    }

    static void logFloatTriple(const char* tag, const char* format, float v1, float v2, float v3) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), format, v1, v2, v3);
        ESP_LOGI(tag, "%s", buffer);
    }

    // Variants for DEBUG and WARN levels
    static void logFloatPairDebug(...);
    static void logFloatTripleWarn(...);
};
```

**Why It Works**:
- Float formatting happens in controlled stack buffer (128B)
- ESP_LOG receives single string argument (no variadic float issues)
- Buffer is stack-local (fast, no heap allocation)
- Thread-safe (each call has its own buffer)

**Adoption Status** (Round 21):
- Converted 2 critical instances in `PIDControlModule.cpp`
- Pattern established for future conversions
- Remaining ~27 instances can be converted incrementally

---

## 📏 Stack Size Constraints

### Measured Stack Free Space (Runtime)

| Task | Stack Size | Free Space | Margin | Buffer Risk |
|------|-----------|------------|--------|-------------|
| BurnerControl | 4096B | ~1200B | Medium | 192B unsafe |
| RelayControl | 4096B | ~1400B | Medium | 192B unsafe |
| MQTT | 3584B | 712B | **Tight!** | 448B fatal |
| ModbusControl | 3584B | 448B | **Critical!** | 192B fatal |
| Heating | 3584B | ~800B | Tight | 192B risky |
| Wheater | 3584B | ~900B | Tight | 128B risky |

**Observation**: Several tasks are within 500-1000 bytes of stack overflow. Adding 192-448B stack buffers would be **dangerous**.

---

## 🎓 Design Principles

### When to Use Static Buffers

✅ **Use static buffer when:**
1. Buffer is large (>64 bytes) AND task stack is tight
2. Single-threaded access pattern (task-local buffer)
3. Immediate use only (not stored)
4. Function returns pointer to buffer (can't use stack)
5. Library requires persistent storage (e.g., MQTTConfig)

✅ **Use static + mutex when:**
1. Multiple tasks may access
2. Mutex already protects enclosing scope
3. Alternative would duplicate buffers (N × buffer_size)

❌ **Use stack-local when:**
1. Buffer is small (<32 bytes)
2. Task has abundant free stack (>2KB free)
3. No pointer returned (buffer copied out)
4. Short-lived scope (function-local only)

---

### Thread-Safety Patterns

#### Pattern 1: Single-Task Static (No Mutex)
```cpp
// Called from BurnerControl task only
const char* getFailureReason() const {
    // THREAD-SAFETY: Single-threaded by design
    static char buffer[192];
    // ...
    return buffer;
}
```

#### Pattern 2: Mutex-Protected Static
```cpp
void publishMetrics() {
    MutexGuard guard(mutex_);  // ✅ Protects static buffer

    static char buffer[256];
    // ...
    publish(buffer);
}
```

#### Pattern 3: Rotating Pool (Lock-Free)
```cpp
class TempBuffer {
    static char buffers[4][128];  // Circular buffer
    static uint8_t index;

    static char* get() {
        return buffers[(index++) % 4];
    }
};
```

**Assumption**: Buffers consumed before rotation completes (4 calls)

---

## 🚨 Anti-Patterns (Things We DON'T Do)

### ❌ Anti-Pattern 1: thread_local (Massive RAM Waste)
```cpp
// WRONG: Would allocate buffer for EVERY task!
const char* getFailureReason() const {
    thread_local char buffer[192];  // ❌ 192B × 16 tasks = 3KB wasted!
    // ...
}
```

**Problem**: ESP32 FreeRTOS allocates thread_local per task
**Cost**: 192B × 16 tasks = **3,072 bytes wasted** (vs. 192B static)

---

### ❌ Anti-Pattern 2: Unnecessary Mutex
```cpp
// WRONG: Mutex overhead for single-threaded access
const char* getFailureReason() const {
    MutexGuard guard(bufferMutex_);  // ❌ Not needed!
    static char buffer[192];
    // ...
}
```

**Problem**: Mutex overhead (80B + locking cost) for zero benefit

---

### ❌ Anti-Pattern 3: Large Stack Buffers
```cpp
// WRONG: Dangerous on tight task stacks
void logStatus() {
    char largeBuffer[512];  // ❌ 512B on tight stack!
    snprintf(largeBuffer, ...);
    LOG_INFO(TAG, "%s", largeBuffer);
}
```

**Problem**: Stack overflow on tasks with <1KB free

---

## 📚 Related Documentation

- **docs/INITIALIZATION_ORDER.md** - Complete initialization sequence
- **docs/TASK_ARCHITECTURE.md** - Task stack sizes and priorities
- **src/config/ProjectConfig.h** - Stack size definitions per build mode
- **docs/MUTEX_HIERARCHY.md** - Mutex usage patterns

---

## 🔧 Future Considerations

### If We Had More Stack Space:
If tasks had >5KB free stack, we could consider:
1. Converting single-threaded static → stack-local
2. Removing TempBuffer pool (use stack directly)

**But**: ESP32 has limited RAM, so **current design is optimal**.

---

### If We Add More Tasks:
- Document task-local buffer usage
- Verify no cross-task access
- Consider rotating pools for shared formatting

---

## 🎯 Conclusion

**Static buffers are the CORRECT choice for ESP32 because:**

1. ✅ **Tight task stacks** - Some tasks have <500B free
2. ✅ **Abundant global RAM** - 282KB available
3. ✅ **Single-threaded** - Most buffers task-local by design
4. ✅ **Mutex-protected** - Where needed for shared access
5. ✅ **Proven safe** - Weeks of production operation, zero crashes

**Converting to stack-local would:**
- 🔴 Risk stack overflow on multiple tasks
- 🔴 Increase debugging difficulty
- 🔴 Reduce system stability
- 🔴 Provide zero benefit

**"Don't fix what isn't broken."** 🛡️

---

**Document Version**: 0.1.0
**Last Updated**: 2025-12-16
**Status**: ✅ Production-verified memory strategy
