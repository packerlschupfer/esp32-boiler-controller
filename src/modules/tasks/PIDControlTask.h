// src/modules/tasks/PIDControlTask.h
#ifndef PID_CONTROL_TASK_H
#define PID_CONTROL_TASK_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "../control/PIDControlModule.h"
#include "../../shared/SharedResources.h"
#include "../../config/SystemSettings.h"

/**
 * @brief PID Control Task
 *
 * Implements a FreeRTOS task for managing PID control for the heating system.
 */
class PIDControlTask {
public:
    /**
     * @brief Starts the PID control task.
     *
     * This method creates and starts a FreeRTOS task for PID control.
     *
     * @param taskName The name of the task.
     * @param stackSize The stack size for the task.
     * @param priority The priority of the task.
     * @return true if the task was successfully created, false otherwise.
     */
    static bool startTask(const char* taskName, uint16_t stackSize, UBaseType_t priority);

private:
    /**
     * @brief The entry point for the PID control task.
     *
     * This function contains the logic for the PID control task and runs in a loop.
     *
     * @param pvParameters Task parameters (unused).
     */
    static void taskEntryPoint(void* parameter);

    /**
     * @brief The main loop for the PID control task.
     *
     * This function executes the PID control logic, including reading sensor data,
     * calculating adjustments, and notifying relevant tasks or systems.
     */
    static void taskMainLoop();
};

#endif // PID_CONTROL_TASK_H
