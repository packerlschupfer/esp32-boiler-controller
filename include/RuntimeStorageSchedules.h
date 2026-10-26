// RuntimeStorageSchedules.h
// Extension for RuntimeStorage to handle TimerScheduler schedules
#pragma once

#include <Arduino.h>
#include "TimerSchedule.h"
#include <RuntimeStorage.h>
#include "utils/CriticalDataStorage.h"  // F7: for ADDR_CRITICAL_END region guard

// Use separate namespace to avoid conflict with rtstorage::RuntimeStorage class
namespace schedules {

// Schedule storage configuration
// F7 (CRITICAL): ScheduleStorage previously started at 0x4C20 - the SAME address
// as CriticalDataStorage's critical region (0x4C20..0x6120). The two drivers
// mutually corrupted each other: any emergency save wiped the schedule header
// (erasing all schedules and the emergency forensic record), and counter/log
// saves clobbered individual schedules. Relocated to 0x6200, immediately after
// the critical region, with a compile-time overlap guard below. NOTE: this is a
// one-time layout change - existing schedules in the old (corrupted) region are
// NOT migrated (they were already being erased after every emergency); schedules
// must be re-added once after this update.
const uint16_t ADDR_SCHEDULES = 0x6200;  // After CriticalDataStorage (ends 0x6120)
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

// F7: compile-time FRAM region map guard. If any of these fire, two drivers
// claim overlapping FRAM and would silently corrupt each other at runtime.
namespace {
    constexpr uint32_t kScheduleRegionBytes =
        sizeof(ScheduleStorageHeader) + static_cast<uint32_t>(MAX_SCHEDULES) * sizeof(StoredSchedule);
    // Must start at/after the end of CriticalDataStorage's critical region.
    static_assert(ADDR_SCHEDULES >= CriticalDataStorage::ADDR_CRITICAL_END,
                  "FRAM schedule region overlaps CriticalDataStorage critical region");
    // Must fit within the actual usage window we advertise...
    static_assert(kScheduleRegionBytes <= SIZE_SCHEDULES,
                  "Schedule header + MAX_SCHEDULES exceeds SIZE_SCHEDULES");
    // ...and within the 32KB MB85RC256V address space.
    static_assert(static_cast<uint32_t>(ADDR_SCHEDULES) + kScheduleRegionBytes <= 0x8000u,
                  "FRAM schedule region exceeds 32KB device address space");
}

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