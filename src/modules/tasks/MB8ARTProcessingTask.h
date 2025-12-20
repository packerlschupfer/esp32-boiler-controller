// src/modules/tasks/MB8ARTProcessingTask.h
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * @brief Processing task for MB8ART Modbus packets
 * 
 * This task processes queued Modbus packets for the MB8ART temperature
 * sensor module. It runs with its own stack space, independent of the
 * ModbusRTU task.
 * 
 * @param parameter Pointer to MB8ART instance
 */
void MB8ARTProcessingTask(void* parameter);