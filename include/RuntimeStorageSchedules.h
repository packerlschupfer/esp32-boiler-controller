// RuntimeStorageSchedules.h
// Extension for RuntimeStorage to handle TimerScheduler schedules
#pragma once

#include <Arduino.h>
#include "TimerSchedule.h"
#include <RuntimeStorage.h>

// Use separate namespace to avoid conflict with rtstorage::RuntimeStorage class
namespace schedules {

// Schedule storage configuration
const uint16_t ADDR_SCHEDULES = 0x4C20;  // Start of reserved area
const uint16_t SIZE_SCHEDULES = 4096;    // 4KB for schedules
const uint8_t MAX_SCHEDULES = 20;        // Maximum number of schedules

// Schedule storage header
struct ScheduleStorageHeader {
    uint32_t magic;          // 0x53434844 ('SCHD')
    uint8_t version;         // Storage format version
    uint8_t count;           // Number of stored schedules
    uint16_t reserved;       // Reserved for future use
    uint32_t crc;           // CRC of header
} __attribute__((packed));

// Stored schedule format (fixed size for FRAM)
struct StoredSchedule {
    uint8_t id;
    uint8_t type;           // ScheduleType
    uint8_t dayMask;
    uint8_t startHour;
    uint8_t startMinute;
    uint8_t endHour;
    uint8_t endMinute;
    uint8_t enabled;
    char name[32];          // Fixed size name
    uint8_t actionData[16]; // Action-specific data
    uint32_t crc;          // CRC of this schedule
} __attribute__((packed));

// Constants
const uint32_t SCHEDULE_MAGIC = 0x53434844;  // 'SCHD'
const uint8_t SCHEDULE_VERSION = 1;

// Extension class for schedule storage
class ScheduleStorage {
public:
    ScheduleStorage(rtstorage::RuntimeStorage& storage) : _storage(storage) {}
    
    // Initialize schedule storage area
    bool initializeScheduleStorage();
    
    // Save all schedules
    bool saveSchedules(const std::vector<TimerSchedule>& schedules);
    
    // Load all schedules
    bool loadSchedules(std::vector<TimerSchedule>& schedules);
    
    // Clear all schedules
    bool clearSchedules();
    
    // Get schedule count
    uint8_t getScheduleCount();
    
private:
    rtstorage::RuntimeStorage& _storage;
    
    // Helper methods
    bool writeScheduleHeader(const ScheduleStorageHeader& header);
    bool readScheduleHeader(ScheduleStorageHeader& header);
    bool writeSchedule(uint8_t index, const StoredSchedule& schedule);
    bool readSchedule(uint8_t index, StoredSchedule& schedule);
    uint32_t calculateCRC(const uint8_t* data, size_t length);
    
    // Convert between TimerSchedule and StoredSchedule
    void packSchedule(const TimerSchedule& src, StoredSchedule& dest);
    // Round 15 Issue #17: Returns false if schedule type is invalid
    bool unpackSchedule(const StoredSchedule& src, TimerSchedule& dest);
};

} // namespace schedules