/**
 * @file test_sensor_integration.cpp
 * @brief Integration tests for MB8ART sensor data pipeline
 */

#include <unity.h>
#include <cstring>
#include <chrono>
#include <thread>

// Include the headers we're testing
#include "../../include/shared/Temperature.h"
#include "mocks/MockSharedSensorReadings.h"
#include "mocks/MockTemperatureSensorFallback.h"

// Mock MB8ART sensor readings
class MockMB8ART {
private:
    Temperature_t channels[8];
    bool simulateTimeout;
    bool connected;
    
public:
    MockMB8ART() : simulateTimeout(false), connected(true) {
        // Initialize with reasonable temperatures
        for (int i = 0; i < 8; i++) {
            channels[i] = tempFromFloat(20.0f + i * 5.0f);
        }
    }
    
    void setChannelTemp(int channel, float temp) {
        if (channel >= 0 && channel < 8) {
            channels[channel] = tempFromFloat(temp);
        }
    }
    
    void setSimulateTimeout(bool timeout) {
        simulateTimeout = timeout;
    }
    
    void setConnected(bool conn) {
        connected = conn;
    }
    
    Temperature_t readChannel(int channel) {
        if (!connected || simulateTimeout || channel < 0 || channel >= 8) {
            return TEMP_INVALID;
        }
        return channels[channel];
    }
    
    bool isConnected() const {
        return connected && !simulateTimeout;
    }
};

// Test variables
static MockMB8ART* mockSensor = nullptr;
static SharedSensorReadings* readings = nullptr;
static TemperatureSensorFallback* fallback = nullptr;

void setupSensorIntegration() {
    mockSensor = new MockMB8ART();
    readings = new SharedSensorReadings();
    fallback = new TemperatureSensorFallback();
}

void tearDownSensorIntegration() {
    delete fallback;
    delete readings;
    delete mockSensor;
}

// Test normal sensor data flow
void test_sensor_normal_data_flow() {
    setupSensorIntegration();
    
    // Set known temperatures
    mockSensor->setChannelTemp(0, 65.5f);  // Boiler input
    mockSensor->setChannelTemp(1, 70.2f);  // Boiler output
    mockSensor->setChannelTemp(2, 55.0f);  // Water temp
    mockSensor->setChannelTemp(3, 45.0f);  // Return temp
    mockSensor->setChannelTemp(4, 120.0f); // Exhaust temp
    mockSensor->setChannelTemp(6, 22.0f);  // Inside temp
    mockSensor->setChannelTemp(7, 10.0f);  // Outside temp
    
    // Simulate sensor read and update
    readings->boilerTempInput = mockSensor->readChannel(0);
    readings->boilerTempOutput = mockSensor->readChannel(1);
    readings->waterTemp = mockSensor->readChannel(2);
    readings->returnTemp = mockSensor->readChannel(3);
    readings->exhaustTemp = mockSensor->readChannel(4);
    readings->insideTemp = mockSensor->readChannel(6);
    readings->outsideTemp = mockSensor->readChannel(7);
    
    // Verify correct values
    TEST_ASSERT_EQUAL_INT16(tempFromFloat(65.5f), readings->boilerTempInput);
    TEST_ASSERT_EQUAL_INT16(tempFromFloat(70.2f), readings->boilerTempOutput);
    TEST_ASSERT_EQUAL_INT16(tempFromFloat(55.0f), readings->waterTemp);
    TEST_ASSERT_EQUAL_INT16(tempFromFloat(45.0f), readings->returnTemp);
    TEST_ASSERT_EQUAL_INT16(tempFromFloat(120.0f), readings->exhaustTemp);
    TEST_ASSERT_EQUAL_INT16(tempFromFloat(22.0f), readings->insideTemp);
    TEST_ASSERT_EQUAL_INT16(tempFromFloat(10.0f), readings->outsideTemp);
    
    // Check fallback mode
    auto mode = fallback->evaluateMode(*readings);
    TEST_ASSERT_EQUAL(TemperatureSensorFallback::FallbackMode::NORMAL, mode);
    
    tearDownSensorIntegration();
}

// Test sensor timeout handling
void test_sensor_timeout_handling() {
    setupSensorIntegration();
    
    // Simulate timeout on critical sensors
    mockSensor->setSimulateTimeout(true);
    
    // Try to read sensors
    readings->boilerTempInput = mockSensor->readChannel(0);
    readings->boilerTempOutput = mockSensor->readChannel(1);
    readings->waterTemp = mockSensor->readChannel(2);
    
    // Verify invalid readings
    TEST_ASSERT_EQUAL_INT16(TEMP_INVALID, readings->boilerTempInput);
    TEST_ASSERT_EQUAL_INT16(TEMP_INVALID, readings->boilerTempOutput);
    TEST_ASSERT_EQUAL_INT16(TEMP_INVALID, readings->waterTemp);
    
    // Check fallback mode escalation
    auto mode = fallback->evaluateMode(*readings);
    TEST_ASSERT(mode >= TemperatureSensorFallback::FallbackMode::MINIMAL);
    
    tearDownSensorIntegration();
}

