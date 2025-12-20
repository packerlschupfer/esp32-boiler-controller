/**
 * @file test_control_loop_integration.cpp
 * @brief Integration tests for complete control loop behavior
 */

#include <unity.h>
#include <cmath>

// Include headers we're testing
#include "../../include/shared/Temperature.h"
#include "mocks/MockTime.h"
#include "mocks/MockSharedSensorReadings.h"
#include "mocks/MockSystemSettings.h"
#include "mocks/MockHeatingControl.h"
#include "mocks/MockBurnerRequestManager.h"
#include "mocks/MockMB8ART.h"

// Mock components
class MockSystemComponents {
public:
    SharedSensorReadings readings;
    SystemSettings settings;
    HeatingControlModule* heatingControl;
    BurnerRequestManager* requestManager;
    
    MockSystemComponents() {
        // Initialize with default settings
        settings.heating_target_temperature = tempFromFloat(21.0f);
        settings.heating_hysteresis = tempFromFloat(2.0f);
        settings.heating_curve_enable = false;
        settings.wHeaterConfTempLimitLow = tempFromFloat(45.0f);
        settings.wHeaterConfTempLimitHigh = tempFromFloat(60.0f);
        settings.wHeaterEnable = true;
        settings.heatingEnable = true;
        
        // Initialize readings with safe values
        readings.boilerTempInput = tempFromFloat(20.0f);
        readings.boilerTempOutput = tempFromFloat(20.0f);
        readings.waterTemp = tempFromFloat(50.0f);
        readings.returnTemp = tempFromFloat(40.0f);
        readings.exhaustTemp = tempFromFloat(100.0f);
        readings.insideTemp = tempFromFloat(19.0f);
        readings.outsideTemp = tempFromFloat(10.0f);
        
        heatingControl = new HeatingControlModule();
        requestManager = new BurnerRequestManager();
    }
    
    ~MockSystemComponents() {
        delete requestManager;
        delete heatingControl;
    }
    
    void simulateTemperatureChange(float deltaTemp) {
        // Simulate gradual temperature change
        readings.insideTemp = readings.insideTemp + tempFromFloat(deltaTemp);
        readings.boilerTempOutput = readings.boilerTempOutput + tempFromFloat(deltaTemp * 2);
        readings.returnTemp = readings.returnTemp + tempFromFloat(deltaTemp * 1.5);
    }
};

// Test basic heating control loop
void test_basic_heating_control_loop() {
    MockSystemComponents system;
    
    // Set room temperature below target
    system.readings.insideTemp = tempFromFloat(18.0f);
    system.settings.heating_target_temperature = tempFromFloat(21.0f);
    
    // Calculate if heating is needed
    Temperature_t targetTemp = system.heatingControl->calculateSpaceHeatingTargetTemp(
        system.readings, system.settings);
    
    bool heatingNeeded = system.heatingControl->checkHeatingConditions(
        system.readings, targetTemp, system.settings.heating_hysteresis);
    
    TEST_ASSERT_TRUE(heatingNeeded);
    
    // Request should be made to burner
    if (heatingNeeded) {
        system.requestManager->requestHeating(targetTemp, 100);
    }
    
    // Verify request was set
    auto request = system.requestManager->getCurrentRequest();
    TEST_ASSERT_EQUAL(BurnerRequestManager::RequestSource::HEATING, request.source);
    TEST_ASSERT_EQUAL(targetTemp, request.targetTemperature);
    TEST_ASSERT_EQUAL(100, request.powerPercent);
}

// Test water heating priority
void test_water_heating_priority() {
    MockSystemComponents system;
    
    // Both heating and water need heat
    system.readings.insideTemp = tempFromFloat(18.0f);
    system.readings.waterTemp = tempFromFloat(40.0f);
    system.settings.wHeaterPriority = true;
    
    // Space heating request
    system.requestManager->requestHeating(tempFromFloat(70.0f), 80);
    
    // Water heating request with priority
    system.requestManager->requestWater(tempFromFloat(65.0f), 100);
    
    // Water should win due to priority
    auto request = system.requestManager->getCurrentRequest();
    TEST_ASSERT_EQUAL(BurnerRequestManager::RequestSource::WATER, request.source);
    TEST_ASSERT_EQUAL(tempFromFloat(65.0f), request.targetTemperature);
    TEST_ASSERT_EQUAL(100, request.powerPercent);
}

