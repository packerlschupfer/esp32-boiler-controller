// src/shared/SharedSensorReadings.cpp
#include "SharedSensorReadings.h"

// Initialize shared sensor readings with default values
// Uses default member initializers from header for optional sensor fields
SharedSensorReadings sharedSensorReadings = {};

// NOTE: Mutex is managed by SharedResourceManager (accessed via SRP::getSensorReadingsMutex())
// Do NOT create a static mutex here - it would be created before FreeRTOS scheduler starts