// Test partial sensor failure
void test_partial_sensor_failure() {
    setupSensorIntegration();
    
    // Set most sensors to valid values
    mockSensor->setChannelTemp(0, 65.0f);
    mockSensor->setChannelTemp(1, 70.0f);
    mockSensor->setChannelTemp(2, 55.0f);
    mockSensor->setChannelTemp(3, 45.0f);
    
    // Read sensors but simulate one failure
    readings->boilerTempInput = mockSensor->readChannel(0);
    readings->boilerTempOutput = TEMP_INVALID;  // Simulate this sensor failed
    readings->waterTemp = mockSensor->readChannel(2);
    readings->returnTemp = mockSensor->readChannel(3);
    
    // Check fallback mode - should degrade but not shutdown
    auto mode = fallback->evaluateMode(*readings);
    TEST_ASSERT_EQUAL(TemperatureSensorFallback::FallbackMode::DEGRADED, mode);
    
    // Get adjusted limits
    auto limits = fallback->getAdjustedLimits();
    TEST_ASSERT(limits.maxTemperature < tempFromFloat(85.0f)); // Should be reduced
    TEST_ASSERT(limits.powerLimit < 100); // Should be reduced
    
    tearDownSensorIntegration();
}

// Test sensor recovery
void test_sensor_recovery() {
    setupSensorIntegration();
    
    // Start with failed sensors
    readings->boilerTempInput = TEMP_INVALID;
    readings->boilerTempOutput = TEMP_INVALID;
    
    // Verify degraded mode
    auto mode1 = fallback->evaluateMode(*readings);
    TEST_ASSERT(mode1 >= TemperatureSensorFallback::FallbackMode::MINIMAL);
    
    // Recover sensors
    mockSensor->setChannelTemp(0, 65.0f);
    mockSensor->setChannelTemp(1, 70.0f);
    mockSensor->setChannelTemp(2, 55.0f);
    mockSensor->setChannelTemp(3, 45.0f);
    readings->boilerTempInput = mockSensor->readChannel(0);
    readings->boilerTempOutput = mockSensor->readChannel(1);
    readings->waterTemp = mockSensor->readChannel(2);
    readings->returnTemp = mockSensor->readChannel(3);
    
    // Update fallback multiple times to trigger recovery
    for (int i = 0; i < 5; i++) {
        fallback->evaluateMode(*readings);
    }
    
    // Should recover to normal
    auto mode2 = fallback->evaluateMode(*readings);
    TEST_ASSERT_EQUAL(TemperatureSensorFallback::FallbackMode::NORMAL, mode2);
    
    tearDownSensorIntegration();
}

// Test sensor data validation
void test_sensor_data_validation() {
    setupSensorIntegration();
    
    // Test unrealistic temperature values
    mockSensor->setChannelTemp(0, 150.0f);  // Too high for boiler
    mockSensor->setChannelTemp(1, -50.0f);  // Too low
    
    readings->boilerTempInput = mockSensor->readChannel(0);
    readings->boilerTempOutput = mockSensor->readChannel(1);
    
    // Values should be accepted by sensor but caught by safety validation
    TEST_ASSERT_EQUAL_INT16(tempFromFloat(150.0f), readings->boilerTempInput);
    TEST_ASSERT_EQUAL_INT16(tempFromFloat(-50.0f), readings->boilerTempOutput);
    
    // Fallback should detect anomaly
    auto mode = fallback->evaluateMode(*readings);
    TEST_ASSERT(mode != TemperatureSensorFallback::FallbackMode::NORMAL);
    
    tearDownSensorIntegration();
}

// Test sensor update timing
void test_sensor_update_timing() {
    setupSensorIntegration();
    
    // Record start time
    auto start = std::chrono::steady_clock::now();
    
    // Simulate multiple sensor reads
    for (int i = 0; i < 10; i++) {
        readings->boilerTempInput = mockSensor->readChannel(0);
        readings->boilerTempOutput = mockSensor->readChannel(1);
        readings->waterTemp = mockSensor->readChannel(2);
        readings->returnTemp = mockSensor->readChannel(3);
        readings->exhaustTemp = mockSensor->readChannel(4);
        readings->insideTemp = mockSensor->readChannel(6);
        readings->outsideTemp = mockSensor->readChannel(7);
        
        // Small delay to simulate real timing
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    // Should complete reasonably fast (< 200ms for 10 reads)
    TEST_ASSERT_LESS_THAN(200, duration.count());
    
    tearDownSensorIntegration();
}

// main() is in test_main.cpp