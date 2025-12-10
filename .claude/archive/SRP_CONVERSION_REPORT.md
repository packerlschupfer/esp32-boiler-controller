# SRP (SystemResourceProvider) Conversion Report

## Overview
This report identifies files that are still using direct global variables instead of the SystemResourceProvider (SRP) pattern and outlines the changes needed to complete the conversion.

## Files Requiring Conversion

### 1. **src/modules/tasks/SensorTask.cpp**
**Status:** ❌ Not converted
**Direct Global Usage:**
- `extern SharedSensorReadings sharedSensorReadings;`
- `extern SemaphoreHandle_t xSharedSensorReadingsMutex;`
- `extern EventGroupHandle_t xSensorEventGroup;`
- `extern MB8ART* MB8ART1;`
- `extern ATC_MiThermometer miTh;`

**Required Changes:**
```cpp
// Replace extern declarations with SRP includes
#include "core/SystemResourceProvider.h"

// Replace direct mutex access:
// OLD: xSemaphoreTake(xSharedSensorReadingsMutex, portMAX_DELAY)
// NEW: xSemaphoreTake(SRP::getSensorReadingsMutex(), portMAX_DELAY)

// Replace direct event group access:
// OLD: xEventGroupSetBits(xSensorEventGroup, SENSOR_DATA_ERROR_BIT)
// NEW: xEventGroupSetBits(SRP::getSensorEventGroup(), SENSOR_DATA_ERROR_BIT)

// Replace direct MB8ART access:
// OLD: MB8ART1->requestData()
// NEW: SRP::getMB8ART()->requestData()
```

### 2. **src/modules/tasks/MQTTTask.cpp**
**Status:** ❌ Not converted
**Direct Global Usage:**
- `extern SharedSensorReadings sharedSensorReadings;`
- `extern SemaphoreHandle_t xSharedSensorReadingsMutex;`
- `extern EventGroupHandle_t xSensorEventGroup;`

**Required Changes:**
```cpp
// Replace direct mutex access:
// OLD: xSemaphoreTake(xSharedSensorReadingsMutex, pdMS_TO_TICKS(100))
// NEW: xSemaphoreTake(SRP::getSensorReadingsMutex(), pdMS_TO_TICKS(100))

// Replace direct event group access:
// OLD: xEventGroupWaitBits(xSensorEventGroup, ...)
// NEW: xEventGroupWaitBits(SRP::getSensorEventGroup(), ...)
```

### 3. **src/modules/tasks/MiThermometerSensorTask.cpp**
**Status:** ❌ Not converted
**Direct Global Usage:**
- `extern SharedSensorReadings sharedSensorReadings;`
- `extern SemaphoreHandle_t xSharedSensorReadingsMutex;`
- `extern EventGroupHandle_t xSensorEventGroup;`

**Required Changes:**
```cpp
// Replace direct mutex access:
// OLD: xSemaphoreTake(xSharedSensorReadingsMutex, pdMS_TO_TICKS(100))
// NEW: xSemaphoreTake(SRP::getSensorReadingsMutex(), pdMS_TO_TICKS(100))

// Replace direct event group access:
// OLD: xEventGroupSetBits(xSensorEventGroup, INSIDE_TEMP_UPDATE_BIT)
// NEW: xEventGroupSetBits(SRP::getSensorEventGroup(), INSIDE_TEMP_UPDATE_BIT)

// Also update init() method to use SRP:
// OLD: systemEventGroup = eventGroup ? eventGroup : xSensorEventGroup;
// NEW: systemEventGroup = eventGroup ? eventGroup : SRP::getSensorEventGroup();
```

### 4. **src/modules/tasks/WheaterControlTask.cpp**
**Status:** ❌ Not converted
**Direct Global Usage:**
- `extern EventGroupHandle_t xSystemStateEventGroup;`
- `extern EventGroupHandle_t xControlAndRequestsEventGroup;`
- `extern SemaphoreHandle_t xSharedSensorReadingsMutex;`
- `extern SystemSettings currentSettings;`
- `extern SharedSensorReadings sharedSensorReadings;`

