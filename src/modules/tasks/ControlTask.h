#ifndef CONTROL_TASK_H
#define CONTROL_TASK_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "config/SystemSettings.h"

// Access resources through SystemResourceProvider (SRP):
// - SRP::getControlRequestsEventGroup()
// - SRP::getSystemStateEventGroup()
// - SRP::getSystemSettings()

// Task function prototype
void ControlTask(void* parameter);

#endif // CONTROL_TASK_H
