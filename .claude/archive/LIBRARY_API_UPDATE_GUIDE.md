# Library API Update Guide for Result<T> Error Handling

## Overview
This guide provides instructions for updating MB8ART, RYN4, and MQTTManager libraries to use Result<T> error handling pattern, matching the EthernetManager implementation.

## 1. MB8ART Library Updates

### Current State
- Already has `mb8art::SensorErrorCode` enum
- Methods that need updating:
  - `bool initialize()` 
  - `bool waitForInitializationComplete(TickType_t timeout)`
  - `bool requestData()`
  - `bool reqTemperatures(...)`
  - `bool requestAllData()`

### Required Changes

#### A. Add Result Template to MB8ART.h
```cpp
// Add after the SensorErrorCode enum definition
template<typename T>
struct SensorResult {
    SensorErrorCode error;
    T value;
    
    SensorResult(SensorErrorCode err = SensorErrorCode::SUCCESS, T val = T()) 
        : error(err), value(val) {}
    bool isOk() const { return error == SensorErrorCode::SUCCESS; }
    operator bool() const { return isOk(); }
};

// Specialization for void
template<>
struct SensorResult<void> {
    SensorErrorCode error;
    
    SensorResult(SensorErrorCode err = SensorErrorCode::SUCCESS) : error(err) {}
    bool isOk() const { return error == SensorErrorCode::SUCCESS; }
    operator bool() const { return isOk(); }
};
```

#### B. Update Method Signatures in MB8ART.h
```cpp
// Change from:
bool initialize();
bool waitForInitializationComplete(TickType_t timeout);
bool requestData();
bool reqTemperatures(std::vector<std::optional<float>>& temperatures);
bool requestAllData();

// To:
SensorResult<void> initialize();
SensorResult<void> waitForInitializationComplete(TickType_t timeout);
SensorResult<void> requestData();
SensorResult<void> reqTemperatures(std::vector<std::optional<float>>& temperatures);
SensorResult<void> requestAllData();
```

#### C. Update Method Implementations
Example for initialize():
```cpp
SensorResult<void> MB8ART::initialize() {
    if (initialized) {
        return SensorResult<void>(SensorErrorCode::SUCCESS);
    }
    
    // ... existing initialization code ...
    
    if (/* error condition */) {
        return SensorResult<void>(SensorErrorCode::CONFIG_ERROR);
    }
    
    initialized = true;
    return SensorResult<void>(SensorErrorCode::SUCCESS);
}
```

### Integration Points to Update (in main project)
- main.cpp: lines 304-328 (MB8ART initialization)
- MB8ARTTasks.cpp: line with TODO marker

## 2. RYN4 Library Updates

### Current State
- Already has `ryn4::RelayErrorCode` enum
- Methods that need updating:
  - `bool isInitialized()`
  - `bool waitForInitializationComplete(TickType_t timeout)`
  - `bool requestData()`

### Required Changes

#### A. Add Result Template to RYN4.h
```cpp
// Add in ryn4 namespace after RelayErrorCode enum
template<typename T>
struct RelayResult {
    RelayErrorCode error;
    T value;
    
    RelayResult(RelayErrorCode err = RelayErrorCode::SUCCESS, T val = T()) 
        : error(err), value(val) {}
    bool isOk() const { return error == RelayErrorCode::SUCCESS; }
    operator bool() const { return isOk(); }
};

// Specialization for void
template<>
struct RelayResult<void> {
    RelayErrorCode error;
    
    RelayResult(RelayErrorCode err = RelayErrorCode::SUCCESS) : error(err) {}
    bool isOk() const { return error == RelayErrorCode::SUCCESS; }
    operator bool() const { return isOk(); }
};
```

#### B. Update Method Signatures
```cpp
// Change from:
bool waitForInitializationComplete(TickType_t timeout);
bool requestData();

// To:
RelayResult<void> waitForInitializationComplete(TickType_t timeout);
RelayResult<void> requestData();

// Note: isInitialized() can stay as bool since it's a simple state check
```

### Integration Points to Update
- main.cpp: lines 393-428 (RYN4 initialization)
- RelayControlTask.cpp: 4 locations with TODO markers

## 3. MQTTManager Library Updates

### Current State
- All methods return bool
- No existing error enum

