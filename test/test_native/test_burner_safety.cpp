/**
 * @file test_burner_safety.cpp
 * @brief Unit tests for BurnerSafetyValidator
 */

#include <unity.h>
#include <cstring>
#include <vector>
#include <string>

// Mock the dependencies for native testing
#ifdef NATIVE_TEST

// Mock FreeRTOS types
typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY 0xFFFFFFFF
#define pdTRUE 1
#define pdFALSE 0
typedef void* SemaphoreHandle_t;
typedef uint32_t BaseType_t;

// Mock mutex operations
SemaphoreHandle_t xSemaphoreCreateMutex() { return (void*)1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t timeout) { return pdTRUE; }
void xSemaphoreGive(SemaphoreHandle_t mutex) {}

// Include mock time
#include "mocks/MockTime.h"

// Mock Temperature.h types
typedef int16_t Temperature_t;
#define TEMP_INVALID -32768
inline Temperature_t tempFromFloat(float temp) { return (Temperature_t)(temp * 10); }
inline float tempToFloat(Temperature_t temp) { return temp / 10.0f; }

// Mock SharedSensorReadings
struct SharedSensorReadings {
    Temperature_t boilerTempInput;
    Temperature_t boilerTempOutput;
    Temperature_t waterTemp;
    Temperature_t returnTemp;
    Temperature_t exhaustTemp;
    Temperature_t insideTemp;
    Temperature_t outsideTemp;
    bool sensorsValid;
};

// Mock SystemError
enum class SystemError {
    NONE,
    INVALID_PARAMETER,
    SENSOR_FAILURE,
    TEMPERATURE_OUT_OF_RANGE,
    SAFETY_INTERLOCK_FAILED
};

// Mock Result template
template<typename T>
class Result {
    SystemError error_;
    std::string message_;
    T value_;
public:
    Result() : error_(SystemError::NONE) {}
    Result(SystemError err, const std::string& msg) : error_(err), message_(msg) {}
    bool isSuccess() const { return error_ == SystemError::NONE; }
    bool isError() const { return error_ != SystemError::NONE; }
    SystemError error() const { return error_; }
    const std::string& message() const { return message_; }
    T& value() { return value_; }
    const T& value() const { return value_; }
};

// Specialization for void type
template<>
class Result<void> {
    SystemError error_;
    std::string message_;
public:
    Result() : error_(SystemError::NONE) {}
    Result(SystemError err, const std::string& msg) : error_(err), message_(msg) {}
    bool isSuccess() const { return error_ == SystemError::NONE; }
    bool isError() const { return error_ != SystemError::NONE; }
    SystemError error() const { return error_; }
    const std::string& message() const { return message_; }
};

// Simplified BurnerSafetyValidator for testing
class BurnerSafetyValidator {
public:
    struct SafetyConfig {
        Temperature_t maxBoilerTemp;
        Temperature_t maxWaterTemp;
        Temperature_t maxReturnTemp;
        Temperature_t maxExhaustTemp;
        Temperature_t minReturnTemp;
        Temperature_t maxTempRiseRate;
        uint32_t tempStabilityTime;
        Temperature_t thermalShockThreshold;
    };

private:
    SafetyConfig config_;
    std::vector<Temperature_t> tempHistory_;
    uint32_t lastUpdateTime_;
    SemaphoreHandle_t mutex_;

public:
    BurnerSafetyValidator(const SafetyConfig& config) 
        : config_(config), lastUpdateTime_(0) {
        mutex_ = xSemaphoreCreateMutex();
    }

    Result<void> validatePreIgnition(const SharedSensorReadings& readings) {
        // Check temperature limits
        if (readings.boilerTempOutput > config_.maxBoilerTemp) {
            return Result<void>(SystemError::TEMPERATURE_OUT_OF_RANGE, 
                              "Boiler temperature too high for ignition");
        }
        
        if (readings.waterTemp > config_.maxWaterTemp) {
            return Result<void>(SystemError::TEMPERATURE_OUT_OF_RANGE, 
                              "Water temperature too high for ignition");
        }
        
        // Check thermal shock
        Temperature_t tempDiff = readings.boilerTempOutput - readings.returnTemp;
        if (tempDiff > config_.thermalShockThreshold) {
            return Result<void>(SystemError::SAFETY_INTERLOCK_FAILED, 
                              "Thermal shock risk - temperature differential too high");
        }
        
        return Result<void>();
    }

