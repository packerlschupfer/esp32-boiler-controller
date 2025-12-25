/**
 * @file test_persistent_storage_integration.cpp
 * @brief Integration tests for PersistentStorage with system parameters
 */

#include <unity.h>
#include <map>
#include <string>
#include <vector>
#include <cstring>

// Mock includes
#include "mocks/MockSystemSettings.h"
#include "mocks/MockSharedSensorReadings.h"

// Mock NVS implementation for testing
class MockNVS {
private:
    std::map<std::string, std::vector<uint8_t>> storage;
    bool initialized;
    
public:
    MockNVS() : initialized(false) {}
    
    bool init() {
        initialized = true;
        return true;
    }
    
    bool isInitialized() const {
        return initialized;
    }
    
    bool write(const std::string& key, const void* data, size_t size) {
        if (!initialized) return false;
        
        std::vector<uint8_t> buffer(size);
        memcpy(buffer.data(), data, size);
        storage[key] = buffer;
        return true;
    }
    
    bool read(const std::string& key, void* data, size_t size) {
        if (!initialized) return false;
        
        auto it = storage.find(key);
        if (it == storage.end()) return false;
        if (it->second.size() != size) return false;
        
        memcpy(data, it->second.data(), size);
        return true;
    }
    
    bool exists(const std::string& key) {
        return storage.find(key) != storage.end();
    }
    
    bool erase(const std::string& key) {
        if (!initialized) return false;
        
        storage.erase(key);
        return true;
    }
    
    void clear() {
        storage.clear();
    }
    
    size_t getUsedEntries() const {
        return storage.size();
    }
};

// Mock PersistentStorage
class MockPersistentStorage {
public:
    enum class Result {
        SUCCESS,
        ERROR_NOT_INITIALIZED,
        ERROR_NOT_FOUND,
        ERROR_INVALID_PARAMETER,
        ERROR_WRITE_FAILED,
        ERROR_READ_FAILED
    };
    
    struct ParameterInfo {
        std::string name;
        float value;
        float minValue;
        float maxValue;
        std::string description;
        enum Access { ACCESS_READ_WRITE, ACCESS_READ_ONLY } access;
    };
    
private:
    MockNVS* nvs;
    std::map<std::string, ParameterInfo> parameters;
    bool initialized;
    SystemSettings* settings;
    
public:
    MockPersistentStorage(SystemSettings* systemSettings) 
        : nvs(nullptr), initialized(false), settings(systemSettings) {}
    
    Result initialize(MockNVS* nvsInstance) {
        nvs = nvsInstance;
        if (!nvs || !nvs->init()) {
            return Result::ERROR_NOT_INITIALIZED;
        }
        initialized = true;
        return Result::SUCCESS;
    }
    
    Result registerParameter(const std::string& name, float* valuePtr, 
                           float minVal, float maxVal,
                           const std::string& description = "",
                           ParameterInfo::Access access = ParameterInfo::ACCESS_READ_WRITE) {
        if (!initialized) return Result::ERROR_NOT_INITIALIZED;
        
        ParameterInfo info = {
            name, *valuePtr, minVal, maxVal, description, access
        };
        parameters[name] = info;
        return Result::SUCCESS;
    }
    
    Result setParameter(const std::string& name, float value) {
        if (!initialized) return Result::ERROR_NOT_INITIALIZED;
        
        auto it = parameters.find(name);
        if (it == parameters.end()) return Result::ERROR_NOT_FOUND;
        
        if (it->second.access == ParameterInfo::ACCESS_READ_ONLY) {
            return Result::ERROR_INVALID_PARAMETER;
        }
        
        if (value < it->second.minValue || value > it->second.maxValue) {
            return Result::ERROR_INVALID_PARAMETER;
        }
        
        it->second.value = value;
        return Result::SUCCESS;
    }
    
    Result getParameter(const std::string& name, float& value) {
        if (!initialized) return Result::ERROR_NOT_INITIALIZED;
        
        auto it = parameters.find(name);
        if (it == parameters.end()) return Result::ERROR_NOT_FOUND;
        
        value = it->second.value;
        return Result::SUCCESS;
    }
    
    Result saveToNVS() {
        if (!initialized || !nvs) return Result::ERROR_NOT_INITIALIZED;
        
        // Save all parameters
        for (const auto& param : parameters) {
            if (!nvs->write(param.first, &param.second.value, sizeof(float))) {
                return Result::ERROR_WRITE_FAILED;
            }
        }
        
        // Save system settings structure
        if (!nvs->write("system_settings", settings, sizeof(SystemSettings))) {
            return Result::ERROR_WRITE_FAILED;
        }
        
        return Result::SUCCESS;
    }
    