### Required Changes

#### A. Create Error Enum and Result Template
```cpp
// Add to MQTTManager.h
enum class MQTTError {
    OK = 0,
    NOT_INITIALIZED,
    ALREADY_CONNECTED,
    CONNECTION_FAILED,
    BROKER_UNREACHABLE,
    PUBLISH_FAILED,
    SUBSCRIBE_FAILED,
    INVALID_PARAMETER,
    MEMORY_ALLOCATION_FAILED,
    TIMEOUT,
    UNKNOWN_ERROR
};

template<typename T>
struct MQTTResult {
    MQTTError error;
    T value;
    
    MQTTResult(MQTTError err = MQTTError::OK, T val = T()) 
        : error(err), value(val) {}
    bool isOk() const { return error == MQTTError::OK; }
    operator bool() const { return isOk(); }
};

template<>
struct MQTTResult<void> {
    MQTTError error;
    
    MQTTResult(MQTTError err = MQTTError::OK) : error(err) {}
    bool isOk() const { return error == MQTTError::OK; }
    operator bool() const { return isOk(); }
};
```

#### B. Update Method Signatures
```cpp
// Change from:
bool begin(...);
bool connect();
bool publish(const char* topic, const char* payload, int qos = 0, bool retain = false);
bool subscribe(const char* topic, int qos = 0);
bool isConnected();

// To:
MQTTResult<void> begin(...);
MQTTResult<void> connect();
MQTTResult<void> publish(const char* topic, const char* payload, int qos = 0, bool retain = false);
MQTTResult<void> subscribe(const char* topic, int qos = 0);
bool isConnected();  // Can stay as bool for simple state check
```

### Integration Points to Update
- MQTTTask.cpp: 8 locations with TODO markers

## 4. LibraryErrorMapper.h Updates

After updating the libraries, update the error mapper:

### For MQTTManager
```cpp
static SystemError mapMQTTError(MQTTError mqttError) {
    switch (mqttError) {
        case MQTTError::OK:
            return SystemError::SUCCESS;
        case MQTTError::NOT_INITIALIZED:
            return SystemError::NOT_INITIALIZED;
        case MQTTError::CONNECTION_FAILED:
        case MQTTError::BROKER_UNREACHABLE:
            return SystemError::MQTT_BROKER_UNREACHABLE;
        case MQTTError::PUBLISH_FAILED:
            return SystemError::MQTT_PUBLISH_FAILED;
        case MQTTError::SUBSCRIBE_FAILED:
            return SystemError::MQTT_SUBSCRIBE_FAILED;
        case MQTTError::INVALID_PARAMETER:
            return SystemError::INVALID_PARAMETER;
        case MQTTError::MEMORY_ALLOCATION_FAILED:
            return SystemError::MEMORY_ALLOCATION_FAILED;
        case MQTTError::TIMEOUT:
            return SystemError::TIMEOUT;
        default:
            return SystemError::UNKNOWN_ERROR;
    }
}

static Result<void> convertMQTTResult(const MQTTResult<void>& mqttResult) {
    if (mqttResult.isOk()) {
        return Result<void>();
    }
    SystemError sysError = mapMQTTError(mqttResult.error);
    return Result<void>(sysError, ErrorHandler::errorToString(sysError));
}
```

## 5. Testing Pattern

After updating each library:

1. Compile the library standalone to ensure no syntax errors
2. Update the integration points in the main project
3. Compile the main project
4. Test error propagation by simulating failures

## 6. Example Integration Update

When updating integration points, follow this pattern:

```cpp
// Old code:
if (!device->initialize()) {
    LOG_ERROR(TAG, "Initialization failed");
    return false;
}

// New code:
auto result = device->initialize();
if (!result.isOk()) {
    SystemError sysError = LibraryErrorMapper::mapMB8ARTError(result.error);
    ErrorHandler::logError(TAG, sysError, "Initialization failed");
    return false;
}
```

## Notes
- Keep error enums compatible with existing code where possible
- Methods that are simple state checks (like isConnected()) can remain as bool
- Ensure all error conditions return appropriate error codes
- Update both header declarations and cpp implementations
- Don't forget to update the namespace-specific types (mb8art::, ryn4::, etc.)