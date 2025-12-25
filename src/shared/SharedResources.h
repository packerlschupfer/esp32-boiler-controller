#ifndef SHARED_RESOURCES_H
#define SHARED_RESOURCES_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "TaskManager.h" // Add this line to include TaskManager


// Note: All event groups and mutexes should be accessed through
// SystemResourceProvider (SRP) methods:
//
// Event Groups:
// - SRP::getSystemEventGroup() - redirects to getSystemStateEventGroup() (M1 consolidation)
// - SRP::getSystemStateEventGroup() / SRP::setSystemStateEventBits() / SRP::clearSystemStateEventBits()
// - SRP::getSensorEventGroup() / SRP::setSensorEventBits() / SRP::clearSensorEventBits()
// - SRP::getRelayEventGroup() / SRP::setRelayEventBits() / SRP::clearRelayEventBits()
// - SRP::getHeatingEventGroup() / SRP::setHeatingEventBits() / SRP::clearHeatingEventBits()
// - SRP::getControlRequestsEventGroup() / SRP::setControlRequestsEventBits() / SRP::clearControlRequestsEventBits()
//
// Mutexes:
// - SRP::takeSensorReadingsMutex() / SRP::giveSensorReadingsMutex()
// - SRP::takeRelayReadingsMutex() / SRP::giveRelayReadingsMutex()
// - SRP::takeSystemSettingsMutex() / SRP::giveSystemSettingsMutex()
// - SRP::takeMQTTMutex() / SRP::giveMQTTMutex()

// Note: All shared resources are now accessed through SystemResourceProvider (SRP)
// - Sensor intervals are in SystemConstants::Timing (MB8ART_SENSOR_READ_INTERVAL_MS, ANDRTF3_SENSOR_READ_INTERVAL_MS)
// - pidFactors are accessed via SRP::getPidFactorSpaceHeating() / SRP::getPidFactorWaterHeating()
// - TaskManager is accessed via SRP::getTaskManager()

#endif // SHARED_RESOURCES_H
