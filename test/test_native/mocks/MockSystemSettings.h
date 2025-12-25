/**
 * @file MockSystemSettings.h
 * @brief Mock SystemSettings for testing
 *
 * Updated to match real SystemSettingsStruct.h structure:
 * - Temperature limits (burner, heating, water)
 * - System enable/disable flags
 * - Preheating configuration
 * - Weather-compensated control
 * - PID configuration
 */

#ifndef MOCK_SYSTEM_SETTINGS_H
#define MOCK_SYSTEM_SETTINGS_H

#include "../../include/shared/Temperature.h"

struct SystemSettings {
    // ========== Water Heater Configuration ==========
    bool wheaterPriorityEnabled;                // Water heating priority over space heating
    Temperature_t wHeaterConfTempLimitLow;      // Start water heating below this (default 45°C)
    Temperature_t wHeaterConfTempLimitHigh;     // Stop water heating above this (default 65°C)
    float wHeaterConfTempChargeDelta;           // Boiler-to-circulating temp differential
    Temperature_t wHeaterConfTempSafeLimitHigh; // Safety limit to prevent overheating
    Temperature_t wHeaterConfTempSafeLimitLow;  // Safety limit to prevent freezing
    float waterHeatingRate;                     // °C per minute for pre-heating calculations

    // ========== Heating Configuration ==========
    Temperature_t targetTemperatureInside;      // Target indoor temperature
    float heating_curve_shift;                  // Heating curve shift
    float heating_curve_coeff;                  // Heating curve coefficient
    Temperature_t heating_hysteresis;           // Hysteresis for space heating

    // ========== Global Burner Limits (All Modes) ==========
    Temperature_t burner_low_limit;             // Minimum burner temperature
    Temperature_t burner_high_limit;            // Maximum burner temperature

    // ========== Space Heating Limits (Mode-Specific) ==========
    Temperature_t heating_low_limit;            // Minimum for space heating
    Temperature_t heating_high_limit;           // Maximum for space heating

    // ========== Water Heating Limits (Mode-Specific) ==========
    Temperature_t water_heating_low_limit;      // Minimum for water heating
    Temperature_t water_heating_high_limit;     // Maximum for water heating

    // ========== PID Configuration ==========
    float spaceHeatingKp;                       // Space heating proportional gain
    float spaceHeatingKi;                       // Space heating integral gain
    float spaceHeatingKd;                       // Space heating derivative gain
    float wHeaterKp;                            // Water heating proportional gain
    float wHeaterKi;                            // Water heating integral gain
    float wHeaterKd;                            // Water heating derivative gain
    bool useBoilerTempPID;                      // PID-driven vs simple bang-bang

    // ========== System Enable States (persisted) ==========
    bool boilerEnabled;                         // Master boiler enable
    bool heatingEnabled;                        // Space heating enable
    bool waterEnabled;                          // Water heating enable

    // ========== Override Flags (summer mode) ==========
    bool heatingOverrideOff;                    // Block heating circuit
    bool waterOverrideOff;                      // Block water heating circuit

    // ========== Boiler Temperature Controller ==========
    Temperature_t boilerOffHysteresis;          // +°C above target → OFF
    Temperature_t boilerOnHysteresis;           // -°C below target → ON (HALF)
    Temperature_t boilerFullThreshold;          // -°C below target → FULL

    // ========== Return Preheating Configuration ==========
    bool preheatEnabled;                        // Enable/disable return preheating
    uint8_t preheatOffMultiplier;               // OFF duration = ON duration × this
    uint8_t preheatMaxCycles;                   // Maximum pump cycles before timeout
    uint32_t preheatTimeoutMs;                  // Maximum preheating time
    uint16_t preheatPumpMinMs;                  // Minimum between pump state changes
    Temperature_t preheatSafeDiff;              // Exit preheating below this differential

    // ========== Pump Overrun Configuration ==========
    uint32_t pumpCooldownMs;                    // Pump overrun after burner stops

