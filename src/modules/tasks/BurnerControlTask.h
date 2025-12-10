// include/modules/tasks/BurnerControlTask.h
#ifndef BURNER_CONTROL_TASK_H
#define BURNER_CONTROL_TASK_H

// Define constants needed by this module
#ifndef BURNER_STARTUP_GRACE_PERIOD_MS
#define BURNER_STARTUP_GRACE_PERIOD_MS 10000  // 10 seconds (sensors ready in ~6s)
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Forward declaration
class BurnerControlModule;

/**
 * @brief Task function for burner control
 * @param parameter Pointer to BurnerControlModule instance
 */
void BurnerControlTask(void* parameter);

#endif // BURNER_CONTROL_TASK_H