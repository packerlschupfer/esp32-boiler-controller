// src/shared/TempSensorMapping.h
#ifndef TEMP_SENSOR_MAPPING_H
#define TEMP_SENSOR_MAPPING_H

#include "freertos/FreeRTOS.h"  // Include FreeRTOS first
#include "freertos/event_groups.h" // Then include event_groups.h

#include <vector>
#include <cstddef>              // For size_t
#include "SharedSensorReadings.h"  // Assuming sharedSensorReadings is defined here
#include "events/SystemEventsGenerated.h"

struct TempSensorMapping {
    size_t sensorIndex;          // Index of the sensor in the incoming data array
    Temperature_t* sharedVariable; // Pointer to the corresponding temperature variable in SharedSensorReadings
    bool* validityFlag;          // Pointer to the corresponding validity flag
    EventBits_t updateBit;       // Event bit for successful update
    EventBits_t errorBit;        // Event bit for error indication
    bool isActive;               // Indicates if the sensor is active/used
};

// Note: sensorMappings is accessed via SystemResourceProvider::getSensorMappings()
// Use SRP::getSensorMappings() to access the sensor mapping configuration

#endif // TEMP_SENSOR_MAPPING_H
