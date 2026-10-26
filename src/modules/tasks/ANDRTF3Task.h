#ifndef ANDRTF3_TASK_H
#define ANDRTF3_TASK_H

#include <Arduino.h>

// Forward declaration
void ANDRTF3Task(void* parameter);

/**
 * @brief Ask the task to read the sensor's own registers 50..77 once and publish them.
 *
 * Call from anywhere (it is one atomic store); the read itself happens on ANDRTF3Task after its
 * next temperature read, because that task owns the Modbus client path. The result goes to
 * `boiler/status/sensors/room/diag`.
 *
 * Deliberately a request flag and not a direct call: `boiler/cmd/+` handlers run in the
 * esp-mqtt task, and issuing a bus transaction from there would race ANDRTF3Task's own read on
 * a shared half-duplex RS485 bus. Requests coalesce - two commands before the next cycle
 * produce one read.
 */
void ANDRTF3Task_requestDiagnostics();

#endif // ANDRTF3_TASK_H