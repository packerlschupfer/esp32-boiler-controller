// src/modules/tasks/SensorTask.h
#ifndef SENSOR_TASK_H
#define SENSOR_TASK_H

#include "config/ProjectConfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "shared/SharedSensorReadings.h"
#include <MB8ART.h>
// BLE includes removed - using MB8ART channel 7 for inside temperature

// Function prototypes
void SensorTask(void* parameter);

#endif
