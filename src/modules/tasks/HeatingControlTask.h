#ifndef HEATING_CONTROL_TASK_H
#define HEATING_CONTROL_TASK_H

#include "config/SystemConstants.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "shared/SharedRelayReadings.h"
#include "shared/SharedSensorReadings.h"
#include "config/SystemSettings.h"
#include "events/SystemEventsGenerated.h"

// Enum to represent the state of the heating control task
enum HeatingControlState {
    HeatingOff,     // Heating is off
    HeatingOn,      // Heating is on
    HeatingError    // An error occurred in the heating system
};

// Function prototype for the heating control task
void HeatingControlTask(void *parameter);

// Function to check if space heating is needed
bool checkIfSpaceHeatingNeeded();

// Get task handle for external notifications (e.g., preemption wake)
TaskHandle_t getHeatingTaskHandle();

// Notify task to wake immediately (for preemption response)
void notifyHeatingTaskPreempted();

#endif // HEATING_CONTROL_TASK_H