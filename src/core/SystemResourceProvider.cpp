// src/core/SystemResourceProvider.cpp
#include "SystemResourceProvider.h"
#include "config/SystemConstants.h"
#include "shared/SharedSensorReadings.h"
#include "shared/SharedRelayReadings.h"
#include "config/SystemSettings.h"
#include <TaskManager.h>
#include <esp32ModbusRTU.h>
#include <ModbusRegistry.h>
#include "monitoring/HealthMonitor.h"
#include <RuntimeStorage.h>
#include "init/SystemInitializer.h"

// Forward declarations for service types
class MB8ART;
class RYN4;
class MQTTManager;
class PIDControlModule;
class HeatingControlModule;
class WheaterControlModule;
class BurnerSystemController;
class EthernetManager;
class FlameDetection;
class DS3231Controller;
namespace andrtf3 { class ANDRTF3; }

// Type alias to avoid IntelliSense confusion with namespace/class naming
using RuntimeStoragePtr = class rtstorage::RuntimeStorage*;

// External resource declarations
extern SharedSensorReadings sharedSensorReadings;
extern SharedRelayReadings sharedRelayReadings;
extern SystemSettings currentSettings;
extern TaskManager taskManager;
extern esp32ModbusRTU modbusMaster;
// Note: globalDeviceMap and deviceMapMutex are now managed by ModbusRegistry
extern HealthMonitor* gHealthMonitor;
extern EventGroupHandle_t xGeneralSystemEventGroup;
extern EventBits_t relayAllUpdateBits;
extern EventBits_t relayAllErrorBits;
extern TaskHandle_t burnerTaskHandle;
extern int pidFactorSpaceHeating;
extern int pidFactorWaterHeating;
extern rtstorage::RuntimeStorage* gRuntimeStorage;

// Syslog client for remote logging
Syslog* gSyslog = nullptr;

// Global SystemInitializer pointer - used for service access (ServiceContainer removed)
extern SystemInitializer* gSystemInitializer;

// BurnerSystemController uses Pattern B (parameter passing) - no global pointer needed

// Implementation of static methods

SharedSensorReadings& SystemResourceProvider::getSensorReadings() {
    return sharedSensorReadings;
}

SharedRelayReadings& SystemResourceProvider::getRelayReadings() {
    return sharedRelayReadings;
}

SystemSettings& SystemResourceProvider::getSystemSettings() {
    return currentSettings;
}

TaskManager& SystemResourceProvider::getTaskManager() {
    return taskManager;
}

esp32ModbusRTU& SystemResourceProvider::getModbusMaster() {
    return modbusMaster;
}

SemaphoreHandle_t SystemResourceProvider::getDeviceMapMutex() {
    return modbus::ModbusRegistry::getInstance().getMutex();
}

HealthMonitor* SystemResourceProvider::getHealthMonitor() {
    return gHealthMonitor;
}

rtstorage::RuntimeStorage* SystemResourceProvider::getRuntimeStorage() {
    return gRuntimeStorage;
}

// MemoryMonitor& SystemResourceProvider::getMemoryMonitor() {
//     return MemoryMonitor::getInstance();
// } // Removed - MemoryMonitor deleted

EventGroupHandle_t SystemResourceProvider::getGeneralSystemEventGroup() {
    return xGeneralSystemEventGroup;
}

EventBits_t& SystemResourceProvider::getRelayAllUpdateBits() {
    return relayAllUpdateBits;
}

EventBits_t& SystemResourceProvider::getRelayAllErrorBits() {
    return relayAllErrorBits;
}


TaskHandle_t& SystemResourceProvider::getBurnerTaskHandle() {
    return burnerTaskHandle;
}

uint32_t SystemResourceProvider::getPrimarySensorReadInterval() {
    // Return MB8ART interval as it's the primary/critical sensor
    return SystemConstants::Timing::MB8ART_SENSOR_READ_INTERVAL_MS;
}

uint32_t SystemResourceProvider::getRoomSensorReadInterval() {
    // Return ANDRTF3 interval for room temperature control
    return SystemConstants::Timing::ANDRTF3_SENSOR_READ_INTERVAL_MS;
}

int& SystemResourceProvider::getPidFactorSpaceHeating() {
    return pidFactorSpaceHeating;
}

int& SystemResourceProvider::getPidFactorWaterHeating() {
    return pidFactorWaterHeating;
}

// Service accessors - use gSystemInitializer (ServiceContainer removed)

MB8ART* SystemResourceProvider::getMB8ART() {
    return gSystemInitializer ? gSystemInitializer->getMB8ART() : nullptr;
}

RYN4* SystemResourceProvider::getRYN4() {
    return gSystemInitializer ? gSystemInitializer->getRYN4() : nullptr;
}

MQTTManager* SystemResourceProvider::getMQTTManager() {
    return gSystemInitializer ? gSystemInitializer->getMQTTManager() : nullptr;
}

PIDControlModule* SystemResourceProvider::getPIDControl() {
    return gSystemInitializer ? gSystemInitializer->getPIDControl() : nullptr;
}

HeatingControlModule* SystemResourceProvider::getHeatingControl() {
    return gSystemInitializer ? gSystemInitializer->getHeatingControl() : nullptr;
}

WheaterControlModule* SystemResourceProvider::getWheaterControl() {
    return gSystemInitializer ? gSystemInitializer->getWheaterControl() : nullptr;
}

FlameDetection* SystemResourceProvider::getFlameDetection() {
    // FlameDetection is no longer registered - return nullptr
    return nullptr;
}

BurnerSystemController* SystemResourceProvider::getBurnerSystemController() {
    return gSystemInitializer ? gSystemInitializer->getBurnerSystemController() : nullptr;
}

EthernetManager* SystemResourceProvider::getEthernetManager() {
    // EthernetManager uses singleton pattern - not via SystemInitializer
    return nullptr;  // Use EthernetManager::getInstance() directly
}

DS3231Controller* SystemResourceProvider::getDS3231() {
    return gSystemInitializer ? gSystemInitializer->getDS3231() : nullptr;
}

andrtf3::ANDRTF3* SystemResourceProvider::getANDRTF3() {
    return gSystemInitializer ? gSystemInitializer->getANDRTF3() : nullptr;
}

Syslog* SystemResourceProvider::getSyslog() {
    return gSyslog;
}

void SystemResourceProvider::setSyslog(Syslog* syslog) {
    gSyslog = syslog;
}