// Test PID control response
void test_pid_control_response() {
    MockSystemComponents system;
    
    // Configure PID settings
    system.settings.pid_kp = 2.0f;
    system.settings.pid_ki = 0.1f;
    system.settings.pid_kd = 0.5f;
    system.settings.pid_enable = true;
    
    // Initialize PID
    system.heatingControl->initializePID(system.settings);
    
    // Simulate control loop iterations
    float setpoint = 21.0f;
    float currentTemp = 18.0f;
    
    for (int i = 0; i < 10; i++) {
        // Update readings
        system.readings.insideTemp = tempFromFloat(currentTemp);
        
        // Calculate PID output
        float output = system.heatingControl->calculatePIDOutput(
            setpoint, currentTemp, 1.0f); // 1 second dt
        
        // Output should be positive when below setpoint
        TEST_ASSERT_GREATER_THAN(0.0f, output);
        
        // Simulate temperature response
        currentTemp += output * 0.01f; // Small gain
        
        advanceMockMillis( 1000);
    }
    
    // Temperature should be moving toward setpoint (allow small tolerance)
    TEST_ASSERT_GREATER_THAN(17.99f, currentTemp);
}

// Test hysteresis behavior
void test_hysteresis_control() {
    MockSystemComponents system;
    
    system.settings.heating_target_temperature = tempFromFloat(21.0f);
    system.settings.heating_hysteresis = tempFromFloat(2.0f);
    
    // Test turn-on threshold (target - hysteresis)
    system.readings.insideTemp = tempFromFloat(18.9f); // Just below 19°C threshold
    bool shouldHeat1 = system.heatingControl->checkHeatingConditions(
        system.readings, system.settings.heating_target_temperature, 
        system.settings.heating_hysteresis);
    TEST_ASSERT_TRUE(shouldHeat1);
    
    // Test turn-off threshold (target)
    system.readings.insideTemp = tempFromFloat(21.1f); // Just above 21°C
    bool shouldHeat2 = system.heatingControl->checkHeatingConditions(
        system.readings, system.settings.heating_target_temperature, 
        system.settings.heating_hysteresis);
    TEST_ASSERT_FALSE(shouldHeat2);
    
    // Test dead band (between thresholds)
    system.readings.insideTemp = tempFromFloat(20.0f);
    // Result depends on previous state (not tested here)
}

// Test weather compensation
void test_weather_compensation() {
    MockSystemComponents system;
    
    // Enable weather compensation
    system.settings.heating_curve_enable = true;
    system.settings.heating_curve_coeff = 1.5f;
    system.settings.heating_curve_shift = 20.0f;
    system.settings.heating_target_temperature = tempFromFloat(21.0f);
    
    // Test with cold outside temperature
    system.readings.outsideTemp = tempFromFloat(-5.0f);
    Temperature_t targetCold = system.heatingControl->calculateSpaceHeatingTargetTemp(
        system.readings, system.settings);
    
    // Test with warm outside temperature
    system.readings.outsideTemp = tempFromFloat(15.0f);
    Temperature_t targetWarm = system.heatingControl->calculateSpaceHeatingTargetTemp(
        system.readings, system.settings);
    
    // Target should be higher when it's colder outside
    TEST_ASSERT_GREATER_THAN(targetWarm, targetCold);
}

// Test control loop timing
void test_control_loop_timing() {
    MockSystemComponents system;
    
    // Track request changes
    int requestChanges = 0;
    BurnerRequestManager::BurnerRequest lastRequest = {
        .source = BurnerRequestManager::RequestSource::NONE,
        .targetTemperature = 0,
        .powerPercent = 0
    };
    
    // Simulate 60 seconds of control loop
    for (int seconds = 0; seconds < 60; seconds++) {
        // Update every second
        bool heatingNeeded = system.heatingControl->checkHeatingConditions(
            system.readings, 
            system.settings.heating_target_temperature,
            system.settings.heating_hysteresis);
        
        if (heatingNeeded) {
            system.requestManager->requestHeating(
                system.settings.heating_target_temperature, 100);
        } else {
            system.requestManager->clearHeatingRequest();
        }
        
        auto currentRequest = system.requestManager->getCurrentRequest();
        if (currentRequest.source != lastRequest.source ||
            currentRequest.targetTemperature != lastRequest.targetTemperature) {
            requestChanges++;
            lastRequest = currentRequest;
        }
        
        // Simulate small temperature changes
        if (heatingNeeded) {
            system.simulateTemperatureChange(0.05f);
        } else {
            system.simulateTemperatureChange(-0.02f);
        }
        
        advanceMockMillis( 1000);
    }
    
    // Should have reasonable number of state changes (not oscillating)
    TEST_ASSERT_LESS_THAN(10, requestChanges);
}