**Required Changes:**
```cpp
// Replace direct event group access:
// OLD: xEventGroupGetBits(xSystemStateEventGroup)
// NEW: xEventGroupGetBits(SRP::getSystemStateEventGroup())

// OLD: xEventGroupWaitBits(xControlAndRequestsEventGroup, ...)
// NEW: xEventGroupWaitBits(SRP::getControlRequestsEventGroup(), ...)

// Replace direct mutex access:
// OLD: xSemaphoreTake(xSharedSensorReadingsMutex, portMAX_DELAY)
// NEW: xSemaphoreTake(SRP::getSensorReadingsMutex(), portMAX_DELAY)
```

### 5. **src/modules/tasks/BPumpControlTask.cpp**
**Status:** ❌ Not converted
**Direct Global Usage:**
- `extern EventGroupHandle_t xSystemStateEventGroup;`
- `extern EventGroupHandle_t xControlAndRequestsEventGroup;`
- `extern EventGroupHandle_t xBurnerEventGroup;`
- `extern SemaphoreHandle_t xSharedSensorReadingsMutex;`
- `extern SystemSettings currentSettings;`
- `extern SharedSensorReadings sharedSensorReadings;`

**Required Changes:**
```cpp
// Replace all event group access:
// OLD: xEventGroupGetBits(xSystemStateEventGroup)
// NEW: xEventGroupGetBits(SRP::getSystemStateEventGroup())

// OLD: xEventGroupGetBits(xBurnerEventGroup)
// NEW: xEventGroupGetBits(SRP::getBurnerEventGroup())

// Replace mutex access:
// OLD: xSemaphoreTake(xSharedSensorReadingsMutex, pdMS_TO_TICKS(100))
// NEW: xSemaphoreTake(SRP::getSensorReadingsMutex(), pdMS_TO_TICKS(100))
```

### 6. **src/modules/tasks/HeatingControlTask.cpp**
**Status:** ❌ Not converted
**Direct Global Usage:**
- `extern EventGroupHandle_t xSystemStateEventGroup;`
- `extern EventGroupHandle_t xControlAndRequestsEventGroup;`
- `extern EventGroupHandle_t xHeatingEventGroup;`
- `extern SemaphoreHandle_t xSharedSensorReadingsMutex;`
- `extern SystemSettings currentSettings;`
- `extern SharedSensorReadings sharedSensorReadings;`
- `extern HeatingControlModule* heatingControl;`

**Required Changes:**
```cpp
// Replace event group access:
// OLD: xEventGroupGetBits(xSystemStateEventGroup)
// NEW: xEventGroupGetBits(SRP::getSystemStateEventGroup())

// OLD: xEventGroupSetBits(xHeatingEventGroup, HEATING_EVENT_ON)
// NEW: xEventGroupSetBits(SRP::getHeatingEventGroup(), HEATING_EVENT_ON)

// Replace mutex access:
// OLD: xSemaphoreTake(xSharedSensorReadingsMutex, pdMS_TO_TICKS(100))
// NEW: xSemaphoreTake(SRP::getSensorReadingsMutex(), pdMS_TO_TICKS(100))
```

### 7. **src/modules/tasks/ControlTask.cpp**
**Status:** ❌ Not converted
**Direct Global Usage:**
- `extern EventGroupHandle_t xSystemStateEventGroup;`
- `extern EventGroupHandle_t xControlAndRequestsEventGroup;`

**Required Changes:**
```cpp
// Replace event group access:
// OLD: xEventGroupWaitBits(xControlAndRequestsEventGroup, ...)
// NEW: xEventGroupWaitBits(SRP::getControlRequestsEventGroup(), ...)

// OLD: xEventGroupSetBits(xSystemStateEventGroup, ...)
// NEW: xEventGroupSetBits(SRP::getSystemStateEventGroup(), ...)
```