    Result loadFromNVS() {
        if (!initialized || !nvs) return Result::ERROR_NOT_INITIALIZED;
        
        // Load all parameters
        for (auto& param : parameters) {
            float value;
            if (nvs->read(param.first, &value, sizeof(float))) {
                param.second.value = value;
            }
        }
        
        // Load system settings structure
        // Note: We copy field-by-field since SystemSettings has no default assignment operator
        SystemSettings loadedSettings;
        if (nvs->read("system_settings", &loadedSettings, sizeof(SystemSettings))) {
            // Copy key fields manually
            settings->targetTemperatureInside = loadedSettings.targetTemperatureInside;
            settings->heating_hysteresis = loadedSettings.heating_hysteresis;
            settings->heatingEnabled = loadedSettings.heatingEnabled;
            settings->waterEnabled = loadedSettings.waterEnabled;
            settings->wheaterPriorityEnabled = loadedSettings.wheaterPriorityEnabled;
            settings->wHeaterConfTempLimitLow = loadedSettings.wHeaterConfTempLimitLow;
            settings->wHeaterConfTempLimitHigh = loadedSettings.wHeaterConfTempLimitHigh;
            settings->spaceHeatingKp = loadedSettings.spaceHeatingKp;
            settings->spaceHeatingKi = loadedSettings.spaceHeatingKi;
            settings->spaceHeatingKd = loadedSettings.spaceHeatingKd;
        }
        
        return Result::SUCCESS;
    }
    
    std::vector<std::string> listParameters() const {
        std::vector<std::string> names;
        for (const auto& param : parameters) {
            names.push_back(param.first);
        }
        return names;
    }
    
    size_t getParameterCount() const {
        return parameters.size();
    }
};

// Test fixtures
static MockNVS* mockNVS = nullptr;
static MockPersistentStorage* storage = nullptr;
static SystemSettings* settings = nullptr;

void setupPersistentStorage() {
    settings = new SystemSettings();
    mockNVS = new MockNVS();
    storage = new MockPersistentStorage(settings);
}

void tearDownPersistentStorage() {
    delete storage;
    delete mockNVS;
    delete settings;
}

// Test basic parameter registration and retrieval
void test_parameter_registration() {
    setupPersistentStorage();
    
    // Initialize storage
    auto result = storage->initialize(mockNVS);
    TEST_ASSERT_EQUAL(MockPersistentStorage::Result::SUCCESS, result);
    
    // Register parameters
    float targetTemp = 21.0f;
    result = storage->registerParameter("heating/target", &targetTemp, 
                                      10.0f, 30.0f, "Target temperature");
    TEST_ASSERT_EQUAL(MockPersistentStorage::Result::SUCCESS, result);
    
    // Get parameter
    float retrievedValue;
    result = storage->getParameter("heating/target", retrievedValue);
    TEST_ASSERT_EQUAL(MockPersistentStorage::Result::SUCCESS, result);
    TEST_ASSERT_EQUAL_FLOAT(21.0f, retrievedValue);
    
    tearDownPersistentStorage();
}

// Test parameter validation
void test_parameter_validation() {
    setupPersistentStorage();
    
    storage->initialize(mockNVS);
    
    float testValue = 25.0f;
    storage->registerParameter("test/value", &testValue, 20.0f, 30.0f);
    
    // Test valid value
    auto result = storage->setParameter("test/value", 25.0f);
    TEST_ASSERT_EQUAL(MockPersistentStorage::Result::SUCCESS, result);
    
    // Test below minimum
    result = storage->setParameter("test/value", 15.0f);
    TEST_ASSERT_EQUAL(MockPersistentStorage::Result::ERROR_INVALID_PARAMETER, result);
    
    // Test above maximum
    result = storage->setParameter("test/value", 35.0f);
    TEST_ASSERT_EQUAL(MockPersistentStorage::Result::ERROR_INVALID_PARAMETER, result);
    
    tearDownPersistentStorage();
}

// Test save and load functionality
void test_save_and_load() {
    setupPersistentStorage();
    
    storage->initialize(mockNVS);
    
    // Set up parameters
    settings->targetTemperatureInside = tempFromFloat(22.5f);
    settings->heating_hysteresis = tempFromFloat(2.0f);
    settings->wHeaterConfTempLimitLow = tempFromFloat(45.0f);
    settings->wHeaterConfTempLimitHigh = tempFromFloat(60.0f);
    
    // Save to NVS
    auto result = storage->saveToNVS();
    TEST_ASSERT_EQUAL(MockPersistentStorage::Result::SUCCESS, result);
    
    // Modify values
    settings->targetTemperatureInside = tempFromFloat(19.0f);
    settings->wHeaterConfTempLimitHigh = tempFromFloat(55.0f);
    
    // Load from NVS
    result = storage->loadFromNVS();
    TEST_ASSERT_EQUAL(MockPersistentStorage::Result::SUCCESS, result);
    
    // Verify values restored
    TEST_ASSERT_EQUAL_INT16(tempFromFloat(22.5f), settings->targetTemperatureInside);
    TEST_ASSERT_EQUAL_INT16(tempFromFloat(60.0f), settings->wHeaterConfTempLimitHigh);
    
    tearDownPersistentStorage();
}