    // ========== Weather-Compensated Control ==========
    bool useWeatherCompensatedControl;          // Outside temp determines heating
    Temperature_t outsideTempHeatingThreshold;  // Enable heating when outside below this
    Temperature_t roomTempOverheatMargin;       // Stop if room > target + margin
    float roomTempCurveShiftFactor;             // Curve shift per 1°C room deviation

    // ========== Sensor Compensation Offsets ==========
    Temperature_t boilerOutputOffset;           // CH0 offset
    Temperature_t boilerReturnOffset;           // CH1 offset
    Temperature_t waterTankOffset;              // CH2 offset
    Temperature_t roomTempOffset;               // ANDRTF3 offset
    int16_t pressureOffset;                     // Pressure sensor offset

    // ========== Legacy Accessor Methods for Backward Compatibility ==========
    // These provide old field names as methods (avoids copy/assignment issues)
    Temperature_t getHeatingTargetTemperature() const { return targetTemperatureInside; }
    void setHeatingTargetTemperature(Temperature_t t) { targetTemperatureInside = t; }
    bool getHeatingEnable() const { return heatingEnabled; }
    void setHeatingEnable(bool v) { heatingEnabled = v; }
    bool getWHeaterEnable() const { return waterEnabled; }
    void setWHeaterEnable(bool v) { waterEnabled = v; }
    bool getWHeaterPriority() const { return wheaterPriorityEnabled; }
    void setWHeaterPriority(bool v) { wheaterPriorityEnabled = v; }

    // Constructor with defaults matching real SystemSettings
    SystemSettings() :
        // Water heater
        wheaterPriorityEnabled(true),
        wHeaterConfTempLimitLow(tempFromFloat(45.0f)),
        wHeaterConfTempLimitHigh(tempFromFloat(65.0f)),
        wHeaterConfTempChargeDelta(10.0f),
        wHeaterConfTempSafeLimitHigh(tempFromFloat(80.0f)),
        wHeaterConfTempSafeLimitLow(tempFromFloat(5.0f)),
        waterHeatingRate(1.0f),
        // Heating
        targetTemperatureInside(tempFromFloat(18.0f)),
        heating_curve_shift(20.0f),
        heating_curve_coeff(2.0f),
        heating_hysteresis(tempFromFloat(0.5f)),
        // Burner limits
        burner_low_limit(tempFromFloat(38.0f)),
        burner_high_limit(tempFromFloat(110.0f)),
        heating_low_limit(tempFromFloat(40.0f)),
        heating_high_limit(tempFromFloat(75.0f)),
        water_heating_low_limit(tempFromFloat(40.0f)),
        water_heating_high_limit(tempFromFloat(90.0f)),
        // PID
        spaceHeatingKp(1.0f),
        spaceHeatingKi(0.5f),
        spaceHeatingKd(0.1f),
        wHeaterKp(1.0f),
        wHeaterKi(0.5f),
        wHeaterKd(0.1f),
        useBoilerTempPID(true),
        // Enable states
        boilerEnabled(true),
        heatingEnabled(true),
        waterEnabled(true),
        heatingOverrideOff(false),
        waterOverrideOff(false),
        // Boiler controller
        boilerOffHysteresis(50),      // +5.0°C
        boilerOnHysteresis(30),       // -3.0°C
        boilerFullThreshold(100),     // -10.0°C
        // Preheating
        preheatEnabled(true),
        preheatOffMultiplier(5),
        preheatMaxCycles(8),
        preheatTimeoutMs(600000),     // 10 minutes
        preheatPumpMinMs(3000),       // 3 seconds
        preheatSafeDiff(250),         // 25.0°C
        // Pump overrun
        pumpCooldownMs(300000),       // 5 minutes
        // Weather-compensated control
        useWeatherCompensatedControl(true),
        outsideTempHeatingThreshold(150),  // 15.0°C
        roomTempOverheatMargin(20),        // 2.0°C
        roomTempCurveShiftFactor(2.0f),
        // Sensor offsets
        boilerOutputOffset(0),
        boilerReturnOffset(0),
        waterTankOffset(0),
        roomTempOffset(-17),          // -1.7°C default
        pressureOffset(0) {}
};

#endif // MOCK_SYSTEM_SETTINGS_H