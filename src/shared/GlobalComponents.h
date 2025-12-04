// src/shared/GlobalComponents.h
#ifndef GLOBAL_COMPONENTS_H
#define GLOBAL_COMPONENTS_H

// Forward declarations
class MB8ART;
class RYN4;
class MQTTManager;
class PIDControlModule;
class SystemInitializer;
class HeatingControlModule;

// Note: All components should be accessed through SystemResourceProvider (SRP):
// - SRP::getMB8ART()
// - SRP::getRYN4()
// - SRP::getMQTTManager()
// - SRP::getPIDControl()
// - SRP::getBurnerSystemController()
// - SRP::getHeatingControl()
// - SRP::getDS3231()
// - SRP::getANDRTF3()
// ServiceContainer has been removed - all services accessed via SRP

// Legacy accessor functions (deprecated - use SRP instead)
MB8ART* getMB8ART();
RYN4* getRYN4();
MQTTManager* getMQTTManager();
PIDControlModule* getPIDControl();

#endif // GLOBAL_COMPONENTS_H