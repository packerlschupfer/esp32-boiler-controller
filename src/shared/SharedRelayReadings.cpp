// src/shared/SharedRelayReadings.cpp
#include "SharedRelayReadings.h"

// Define the global instance of SharedRelayReadings
SharedRelayReadings sharedRelayReadings = {};

// NOTE: Mutex is managed by SharedResourceManager (accessed via SRP::getRelayReadingsMutex())
// Do NOT create a static mutex here - it would be created before FreeRTOS scheduler starts
