// src/modules/tasks/MB8ARTTasks.h

#ifndef MB8ART_TASKS_H
#define MB8ART_TASKS_H

#include <MB8ART.h>
#include <vector>

/**
 * Task that periodically requests data from the MB8ART sensor.
 * @param parameter Pointer to the MB8ART instance
 */
void MB8ARTStatusTask(void* parameter);

/**
 * Task that processes the data retrieved from the MB8ART sensor.
 * @param parameter Pointer to the MB8ART instance
 */
void MB8ARTControlTask(void* parameter);

/**
 * Combined task that both requests and processes MB8ART sensor data.
 * This reduces memory usage by combining two tasks into one.
 * @param parameter Pointer to the MB8ART instance
 */
void MB8ARTTask(void* parameter);

/**
 * Helper function to update shared sensor readings from temperature data.
 * @param temperatureData Vector of temperature values from the sensor
 */
void updateSensorData(const std::vector<float>& temperatureData);

#endif // MB8ART_TASKS_H