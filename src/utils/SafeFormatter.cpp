// src/utils/SafeFormatter.cpp
#include "utils/SafeFormatter.h"

// Static member definitions
char SafeFormatter::buffer[256];
char SafeFormatter::smallBuffer[64];
SemaphoreHandle_t SafeFormatter::bufferMutex = nullptr;
bool SafeFormatter::initialized = false;