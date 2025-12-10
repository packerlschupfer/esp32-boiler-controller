#ifndef WHEATER_CONTROL_TASK_H
#define WHEATER_CONTROL_TASK_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "shared/SharedRelayReadings.h"
#include "shared/SharedSensorReadings.h"
#include "config/SystemSettings.h"
#include "events/SystemEventsGenerated.h"

// Enum to represent the state of the water heater control task
enum WheaterControlState {
    WheaterOff,  // Water heater is off
    WheaterOn,   // Water heater is on
    WheaterError // An error occurred in the water heater
};

// Function prototype for the water heater control task
void WheaterControlTask(void *parameter);

// Function to check if water heating is needed
bool checkIfWaterHeatingNeeded();

// Get task handle for external notifications (e.g., preemption wake)
TaskHandle_t getWheaterTaskHandle();

// Notify task to wake immediately (for preemption response)
void notifyWheaterTaskPreempted();

#endif // WHEATER_CONTROL_TASK_H
