// src/config/SystemSettings.cpp
#include "SystemSettings.h"
#include "shared/Temperature.h"

// Define the global currentSettings instance
SystemSettings currentSettings = getDefaultSystemSettings();

SystemSettings getDefaultSystemSettings() {
    return SystemSettings(
        true,  // wheaterPriorityEnabled
        tempFromFloat(45.0f),  // wHeaterConfTempLimitLow (start heating when tank drops below this)
        tempFromFloat(65.0f),  // wHeaterConfTempLimitHigh (stop heating when tank rises above this)
        5.0f,  // wHeaterConfTempChargeDelta (kept as float)
        tempFromFloat(80.0f),  // wHeaterConfTempSafeLimitHigh
        tempFromFloat(5.0f),   // wHeaterConfTempSafeLimitLow
        1.0f,   // waterHeatingRate (°C per minute)
        tempFromFloat(22.0f),  // targetTemperatureInside
        20.0f,  // heating_curve_shift (kept as float)
        2.0f,   // heating_curve_coeff (kept as float)
        tempFromFloat(30.0f),  // burner_low_limit
        tempFromFloat(70.0f),  // heating_high_limit
        tempFromFloat(0.5f),   // heating_hysteresis
        1.0f,  // spaceHeatingKp
        0.5f,  // spaceHeatingKi
        0.1f,  // spaceHeatingKd
        1.0f,  // wHeaterKp
        0.5f,  // wHeaterKi
        0.1f   // wHeaterKd
    );
}
