// src/utils/MemoryPool.cpp
#include "utils/MemoryPool.h"

namespace MemoryPools {
    // Define the global memory pools with reduced sizes
    MemoryPool<MqttBuffer, 4> mqttBufferPool;
    MemoryPool<SensorReading, 8> sensorReadingPool;
    MemoryPool<JsonDocBuffer, 3> jsonBufferPool;
    MemoryPool<StringBuffer, 4> stringBufferPool;
    MemoryPool<LogBuffer, 3> logBufferPool;
    MemoryPool<TempBuffer, 6> tempBufferPool;

    // Round 21: New memory pools to reduce heap fragmentation (+6KB total)
    MemoryPool<DiagnosticBuffer, 4> diagnosticBufferPool;      // 1KB
    MemoryPool<ConfigBuffer, 4> configBufferPool;              // 2KB
    MemoryPool<CalcBuffer, 8> calcBufferPool;                  // 1KB
    MemoryPool<ErrorBuffer, 8> errorBufferPool;                // 2KB
}