### 8. **src/diagnostics/MQTTDiagnostics.cpp**
**Status:** ❌ Not converted
**Direct Global Usage:**
- `extern SemaphoreHandle_t xSharedSensorReadingsMutex;`
- `extern SharedSensorReadings sharedSensorReadings;`
- `extern SemaphoreHandle_t xSharedRelayReadingsMutex;`
- `extern SharedRelayReadings sharedRelayReadings;`

**Required Changes:**
```cpp
// Replace mutex access:
// OLD: xSemaphoreTake(xSharedSensorReadingsMutex, pdMS_TO_TICKS(100))
// NEW: xSemaphoreTake(SRP::getSensorReadingsMutex(), pdMS_TO_TICKS(100))

// OLD: xSemaphoreTake(xSharedRelayReadingsMutex, pdMS_TO_TICKS(100))
// NEW: xSemaphoreTake(SRP::getRelayReadingsMutex(), pdMS_TO_TICKS(100))
```

## Conversion Pattern Summary

### Common Replacements:

1. **Event Groups:**
   - `xSystemStateEventGroup` → `SRP::getSystemStateEventGroup()`
   - `xControlAndRequestsEventGroup` → `SRP::getControlRequestsEventGroup()`
   - `xSensorEventGroup` → `SRP::getSensorEventGroup()`
   - `xRelayEventGroup` → `SRP::getRelayEventGroup()`
   - `xBurnerEventGroup` → `SRP::getBurnerEventGroup()`
   - `xHeatingEventGroup` → `SRP::getHeatingEventGroup()`
   - `xWheaterEventGroup` → `SRP::getWheaterEventGroup()`

2. **Mutexes:**
   - `xSharedSensorReadingsMutex` → `SRP::getSensorReadingsMutex()`
   - `xSharedRelayReadingsMutex` → `SRP::getRelayReadingsMutex()`
   - `xSystemSettingsMutex` → `SRP::getSystemSettingsMutex()`
   - `xMQTTMutex` → `SRP::getMQTTMutex()`
   - `xSensorMiThMutex` → `SRP::getSensorMiThMutex()`

3. **Services:**
   - `MB8ART1` → `SRP::getMB8ART()`
   - `RYN4` → `SRP::getRYN4()`
   - Direct MQTT access → `SRP::getMQTTManager()`
   - PID control → `SRP::getPIDControl()`

## Additional Notes

1. **SharedSensorReadings and SharedRelayReadings:** These still need to be accessed directly as they are data structures, not services. However, access to them should always be protected by the appropriate mutex obtained through SRP.

2. **Include Changes:** All converted files need to add:
   ```cpp
   #include "core/SystemResourceProvider.h"
   ```

3. **Remove Extern Declarations:** After conversion, remove all extern declarations for event groups, mutexes, and service pointers.

4. **Testing:** After conversion, thoroughly test each module to ensure:
   - Proper mutex acquisition/release
   - Correct event group behavior
   - No null pointer access
   - Proper initialization order

## Priority Order for Conversion

1. **High Priority** (Core functionality):
   - SensorTask.cpp
   - MQTTTask.cpp
   - ControlTask.cpp

2. **Medium Priority** (Control tasks):
   - HeatingControlTask.cpp
   - WheaterControlTask.cpp
   - BPumpControlTask.cpp

3. **Lower Priority** (Supporting modules):
   - MiThermometerSensorTask.cpp
   - MQTTDiagnostics.cpp

## Benefits of Conversion

1. **Centralized Resource Management:** All system resources accessed through one provider
2. **Easier Testing:** Can mock SRP for unit tests
3. **Better Encapsulation:** No direct global variable access
4. **Improved Maintainability:** Single point of change for resource access
5. **Type Safety:** Compile-time checking of resource types