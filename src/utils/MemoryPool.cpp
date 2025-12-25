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
}