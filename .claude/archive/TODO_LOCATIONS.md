# TODO Locations for Library API Updates

## Summary of Integration Points

This document lists all the TODO markers in the codebase that need updating once the libraries support Result<T>.

## 1. main.cpp (2 remaining TODOs for MB8ART/RYN4)

### MB8ART Integration - Line 304
```cpp
// TODO: When MB8ART API returns Result<T>, update this:
// auto initResult = MB8ART1->initialize();
// if (initResult.isError()) {
//     ErrorHandler::logError(LOG_TAG_MAIN, initResult.error(), "MB8ART initialization failed");
//     unregisterModbusDevice(MB8ART_ADDRESS);
//     delete MB8ART1;
//     MB8ART1 = nullptr;
// } else {
MB8ART1->initialize();
```

### MB8ART Wait - Line 315
```cpp
// TODO: When waitForInitializationComplete returns Result<T>:
// auto waitResult = MB8ART1->waitForInitializationComplete(pdMS_TO_TICKS(10000));
// if (waitResult.isError()) {
//     ErrorHandler::logError(LOG_TAG_MAIN, waitResult.error(), "MB8ART initialization wait failed");
if (!MB8ART1->waitForInitializationComplete(pdMS_TO_TICKS(10000))) {
```

### RYN4 Integration - Line 393
```cpp
// TODO: When RYN4 API returns Result<T>, update this:
// auto initResult = RYN4_1->initialize();
// if (initResult.isError()) {
//     ErrorHandler::logError(LOG_TAG_MAIN, initResult.error(), "RYN4 initialization failed");
//     unregisterModbusDevice(RYN4_ADDRESS);
//     delete RYN4_1;
//     RYN4_1 = nullptr;
// } else {
```

### RYN4 Wait - Line 404
```cpp
// TODO: When waitForInitializationComplete returns Result<T>:
// auto waitResult = RYN4_1->waitForInitializationComplete(pdMS_TO_TICKS(10000));
// if (waitResult.isError()) {
//     ErrorHandler::logError(LOG_TAG_MAIN, waitResult.error(), "RYN4 initialization wait failed");
if (!RYN4_1->waitForInitializationComplete(pdMS_TO_TICKS(10000))) {
```

## 2. MQTTTask.cpp (8 TODOs)

Line numbers and patterns:
- Connection handling
- Publishing operations  
- Subscription operations
- Error recovery

Each TODO includes the pattern to follow once MQTTManager returns Result<T>.

## 3. OTATask.cpp (3 TODOs)

- OTA initialization
- Update handling
- Status checking

## 4. RelayControlTask.cpp (4 TODOs)

- Relay control operations
- Status reading
- Batch operations
- Error handling

## 5. MB8ARTTasks.cpp (1 TODO)

- Temperature data requests

## Integration Pattern

All TODOs follow this general pattern:

```cpp
// Old API (bool return):
if (!operation()) {
    LOG_ERROR(TAG, "Operation failed");
    // handle error
}

// New API (Result<T> return):
auto result = operation();
if (!result.isOk()) {
    SystemError sysError = LibraryErrorMapper::mapXXXError(result.error);
    ErrorHandler::logError(TAG, sysError, "Operation failed");
    // handle error with specific error code
}
```

## Benefits of Migration

1. **Specific Error Information**: Know exactly why operations fail
2. **Better Diagnostics**: Error codes can be reported via MQTT
3. **Consistent Error Handling**: All libraries use the same pattern
4. **Improved Debugging**: Error codes in logs make troubleshooting easier
5. **Predictive Maintenance**: Track error patterns over time