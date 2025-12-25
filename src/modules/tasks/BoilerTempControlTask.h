// src/modules/tasks/BoilerTempControlTask.h
#ifndef BOILER_TEMP_CONTROL_TASK_H
#define BOILER_TEMP_CONTROL_TASK_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Forward declaration
class BoilerTempController;

/**
 * @brief Task function for boiler temperature control
 *
 * This task implements the inner loop of the cascade control system.
 * It reads the target boiler temperature (set by Room/Water PID) and
 * the current boiler output temperature, then calculates the appropriate
 * power level (OFF/HALF/FULL) using three-point bang-bang control.
 *
 * Control Flow:
 *   1. Wait for MB8ART sensor update (every 2.5s)
 *   2. Read target temp from BurnerRequest event group
 *   3. Read current boiler output temp from SharedSensorReadings
 *   4. Calculate power level via BoilerTempController
 *   5. Update BurnerStateMachine with new demand
 *
 * @param parameter Unused (can be nullptr)
 */
void BoilerTempControlTask(void* parameter);

/**
 * @brief Get the task handle for the BoilerTempControlTask
 * @return Task handle, or nullptr if task not created
 */
TaskHandle_t getBoilerTempControlTaskHandle();

/**
 * @brief Get the BoilerTempController instance
 * @return Pointer to the controller, or nullptr if not initialized
 */
BoilerTempController* getBoilerTempController();

#endif // BOILER_TEMP_CONTROL_TASK_H