// Test emergency stop integration
void test_emergency_stop_clears_requests() {
    MockSystemComponents system;
    
    // Set up active heating request
    system.requestManager->requestHeating(tempFromFloat(70.0f), 100);
    TEST_ASSERT_NOT_EQUAL(BurnerRequestManager::RequestSource::NONE, 
                         system.requestManager->getCurrentRequest().source);
    
    // Trigger emergency stop
    system.requestManager->emergencyStop();
    
    // All requests should be cleared
    auto request = system.requestManager->getCurrentRequest();
    TEST_ASSERT_EQUAL(BurnerRequestManager::RequestSource::EMERGENCY, request.source);
    TEST_ASSERT_EQUAL(0, request.powerPercent);
}

// Test anti-flapping behavior
void test_anti_flapping_control() {
    MockSystemComponents system;
    
    // Set temperature right at threshold
    system.settings.heating_target_temperature = tempFromFloat(21.0f);
    system.settings.heating_hysteresis = tempFromFloat(0.5f); // Small hysteresis
    system.readings.insideTemp = tempFromFloat(20.5f);
    
    // Track state changes
    int stateChanges = 0;
    bool lastState = false;
    
    // Simulate temperature oscillations
    for (int i = 0; i < 20; i++) {
        bool heatingNeeded = system.heatingControl->checkHeatingConditions(
            system.readings, 
            system.settings.heating_target_temperature,
            system.settings.heating_hysteresis);
        
        if (heatingNeeded != lastState) {
            stateChanges++;
            lastState = heatingNeeded;
        }
        
        // Oscillate temperature slightly
        float noise = (i % 2 == 0) ? 0.1f : -0.1f;
        system.readings.insideTemp = system.readings.insideTemp + tempFromFloat(noise);
        
        advanceMockMillis( 5000); // 5 seconds
    }
    
    // Should not oscillate excessively despite noise
    TEST_ASSERT_LESS_THAN(5, stateChanges);
}

// Temperature cycle tests that simulate actual temperature changes
void test_temperature_simulation_basics() {
    MockMB8ART sensorBus;
    SharedSensorReadings readings;
    
    // Set initial temperature
    sensorBus.setChannelValue(7, tempFromFloat(18.0f));
    readings.insideTemp = sensorBus.readChannel(7);
    TEST_ASSERT_EQUAL(180, readings.insideTemp);
    
    // Simulate heating
    for (int i = 0; i < 10; i++) {
        Temperature_t current = sensorBus.readChannel(7);
        sensorBus.setChannelValue(7, current + 2);  // +0.2°C per step
    }
    
    readings.insideTemp = sensorBus.readChannel(7);
    TEST_ASSERT_EQUAL(200, readings.insideTemp);  // 18 + 10*0.2 = 20°C
}

// Test complete heating cycle with hysteresis
void test_heating_cycle_with_hysteresis() {
    SystemSettings settings;
    BurnerRequestManager requestManager;
    
    settings.heatingEnable = true;
    settings.heating_target_temperature = tempFromFloat(20.0f);
    settings.heating_hysteresis = tempFromFloat(2.0f);
    
    // Cold start - should request heating
    Temperature_t currentTemp = tempFromFloat(17.0f);
    if (currentTemp < (settings.heating_target_temperature - settings.heating_hysteresis)) {
        requestManager.requestHeating(tempFromFloat(70.0f), 100);
    }
    
    TEST_ASSERT_EQUAL(1, (int)requestManager.getCurrentRequest().source);
    
    // Reach target - should stop
    currentTemp = tempFromFloat(20.0f);
    if (currentTemp >= settings.heating_target_temperature) {
        requestManager.clearHeatingRequest();
    }
    
    TEST_ASSERT_EQUAL(0, (int)requestManager.getCurrentRequest().source);
    
    // Cool down but within hysteresis - should stay off
    currentTemp = tempFromFloat(19.0f);
    // No action - still off
    TEST_ASSERT_EQUAL(0, (int)requestManager.getCurrentRequest().source);
    
    // Cool below hysteresis - should restart
    currentTemp = tempFromFloat(17.5f);
    if (currentTemp < (settings.heating_target_temperature - settings.heating_hysteresis)) {
        requestManager.requestHeating(tempFromFloat(70.0f), 100);
    }
    
    TEST_ASSERT_EQUAL(1, (int)requestManager.getCurrentRequest().source);
}

// main() is in test_main.cpp