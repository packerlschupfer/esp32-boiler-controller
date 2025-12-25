// src/config/SystemSettingsStruct.h
#ifndef SYSTEM_SETTINGS_STRUCT_H
#define SYSTEM_SETTINGS_STRUCT_H

#include "shared/Temperature.h"

// Syslog compile-time defaults (override via platformio.ini build flags)
#ifndef SYSLOG_ENABLED_DEFAULT
    #define SYSLOG_ENABLED_DEFAULT false
#endif
#ifndef SYSLOG_SERVER_IP
    #define SYSLOG_SERVER_IP 192, 168, 20, 27
#endif
#ifndef SYSLOG_MIN_LEVEL
    #define SYSLOG_MIN_LEVEL 2  // ESP_LOG_WARN
#endif

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
    Temperature_t heating_hysteresis;      // Hysteresis for space heating control

    // Global Burner Limits (All Modes)
    Temperature_t burner_low_limit;        // Minimum burner temperature (all modes)
    Temperature_t burner_high_limit;       // Maximum burner temperature (all modes)

    // Space Heating Limits (Mode-Specific)
    Temperature_t heating_low_limit;       // Minimum for space heating
    Temperature_t heating_high_limit;      // Maximum for space heating

    // Water Heating Limits (Mode-Specific)
    Temperature_t water_heating_low_limit;   // Minimum for water heating
    Temperature_t water_heating_high_limit;  // Maximum for water heating

    // Note: Sensor intervals are now in SystemConstants::Timing (MB8ART_SENSOR_READ_INTERVAL_MS, ANDRTF3_SENSOR_READ_INTERVAL_MS)

    // Space Heating PID Coefficients
    float spaceHeatingKp = 1.0f; // Proportional gain for space heating
    float spaceHeatingKi = 0.5f; // Integral gain for space heating
    float spaceHeatingKd = 0.1f; // Derivative gain for space heating

    // Water Heating PID Coefficients
    float wHeaterKp = 1.0f;      // Proportional gain for water heating
    float wHeaterKi = 0.5f;      // Integral gain for water heating
    float wHeaterKd = 0.1f;      // Derivative gain for water heating

    // Boiler Temperature PID Control Mode
    // When enabled, uses PID to control burner power (OFF/HALF/FULL) instead of simple bang-bang
    // - Space heating uses spaceHeatingKp/Ki/Kd (conservative for large radiator thermal mass)
    // - Water heating uses wHeaterKp/Ki/Kd (more aggressive for faster tank charging)
    bool useBoilerTempPID = true;       // true = PID-driven bang-bang (DEFAULT), false = simple bang-bang

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
    Temperature_t roomTempOffset = -17;     // Room temperature offset (default -1.7°C)
    // Pressure (in hundredths of BAR, e.g., -5 = -0.05 BAR)
    int16_t pressureOffset = 0;             // Pressure sensor offset

    // Boiler Temperature Controller Configuration
    // Hysteresis bands for three-point bang-bang control (tenths of °C)
    Temperature_t boilerOffHysteresis = 50;   // +5.0°C above target → OFF
    Temperature_t boilerOnHysteresis = 30;    // -3.0°C below target → ON (HALF)
    Temperature_t boilerFullThreshold = 100;  // -10.0°C below target → FULL

    // Return Preheating Configuration (thermal shock mitigation)
    // Cycles heating pump to warm cold return before burner start
    bool preheatEnabled = true;               // Enable/disable return preheating
    uint8_t preheatOffMultiplier = 5;         // OFF duration = ON duration × this
    uint8_t preheatMaxCycles = 8;             // Maximum pump cycles before timeout
    uint32_t preheatTimeoutMs = 600000;       // 10 minutes maximum preheating time
    uint16_t preheatPumpMinMs = 3000;         // 3s minimum between pump state changes during preheating
    Temperature_t preheatSafeDiff = 250;      // 25.0°C - exit preheating below this differential

    // Pump Overrun Configuration (heat dissipation after burner stops)
    // Keeps pump running after burner stops to distribute residual heat
    uint32_t pumpCooldownMs = 300000;         // 5 minutes pump overrun (was 3min, increased for PID mode)

    // Weather-Compensated Heating Control
    // When enabled, outside temperature determines heating ON/OFF instead of room temp.
    // Room temp only provides curve corrections and overheat protection.
    bool useWeatherCompensatedControl = true;         // Feature flag (ON for testing)
    Temperature_t outsideTempHeatingThreshold = 150;  // 15.0°C - enable heating when outside below this
    Temperature_t roomTempOverheatMargin = 20;        // 2.0°C - stop heating if room > target + margin
    float roomTempCurveShiftFactor = 2.0f;            // Curve shift per 1°C room deviation

    // Syslog Configuration (remote logging)
    // Sends log messages to central syslog server over UDP (RFC 3164)
    // Override defaults via platformio.ini: -DSYSLOG_SERVER_IP=192,168,20,16 -DSYSLOG_MIN_LEVEL=4
    bool syslogEnabled = SYSLOG_ENABLED_DEFAULT;
    uint8_t syslogServerIP[4] = {SYSLOG_SERVER_IP};
    uint16_t syslogPort = 514;                        // Standard syslog port
    uint8_t syslogFacility = 16;                      // LOCAL0 = 16 (LOCAL0-7 = 16-23)
    uint8_t syslogMinLevel = SYSLOG_MIN_LEVEL;

    // Constructor
    SystemSettings(
        bool wheaterPriorityEnabled = true,
        Temperature_t wHeaterConfTempLimitLow = tempFromFloat(45.0f),
        Temperature_t wHeaterConfTempLimitHigh = tempFromFloat(65.0f),
        float wHeaterConfTempChargeDelta = 10.0f,
        Temperature_t wHeaterConfTempSafeLimitHigh = tempFromFloat(80.0f),
        Temperature_t wHeaterConfTempSafeLimitLow = tempFromFloat(5.0f),
        float waterHeatingRate = 1.0f,
        Temperature_t targetTemperatureInside = tempFromFloat(18.0f),
        float heating_curve_shift = 20.0f,
        float heating_curve_coeff = 2.0f,
        Temperature_t heating_hysteresis = tempFromFloat(0.5f),
        Temperature_t burner_low_limit = tempFromFloat(38.0f),   // Global burner minimum
        Temperature_t burner_high_limit = tempFromFloat(110.0f), // Global burner maximum
        Temperature_t heating_low_limit = tempFromFloat(40.0f),  // Space heating minimum
        Temperature_t heating_high_limit = tempFromFloat(75.0f), // Space heating maximum
        Temperature_t water_heating_low_limit = tempFromFloat(40.0f),  // Water heating minimum
        Temperature_t water_heating_high_limit = tempFromFloat(90.0f), // Water heating maximum
        float spaceHeatingKp = 1.0f,
        float spaceHeatingKi = 0.5f,
        float spaceHeatingKd = 0.1f,
        float wHeaterKp = 1.0f,
        float wHeaterKi = 0.5f,
        float wHeaterKd = 0.1f,
        bool useBoilerTempPID = true,
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
          heating_hysteresis(heating_hysteresis),
          burner_low_limit(burner_low_limit),
          burner_high_limit(burner_high_limit),
          heating_low_limit(heating_low_limit),
          heating_high_limit(heating_high_limit),
          water_heating_low_limit(water_heating_low_limit),
          water_heating_high_limit(water_heating_high_limit),
          spaceHeatingKp(spaceHeatingKp),
          spaceHeatingKi(spaceHeatingKi),
          spaceHeatingKd(spaceHeatingKd),
          wHeaterKp(wHeaterKp),
          wHeaterKi(wHeaterKi),
          wHeaterKd(wHeaterKd),
          useBoilerTempPID(useBoilerTempPID),
          autotuneRelayAmplitude(autotuneRelayAmplitude),
          autotuneHysteresis(autotuneHysteresis),
          autotuneMethod(autotuneMethod) {}
};

#endif // SYSTEM_SETTINGS_STRUCT_H
