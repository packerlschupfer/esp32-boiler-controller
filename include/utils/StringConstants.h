#ifndef STRING_CONSTANTS_H
#define STRING_CONSTANTS_H

#include <pgmspace.h>

// Common string constants stored in flash memory to save RAM
namespace StringConstants {
    // Status strings
    static const char STR_ON[] PROGMEM = "ON";
    static const char STR_OFF[] PROGMEM = "OFF";
    static const char STR_ENABLED[] PROGMEM = "ENABLED";
    static const char STR_DISABLED[] PROGMEM = "DISABLED";
    static const char STR_TRUE[] PROGMEM = "true";
    static const char STR_FALSE[] PROGMEM = "false";
    static const char STR_OK[] PROGMEM = "OK";
    static const char STR_ERROR[] PROGMEM = "ERROR";
    static const char STR_UNKNOWN[] PROGMEM = "unknown";
    
    // Common error messages
    static const char STR_MUTEX_FAIL[] PROGMEM = "Failed to acquire mutex";
    static const char STR_WATCHDOG_FAIL[] PROGMEM = "Failed to register with watchdog";
    static const char STR_MEMORY_LOW[] PROGMEM = "Low memory";
    static const char STR_SENSOR_FAIL[] PROGMEM = "Sensor failure";
    
    // System states
    static const char STR_STARTING[] PROGMEM = "Starting";
    static const char STR_RUNNING[] PROGMEM = "Running";
    static const char STR_STOPPING[] PROGMEM = "Stopping";
    static const char STR_STOPPED[] PROGMEM = "Stopped";
    
    // Helper macro for using PROGMEM strings with ESP_LOG
    #define PSTR_LOG(str) (const char*)(str)
}

#endif // STRING_CONSTANTS_H