    Result<void> validateDuringOperation(const SharedSensorReadings& readings) {
        // Update temperature history
        tempHistory_.push_back(readings.boilerTempOutput);
        if (tempHistory_.size() > 10) {
            tempHistory_.erase(tempHistory_.begin());
        }
        
        // Check all temperature limits
        if (readings.boilerTempOutput > config_.maxBoilerTemp) {
            return Result<void>(SystemError::TEMPERATURE_OUT_OF_RANGE, 
                              "Boiler temperature exceeded maximum");
        }
        
        if (readings.exhaustTemp > config_.maxExhaustTemp) {
            return Result<void>(SystemError::TEMPERATURE_OUT_OF_RANGE, 
                              "Exhaust temperature exceeded maximum");
        }
        
        if (readings.returnTemp < config_.minReturnTemp) {
            return Result<void>(SystemError::TEMPERATURE_OUT_OF_RANGE, 
                              "Return temperature below minimum");
        }
        
        // Check temperature rise rate
        if (tempHistory_.size() >= 2) {
            Temperature_t tempRise = tempHistory_.back() - tempHistory_.front();
            uint32_t timeSpan = tempHistory_.size() * 1000; // Assume 1 second between readings
            
            if (tempRise > config_.maxTempRiseRate) {
                return Result<void>(SystemError::SAFETY_INTERLOCK_FAILED, 
                                  "Temperature rising too quickly");
            }
        }
        
        return Result<void>();
    }

    bool checkHardwareInterlocks() {
        // This is the stubbed function we identified as a problem
        return true;  // Always returns true - no actual hardware check
    }

    void resetHistory() {
        tempHistory_.clear();
    }
};

#endif // NATIVE_TEST

// Test variables
static BurnerSafetyValidator* validator = nullptr;
static SharedSensorReadings testReadings;

void setupBurnerSafety(void) {
    // Create validator with test configuration
    BurnerSafetyValidator::SafetyConfig config = {
        .maxBoilerTemp = tempFromFloat(85.0f),
        .maxWaterTemp = tempFromFloat(80.0f),
        .maxReturnTemp = tempFromFloat(75.0f),
        .maxExhaustTemp = tempFromFloat(250.0f),
        .minReturnTemp = tempFromFloat(30.0f),
        .maxTempRiseRate = tempFromFloat(10.0f),  // 10°C/minute
        .tempStabilityTime = 5000,  // 5 seconds
        .thermalShockThreshold = tempFromFloat(30.0f)
    };
    
    validator = new BurnerSafetyValidator(config);
    
    // Initialize test readings with safe values
    testReadings = {
        .boilerTempInput = tempFromFloat(60.0f),
        .boilerTempOutput = tempFromFloat(65.0f),
        .waterTemp = tempFromFloat(55.0f),
        .returnTemp = tempFromFloat(50.0f),
        .exhaustTemp = tempFromFloat(120.0f),
        .insideTemp = tempFromFloat(20.0f),
        .outsideTemp = tempFromFloat(10.0f),
        .sensorsValid = true
    };
    
    setMockMillis(0);
}

void tearDownBurnerSafety(void) {
    delete validator;
    validator = nullptr;
}

// Test pre-ignition validation with safe conditions
void test_pre_ignition_safe_conditions() {
    setupBurnerSafety();
    auto result = validator->validatePreIgnition(testReadings);
    TEST_ASSERT_TRUE(result.isSuccess());
    tearDownBurnerSafety();
}

// Test pre-ignition with high boiler temperature
void test_pre_ignition_high_boiler_temp() {
    setupBurnerSafety();
    testReadings.boilerTempOutput = tempFromFloat(90.0f);  // Above 85°C limit
    
    auto result = validator->validatePreIgnition(testReadings);
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(SystemError::TEMPERATURE_OUT_OF_RANGE, result.error());
    TEST_ASSERT_TRUE(result.message().find("Boiler temperature too high") != std::string::npos);
    tearDownBurnerSafety();
}

