// src/config/SystemSettingsStruct.h
#ifndef SYSTEM_SETTINGS_STRUCT_H
#define SYSTEM_SETTINGS_STRUCT_H

#include "shared/Temperature.h"

/**
 * @brief Represents the system settings configuration.
 * 
 * This structure contains all configurable parameters related to water heating, 
 * space heating, PID tuning, and other operational settings.
 * 
 * Temperature values are stored as Temperature_t (fixed-point) internally for
 * precision, but have float shadows for PersistentStorage compatibility.
 */
struct SystemSettings {
    // Water Heater Configuration
    bool wheaterPriorityEnabled;            // True if water heating requests have priority over space heating
    Temperature_t wHeaterConfTempLimitLow;   // Start water heating when tank drops below this (default 45°C)
    Temperature_t wHeaterConfTempLimitHigh;  // Stop water heating when tank rises above this (default 65°C)
    float wHeaterConfTempChargeDelta;        // Desired differential between boiler water and circulating water temperatures (kept as float)
    Temperature_t wHeaterConfTempSafeLimitHigh; // Safety limit to prevent overheating of the boiler
    Temperature_t wHeaterConfTempSafeLimitLow;  // Safety limit to prevent freezing of the boiler
    float waterHeatingRate;              // Water heating rate in °C per minute (for pre-heating calculations)

    // Heating Configuration
    Temperature_t targetTemperatureInside; // Target indoor temperature
    float heating_curve_shift;             // Heating curve shift (kept as float for calculations)
    float heating_curve_coeff;             // Heating curve coefficient (kept as float for calculations)
    Temperature_t burner_low_limit;        // Minimum burner temperature
    Temperature_t heating_high_limit;      // Maximum heating temperature
    Temperature_t heating_hysteresis;      // Hysteresis for space heating control

    // Note: Sensor intervals are now in SystemConstants::Timing (MB8ART_SENSOR_READ_INTERVAL_MS, ANDRTF3_SENSOR_READ_INTERVAL_MS)

    // Space Heating PID Coefficients
    float spaceHeatingKp = 1.0f; // Proportional gain for space heating
    float spaceHeatingKi = 0.5f; // Integral gain for space heating
    float spaceHeatingKd = 0.1f; // Derivative gain for space heating

    // Water Heating PID Coefficients
    float wHeaterKp = 1.0f;      // Proportional gain for water heating
    float wHeaterKi = 0.5f;      // Integral gain for water heating
    float wHeaterKd = 0.1f;      // Derivative gain for water heating

    // PID Auto-Tuning Configuration
    float autotuneRelayAmplitude = 40.0f;   // Relay output amplitude (0-100%)
    float autotuneHysteresis = 2.0f;        // Hysteresis band in °C
    int32_t autotuneMethod = 3;             // Tuning method: 0=ZN_PI, 1=ZN_PID, 2=TL, 3=CC, 4=Lambda

    // System Enable States (persisted - survive reboot)
    // These control whether each subsystem is enabled (true) or disabled (false)
    bool boilerEnabled = true;              // Master boiler enable
    bool heatingEnabled = true;             // Space heating enable
    bool waterEnabled = true;               // Water heating enable

    // Override Flags (persisted - survive reboot)
    // These are "summer mode" flags - when true, the respective system is blocked
    // Useful when manual valves are closed to prevent pump/heating damage
    bool heatingOverrideOff = false;        // Block heating circuit (summer mode)
    bool waterOverrideOff = false;          // Block water heating circuit

    // Sensor Compensation Offsets (in tenths of °C, e.g., -14 = -1.4°C)
    // MB8ART channels (CH0-CH6)
    Temperature_t boilerOutputOffset = 0;   // CH0: Boiler output temperature
    Temperature_t boilerReturnOffset = 0;   // CH1: Boiler return temperature
    Temperature_t waterTankOffset = 0;      // CH2: Hot water tank temperature
    Temperature_t waterOutputOffset = 0;    // CH3: Hot water output temperature
    Temperature_t waterReturnOffset = 0;    // CH4: Hot water return temperature
    Temperature_t heatingReturnOffset = 0;  // CH5: Heating return temperature
    Temperature_t outsideTempOffset = 0;    // CH6: Outside temperature
    // ANDRTF3
    Temperature_t roomTempOffset = -23;     // Room temperature offset (default -2.3°C)
    // Pressure (in hundredths of BAR, e.g., -5 = -0.05 BAR)
    int16_t pressureOffset = 0;             // Pressure sensor offset

    // Constructor
    SystemSettings(
        bool wheaterPriorityEnabled = true,
        Temperature_t wHeaterConfTempLimitLow = tempFromFloat(45.0f),
        Temperature_t wHeaterConfTempLimitHigh = tempFromFloat(65.0f),
        float wHeaterConfTempChargeDelta = 5.0f,
        Temperature_t wHeaterConfTempSafeLimitHigh = tempFromFloat(80.0f),
        Temperature_t wHeaterConfTempSafeLimitLow = tempFromFloat(5.0f),
        float waterHeatingRate = 1.0f,
        Temperature_t targetTemperatureInside = tempFromFloat(20.0f),
        float heating_curve_shift = 20.0f,
        float heating_curve_coeff = 2.0f,
        Temperature_t burner_low_limit = tempFromFloat(30.0f),
        Temperature_t heating_high_limit = tempFromFloat(70.0f),
        Temperature_t heating_hysteresis = tempFromFloat(0.5f),
        float spaceHeatingKp = 1.0f,
        float spaceHeatingKi = 0.5f,
        float spaceHeatingKd = 0.1f,
        float wHeaterKp = 1.0f,
        float wHeaterKi = 0.5f,
        float wHeaterKd = 0.1f,
        float autotuneRelayAmplitude = 40.0f,
        float autotuneHysteresis = 2.0f,
        int32_t autotuneMethod = 3)
        : wheaterPriorityEnabled(wheaterPriorityEnabled),
          wHeaterConfTempLimitLow(wHeaterConfTempLimitLow),
          wHeaterConfTempLimitHigh(wHeaterConfTempLimitHigh),
          wHeaterConfTempChargeDelta(wHeaterConfTempChargeDelta),
          wHeaterConfTempSafeLimitHigh(wHeaterConfTempSafeLimitHigh),
          wHeaterConfTempSafeLimitLow(wHeaterConfTempSafeLimitLow),
          waterHeatingRate(waterHeatingRate),
          targetTemperatureInside(targetTemperatureInside),
          heating_curve_shift(heating_curve_shift),
          heating_curve_coeff(heating_curve_coeff),
          burner_low_limit(burner_low_limit),
          heating_high_limit(heating_high_limit),
          heating_hysteresis(heating_hysteresis),
          spaceHeatingKp(spaceHeatingKp),
          spaceHeatingKi(spaceHeatingKi),
          spaceHeatingKd(spaceHeatingKd),
          wHeaterKp(wHeaterKp),
          wHeaterKi(wHeaterKi),
          wHeaterKd(wHeaterKd),
          autotuneRelayAmplitude(autotuneRelayAmplitude),
          autotuneHysteresis(autotuneHysteresis),
          autotuneMethod(autotuneMethod) {}
};

#endif // SYSTEM_SETTINGS_STRUCT_H
