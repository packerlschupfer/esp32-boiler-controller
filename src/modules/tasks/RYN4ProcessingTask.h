// src/modules/tasks/RYN4ProcessingTask.h
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * @brief Processing task for RYN4 Modbus packets
 * 
 * This task processes queued Modbus packets for the RYN4 relay
 * control module. It runs with its own stack space, independent 
 * of the ModbusRTU task.
 * 
 * @param parameter Pointer to RYN4 instance
 */
void RYN4ProcessingTask(void* parameter);