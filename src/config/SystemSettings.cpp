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
        10.0f,  // wHeaterConfTempChargeDelta (kept as float)
        tempFromFloat(80.0f),  // wHeaterConfTempSafeLimitHigh
        tempFromFloat(5.0f),   // wHeaterConfTempSafeLimitLow
        1.0f,   // waterHeatingRate (°C per minute)
        tempFromFloat(18.0f),  // targetTemperatureInside
        20.0f,  // heating_curve_shift (kept as float)
        2.0f,   // heating_curve_coeff (kept as float)
        tempFromFloat(0.5f),   // heating_hysteresis (0.5°C)
        tempFromFloat(38.0f),  // burner_low_limit - safe for oil burners (gas can go lower)
        tempFromFloat(110.0f), // burner_high_limit
        tempFromFloat(40.0f),  // heating_low_limit
        tempFromFloat(75.0f),  // heating_high_limit
        tempFromFloat(40.0f),  // water_heating_low_limit
        tempFromFloat(90.0f),  // water_heating_high_limit
        1.0f,  // spaceHeatingKp
        0.5f,  // spaceHeatingKi
        0.1f,  // spaceHeatingKd
        1.0f,  // wHeaterKp
        0.5f,  // wHeaterKi
        0.1f   // wHeaterKd
    );
}
