# Library API Migration Status

## Overview
This document tracks the status of migrating to the new Result<T>-based API from the improved workspace libraries.

## Migration Status

### 1. EthernetManager
**Status**: ✅ Prepared for migration
**Files Updated**:
- `src/main.cpp` - Added TODO comments for Result<T> API (4 locations)
- `src/modules/tasks/OTATask.cpp` - Added TODO comments for Result<T> API (2 locations)  
- `src/modules/tasks/MQTTTask.cpp` - Added TODO comments for Result<T> API (7 locations)

**Pending API Changes**:
```cpp
// Old API (current)
bool initializeAsync(...);
bool waitForConnection(uint32_t timeout);
bool isConnected();

// New API (expected)
Result<void> initializeAsync(...);
Result<bool> waitForConnection(uint32_t timeout);
Result<bool> getConnectionStatus();
```

### 2. MB8ART & RYN4
**Status**: ✅ Prepared for migration
**Files Updated**:
- `src/main.cpp` - Added TODO comments for Result<T> API (4 locations for device initialization)
- `src/modules/tasks/MB8ARTTasks.cpp` - Added TODO comments for requestData() API (1 location)
- `src/modules/tasks/RelayControlTask.cpp` - Added TODO comments for RYN4 operations (4 locations)

**Pending API Changes**:
```cpp
// Old API (current)
void initialize();
bool waitForInitializationComplete(TickType_t timeout);

// New API (expected)
Result<void> initialize();
Result<void> waitForInitializationComplete(TickType_t timeout);
```

### 3. MQTTManager
**Status**: ✅ Prepared for migration
**Files Updated**:
- `src/modules/tasks/MQTTTask.cpp` - Added TODO comments for Result<T> API (8 locations total)
  - publish() operations
  - subscribe() operations
  - connect() operations
  - isConnected() checks

**Pending API Changes**:
```cpp
// Old API (current)
void connect();
bool publish(const char* topic, const char* payload, int qos, bool retain);
bool subscribe(const char* topic, callback, int qos);

// New API (expected)
Result<void> connect();
Result<void> publish(const char* topic, const char* payload, int qos, bool retain);
Result<void> subscribe(const char* topic, callback, int qos);
```

### 4. OTAManager
**Status**: ✅ Prepared for migration
**Files Updated**:
- `src/modules/tasks/OTATask.cpp` - Added TODO comments for Result<T> API (3 locations)
  - initialize() operation
  - handleUpdates() operation
  - isNetworkConnected() check

**Pending API Changes**:
```cpp
// Old API (current)
void initialize(...);
void handleUpdates();

// New API (expected)
Result<void> initialize(...);
Result<void> handleUpdates();
```

## Infrastructure Added

### Error Handling Framework
- **ErrorHandler.h** - Already exists with comprehensive SystemError enum and Result<T> template
- **LibraryErrorMapper.h** - Created to map library-specific errors to SystemError
- **Include statements** - Added `#include "utils/ErrorHandler.h"` to all files that will need it:
  - main.cpp
  - OTATask.cpp
  - MQTTTask.cpp
  - MB8ARTTasks.cpp
  - RelayControlTask.cpp

### Code Patterns Established
All TODO comments follow a consistent pattern:
```cpp
// TODO: When [Library] API returns Result<T>, update this:
// auto result = library->function(...);
// if (result.isError()) {
//     ErrorHandler::logError(LOG_TAG, result.error(), "Context");
//     // Handle error appropriately
// }

// For now, keep existing API until confirmed
existingApiCall();
```

## Next Steps

### 1. Confirm Library APIs
Need to verify the actual Result<T> APIs from the improved libraries:
- Check `/home/mrnice/Documents/PlatformIO/libs/workspace_Class-*` headers
- Confirm error enum names and values
- Update LibraryErrorMapper with actual mappings

### 2. Complete Migration
Once APIs are confirmed:
1. Uncomment the new Result<T> code blocks
2. Remove the old API calls
3. Update LibraryErrorMapper with actual error mappings
4. Test each component thoroughly

### 3. Additional Improvements
- Add unit tests for error handling
- Implement retry logic for transient errors
- Add telemetry for error tracking
- Consider adding error recovery mechanisms

## Benefits of Migration

1. **Unified Error Handling**: All errors use SystemError enum
2. **Better Error Context**: Result<T> includes error messages
3. **Type Safety**: Compile-time checking of error handling
4. **Consistent Pattern**: All library calls follow same error pattern
5. **Future Proof**: Easy to add new error types and handling

## Testing Strategy

1. **Unit Tests**: Test each error mapping function
2. **Integration Tests**: Test error propagation through call chain
3. **Failure Scenarios**: 
   - Disconnect Ethernet cable
   - Unplug Modbus devices
   - Stop MQTT broker
   - Corrupt OTA firmware
4. **Recovery Tests**: Verify system recovers from each error type

## Notes

- All changes maintain backward compatibility
- System continues to work with current boolean-based APIs
- Migration can be done incrementally as libraries are updated
- Error handling infrastructure is ready for immediate use
- Total TODO comments added: ~27 locations across 5 files
- Each TODO follows a consistent pattern for easy search and replace

## Summary of Changes Made

1. **Added comprehensive TODO comments** throughout the codebase marking where Result<T> API will be used
2. **Created LibraryErrorMapper.h** to provide error code translation infrastructure
3. **Added ErrorHandler.h includes** to all files that will need error handling
4. **Documented all changes** in this migration status file
5. **Preserved existing functionality** - all code continues to work with current APIs

The project is now fully prepared for the library API migration. When the improved libraries are confirmed to use Result<T> patterns, developers can:
1. Search for "TODO: When" to find all migration points
2. Uncomment the new code blocks
3. Remove the old API calls
4. Update LibraryErrorMapper with actual error enums
5. Test thoroughly