// Test parameter listing
void test_parameter_listing() {
    setupPersistentStorage();
    
    storage->initialize(mockNVS);
    
    // Register multiple parameters
    float temp1 = 21.0f, temp2 = 45.0f, temp3 = 2.0f;
    storage->registerParameter("heating/target", &temp1, 10.0f, 30.0f);
    storage->registerParameter("water/lowLimit", &temp2, 30.0f, 60.0f);
    storage->registerParameter("heating/hysteresis", &temp3, 0.5f, 5.0f);
    
    // List parameters
    auto params = storage->listParameters();
    TEST_ASSERT_EQUAL(3, params.size());
    
    // Check count
    TEST_ASSERT_EQUAL(3, storage->getParameterCount());
    
    tearDownPersistentStorage();
}

// Test read-only parameters
void test_readonly_parameters() {
    setupPersistentStorage();
    
    storage->initialize(mockNVS);
    
    // Register read-only parameter
    float version = 1.0f;
    storage->registerParameter("system/version", &version, 0.0f, 100.0f,
                             "System version", 
                             MockPersistentStorage::ParameterInfo::ACCESS_READ_ONLY);
    
    // Try to modify read-only parameter
    auto result = storage->setParameter("system/version", 2.0f);
    TEST_ASSERT_EQUAL(MockPersistentStorage::Result::ERROR_INVALID_PARAMETER, result);
    
    // Verify value unchanged
    float retrievedValue;
    storage->getParameter("system/version", retrievedValue);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, retrievedValue);
    
    tearDownPersistentStorage();
}

// Test persistence across restarts
void test_persistence_across_restarts() {
    setupPersistentStorage();
    
    // First session
    storage->initialize(mockNVS);
    
    settings->targetTemperatureInside = tempFromFloat(23.0f);
    settings->spaceHeatingKp = 2.5f;
    settings->spaceHeatingKi = 0.15f;
    
    storage->saveToNVS();
    
    // Simulate restart - delete and recreate storage
    delete storage;
    delete settings;
    
    settings = new SystemSettings();  // Will have default values
    storage = new MockPersistentStorage(settings);
    
    // Second session
    storage->initialize(mockNVS);
    storage->loadFromNVS();
    
    // Verify persistence
    TEST_ASSERT_EQUAL_INT16(tempFromFloat(23.0f), settings->targetTemperatureInside);
    TEST_ASSERT_EQUAL_FLOAT(2.5f, settings->spaceHeatingKp);
    TEST_ASSERT_EQUAL_FLOAT(0.15f, settings->spaceHeatingKi);
    
    tearDownPersistentStorage();
}

// Test error handling
void test_error_handling() {
    setupPersistentStorage();
    
    // Test operations before initialization
    float value;
    auto result = storage->getParameter("test", value);
    TEST_ASSERT_EQUAL(MockPersistentStorage::Result::ERROR_NOT_INITIALIZED, result);
    
    // Initialize
    storage->initialize(mockNVS);
    
    // Test non-existent parameter
    result = storage->getParameter("nonexistent", value);
    TEST_ASSERT_EQUAL(MockPersistentStorage::Result::ERROR_NOT_FOUND, result);
    
    // Test setting non-existent parameter
    result = storage->setParameter("nonexistent", 10.0f);
    TEST_ASSERT_EQUAL(MockPersistentStorage::Result::ERROR_NOT_FOUND, result);
    
    tearDownPersistentStorage();
}

// Test NVS space usage
void test_nvs_space_usage() {
    setupPersistentStorage();
    
    storage->initialize(mockNVS);
    
    // Initial state
    TEST_ASSERT_EQUAL(0, mockNVS->getUsedEntries());
    
    // Register and save parameters
    float values[10];
    for (int i = 0; i < 10; i++) {
        values[i] = i * 10.0f;
        char name[32];
        snprintf(name, sizeof(name), "param%d", i);
        storage->registerParameter(name, &values[i], 0.0f, 100.0f);
    }
    
    storage->saveToNVS();
    
    // Check space usage (10 params + 1 settings struct)
    TEST_ASSERT_EQUAL(11, mockNVS->getUsedEntries());
    
    tearDownPersistentStorage();
}

// main() is in test_main.cpp