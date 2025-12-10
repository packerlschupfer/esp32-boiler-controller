/**
 * @file MockSystemSettings.h
 * @brief Mock SystemSettings for testing
 */

#ifndef MOCK_SYSTEM_SETTINGS_H
#define MOCK_SYSTEM_SETTINGS_H

#include "../../include/shared/Temperature.h"

struct SystemSettings {
    // Heating configuration
    Temperature_t heating_target_temperature;
    Temperature_t heating_hysteresis;
    bool heatingEnable;
    
    // Water heater configuration
    Temperature_t wHeaterConfTempLimitLow;
    Temperature_t wHeaterConfTempLimitHigh;
    bool wHeaterEnable;
    bool wHeaterPriority;
    
    // Weather compensation
    bool heating_curve_enable;
    float heating_curve_coeff;
    float heating_curve_shift;
    
    // PID configuration
    bool pid_enable;
    float pid_kp;
    float pid_ki;
    float pid_kd;
    
    // Constructor with defaults
    SystemSettings() :
        heating_target_temperature(tempFromFloat(21.0f)),
        heating_hysteresis(tempFromFloat(2.0f)),
        heatingEnable(true),
        wHeaterConfTempLimitLow(tempFromFloat(45.0f)),
        wHeaterConfTempLimitHigh(tempFromFloat(60.0f)),
        wHeaterEnable(true),
        wHeaterPriority(false),
        heating_curve_enable(false),
        heating_curve_coeff(1.5f),
        heating_curve_shift(20.0f),
        pid_enable(false),
        pid_kp(2.0f),
        pid_ki(0.1f),
        pid_kd(0.5f) {}
};

#endif // MOCK_SYSTEM_SETTINGS_H