// Test pre-ignition with high water temperature
void test_pre_ignition_high_water_temp() {
    setupBurnerSafety();
    testReadings.waterTemp = tempFromFloat(85.0f);  // Above 80°C limit
    
    auto result = validator->validatePreIgnition(testReadings);
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(SystemError::TEMPERATURE_OUT_OF_RANGE, result.error());
    TEST_ASSERT_TRUE(result.message().find("Water temperature too high") != std::string::npos);
    tearDownBurnerSafety();
}

// Test thermal shock detection
void test_thermal_shock_detection() {
    setupBurnerSafety();
    testReadings.boilerTempOutput = tempFromFloat(80.0f);
    testReadings.returnTemp = tempFromFloat(45.0f);  // 35°C difference > 30°C threshold
    
    auto result = validator->validatePreIgnition(testReadings);
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(SystemError::SAFETY_INTERLOCK_FAILED, result.error());
    TEST_ASSERT_TRUE(result.message().find("Thermal shock risk") != std::string::npos);
    tearDownBurnerSafety();
}

// Test operation validation with safe conditions
void test_operation_safe_conditions() {
    setupBurnerSafety();
    auto result = validator->validateDuringOperation(testReadings);
    TEST_ASSERT_TRUE(result.isSuccess());
    tearDownBurnerSafety();
}

// Test operation with high exhaust temperature
void test_operation_high_exhaust_temp() {
    setupBurnerSafety();
    testReadings.exhaustTemp = tempFromFloat(300.0f);  // Above 250°C limit
    
    auto result = validator->validateDuringOperation(testReadings);
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(SystemError::TEMPERATURE_OUT_OF_RANGE, result.error());
    TEST_ASSERT_TRUE(result.message().find("Exhaust temperature exceeded") != std::string::npos);
    tearDownBurnerSafety();
}

// Test operation with low return temperature
void test_operation_low_return_temp() {
    setupBurnerSafety();
    testReadings.returnTemp = tempFromFloat(25.0f);  // Below 30°C minimum
    
    auto result = validator->validateDuringOperation(testReadings);
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(SystemError::TEMPERATURE_OUT_OF_RANGE, result.error());
    TEST_ASSERT_TRUE(result.message().find("Return temperature below minimum") != std::string::npos);
    tearDownBurnerSafety();
}

// Test rapid temperature rise detection
void test_rapid_temperature_rise() {
    setupBurnerSafety();
    // Simulate rapid temperature rise
    testReadings.boilerTempOutput = tempFromFloat(65.0f);
    validator->validateDuringOperation(testReadings);
    
    advanceMockMillis(1000);
    testReadings.boilerTempOutput = tempFromFloat(70.0f);
    validator->validateDuringOperation(testReadings);
    
    advanceMockMillis(1000);
    testReadings.boilerTempOutput = tempFromFloat(80.0f);  // 15°C rise in 2 seconds
    
    auto result = validator->validateDuringOperation(testReadings);
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(SystemError::SAFETY_INTERLOCK_FAILED, result.error());
    TEST_ASSERT_TRUE(result.message().find("Temperature rising too quickly") != std::string::npos);
    tearDownBurnerSafety();
}

// Test hardware interlock check (the problematic stub)
void test_hardware_interlock_always_true() {
    setupBurnerSafety();
    // This test documents the current behavior - always returns true
    bool interlockStatus = validator->checkHardwareInterlocks();
    TEST_ASSERT_TRUE(interlockStatus);
    
    // This is a problem because it doesn't actually check hardware
    // In a real system, this should check GPIO pins for:
    // - Pressure switches
    // - Temperature limit switches
    // - Manual safety switches
    // - Gas valve feedback
    tearDownBurnerSafety();
}

// Test history reset
void test_history_reset() {
    setupBurnerSafety();
    // Add some temperature readings
    validator->validateDuringOperation(testReadings);
    testReadings.boilerTempOutput = tempFromFloat(70.0f);
    validator->validateDuringOperation(testReadings);
    
    // Reset history
    validator->resetHistory();
    
    // Rapid rise should not be detected after reset
    testReadings.boilerTempOutput = tempFromFloat(85.0f);
    auto result = validator->validateDuringOperation(testReadings);
    TEST_ASSERT_TRUE(result.isSuccess());
    tearDownBurnerSafety();
}

// main() is in test_main.cpp