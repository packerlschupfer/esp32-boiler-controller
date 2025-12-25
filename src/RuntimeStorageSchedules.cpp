// RuntimeStorageSchedules.cpp
// Implementation of schedule storage for RuntimeStorage
#include "RuntimeStorageSchedules.h"
#include "LoggingMacros.h"
#include "IScheduleAction.h"
#include <cstring>

static const char* TAG = "ScheduleStorage";

namespace schedules {

bool ScheduleStorage::initializeScheduleStorage() {
    LOG_INFO(TAG, "Initializing schedule storage area");
    
    ScheduleStorageHeader header;
    
    // Try to read existing header
    if (readScheduleHeader(header)) {
        if (header.magic == SCHEDULE_MAGIC && header.version == SCHEDULE_VERSION) {
            // Calculate expected CRC
            uint32_t expectedCRC = calculateCRC(reinterpret_cast<const uint8_t*>(&header), 
                                               sizeof(header) - sizeof(header.crc));
            if (header.crc == expectedCRC) {
                LOG_INFO(TAG, "Valid schedule storage found with %d schedules", header.count);
                return true;
            }
        }
    }
    
    // Initialize new storage
    LOG_INFO(TAG, "Initializing new schedule storage");
    header.magic = SCHEDULE_MAGIC;
    header.version = SCHEDULE_VERSION;
    header.count = 0;
    header.reserved = 0;
    header.crc = calculateCRC(reinterpret_cast<const uint8_t*>(&header), 
                             sizeof(header) - sizeof(header.crc));
    
    return writeScheduleHeader(header);
}

bool ScheduleStorage::saveSchedules(const std::vector<TimerSchedule>& schedules) {
    if (schedules.size() > MAX_SCHEDULES) {
        LOG_ERROR(TAG, "Too many schedules: %d (max %d)", schedules.size(), MAX_SCHEDULES);
        return false;
    }
    
    LOG_INFO(TAG, "Saving %d schedules to FRAM", schedules.size());
    
    // Update header
    ScheduleStorageHeader header;
    header.magic = SCHEDULE_MAGIC;
    header.version = SCHEDULE_VERSION;
    header.count = schedules.size();
    header.reserved = 0;
    header.crc = calculateCRC(reinterpret_cast<const uint8_t*>(&header), 
                             sizeof(header) - sizeof(header.crc));
    
    if (!writeScheduleHeader(header)) {
        LOG_ERROR(TAG, "Failed to write schedule header");
        return false;
    }
    
    // Save each schedule
    for (size_t i = 0; i < schedules.size(); i++) {
        StoredSchedule stored;
        packSchedule(schedules[i], stored);
        
        if (!writeSchedule(i, stored)) {
            LOG_ERROR(TAG, "Failed to write schedule %d", i);
            return false;
        }
    }
    
    LOG_INFO(TAG, "Successfully saved %d schedules", schedules.size());
    return true;
}

bool ScheduleStorage::loadSchedules(std::vector<TimerSchedule>& schedules) {
    schedules.clear();
    
    // Read header
    ScheduleStorageHeader header;
    if (!readScheduleHeader(header)) {
        LOG_ERROR(TAG, "Failed to read schedule header");
        return false;
    }
    
    // Verify header
    if (header.magic != SCHEDULE_MAGIC) {
        LOG_ERROR(TAG, "Invalid schedule magic: 0x%08X", header.magic);
        return false;
    }
    
    if (header.version != SCHEDULE_VERSION) {
        LOG_ERROR(TAG, "Unsupported schedule version: %d", header.version);
        return false;
    }
    
    // Verify CRC
    uint32_t expectedCRC = calculateCRC(reinterpret_cast<const uint8_t*>(&header), 
                                       sizeof(header) - sizeof(header.crc));
    if (header.crc != expectedCRC) {
        LOG_ERROR(TAG, "Schedule header CRC mismatch");
        return false;
    }
    
    if (header.count > MAX_SCHEDULES) {
        LOG_ERROR(TAG, "Invalid schedule count: %d", header.count);
        return false;
    }
    
    LOG_INFO(TAG, "Loading %d schedules from FRAM", header.count);

    // Round 15 Issue #15 fix: Reserve vector capacity to avoid multiple reallocations
    schedules.reserve(header.count);

    // Load each schedule
    for (uint8_t i = 0; i < header.count; i++) {
        StoredSchedule stored;
        if (!readSchedule(i, stored)) {
            LOG_ERROR(TAG, "Failed to read schedule %d", i);
            continue;
        }

        // Verify schedule CRC
        uint32_t expectedCRC = calculateCRC(reinterpret_cast<const uint8_t*>(&stored),
                                           sizeof(stored) - sizeof(stored.crc));
        if (stored.crc != expectedCRC) {
            LOG_WARN(TAG, "Schedule %d CRC mismatch, skipping", i);
            continue;
        }

        TimerSchedule schedule;
        if (!unpackSchedule(stored, schedule)) {
            LOG_WARN(TAG, "Schedule %d has invalid type, skipping", i);
            continue;
        }
        schedules.push_back(schedule);
    }
    
    LOG_INFO(TAG, "Loaded %d schedules", schedules.size());
    return true;
}

bool ScheduleStorage::clearSchedules() {
    LOG_INFO(TAG, "Clearing all schedules");
    
    ScheduleStorageHeader header;
    header.magic = SCHEDULE_MAGIC;
    header.version = SCHEDULE_VERSION;
    header.count = 0;
    header.reserved = 0;
    header.crc = calculateCRC(reinterpret_cast<const uint8_t*>(&header), 
                             sizeof(header) - sizeof(header.crc));
    
    return writeScheduleHeader(header);
}

uint8_t ScheduleStorage::getScheduleCount() {
    ScheduleStorageHeader header;
    if (readScheduleHeader(header) && 
        header.magic == SCHEDULE_MAGIC && 
        header.version == SCHEDULE_VERSION) {
        return header.count;
    }
    return 0;
}

bool ScheduleStorage::writeScheduleHeader(const ScheduleStorageHeader& header) {
    return _storage.writeBytes(ADDR_SCHEDULES, 
                              reinterpret_cast<const uint8_t*>(&header), 
                              sizeof(header));
}

bool ScheduleStorage::readScheduleHeader(ScheduleStorageHeader& header) {
    return _storage.readBytes(ADDR_SCHEDULES, 
                             reinterpret_cast<uint8_t*>(&header), 
                             sizeof(header));
}

bool ScheduleStorage::writeSchedule(uint8_t index, const StoredSchedule& schedule) {
    if (index >= MAX_SCHEDULES) {
        return false;
    }
    
    uint16_t addr = ADDR_SCHEDULES + sizeof(ScheduleStorageHeader) + (index * sizeof(StoredSchedule));
    return _storage.writeBytes(addr, 
                              reinterpret_cast<const uint8_t*>(&schedule), 
                              sizeof(schedule));
}

bool ScheduleStorage::readSchedule(uint8_t index, StoredSchedule& schedule) {
    if (index >= MAX_SCHEDULES) {
        return false;
    }
    
    uint16_t addr = ADDR_SCHEDULES + sizeof(ScheduleStorageHeader) + (index * sizeof(StoredSchedule));
    return _storage.readBytes(addr, 
                             reinterpret_cast<uint8_t*>(&schedule), 
                             sizeof(schedule));
}

uint32_t ScheduleStorage::calculateCRC(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
    }
    
    return ~crc;
}

void ScheduleStorage::packSchedule(const TimerSchedule& src, StoredSchedule& dest) {
    memset(&dest, 0, sizeof(dest));
    
    dest.id = src.id;
    dest.type = static_cast<uint8_t>(src.type);
    dest.dayMask = src.dayMask;
    dest.startHour = src.startHour;
    dest.startMinute = src.startMinute;
    dest.endHour = src.endHour;
    dest.endMinute = src.endMinute;
    dest.enabled = src.enabled ? 1 : 0;
    
    // Copy name (truncate if necessary)
    strncpy(dest.name, src.name.c_str(), sizeof(dest.name) - 1);
    dest.name[sizeof(dest.name) - 1] = '\0';
    
    // Pack action data based on type
    switch (src.type) {
        case ScheduleType::WATER_HEATING:
            dest.actionData[0] = src.actionData.waterHeating.targetTempC;
            // Copy remaining reserved bytes
            memcpy(&dest.actionData[1], src.actionData.waterHeating.reserved, 
                   sizeof(src.actionData.waterHeating.reserved));
            break;
            
        case ScheduleType::SPACE_HEATING:
            dest.actionData[0] = src.actionData.spaceHeating.targetTempC;
            dest.actionData[1] = src.actionData.spaceHeating.mode;
            dest.actionData[2] = src.actionData.spaceHeating.zones;
            break;
            
        default:
            // Unknown type, leave action data as zeros
            break;
    }
    
    // Calculate CRC
    dest.crc = calculateCRC(reinterpret_cast<const uint8_t*>(&dest), 
                           sizeof(dest) - sizeof(dest.crc));
}

bool ScheduleStorage::unpackSchedule(const StoredSchedule& src, TimerSchedule& dest) {
    // Round 15 Issue #17 fix: Validate schedule type before casting
    // Valid types: WATER_HEATING=0, SPACE_HEATING=1, LIGHTING=2, VENTILATION=3, CUSTOM=255
    uint8_t typeVal = src.type;
    if (typeVal > 3 && typeVal != 255) {
        LOG_WARN(TAG, "Invalid schedule type value: %d", typeVal);
        return false;
    }

    dest.id = src.id;
    dest.type = static_cast<ScheduleType>(typeVal);
    dest.dayMask = src.dayMask;
    dest.startHour = src.startHour;
    dest.startMinute = src.startMinute;
    dest.endHour = src.endHour;
    dest.endMinute = src.endMinute;
    dest.enabled = src.enabled != 0;
    dest.name = String(src.name);

    // Unpack action data based on type
    switch (dest.type) {
        case ScheduleType::WATER_HEATING:
            dest.actionData.waterHeating.targetTempC = src.actionData[0];
            // Copy remaining reserved bytes
            memcpy(dest.actionData.waterHeating.reserved, &src.actionData[1],
                   sizeof(dest.actionData.waterHeating.reserved));
            break;

        case ScheduleType::SPACE_HEATING:
            dest.actionData.spaceHeating.targetTempC = src.actionData[0];
            dest.actionData.spaceHeating.mode = src.actionData[1];
            dest.actionData.spaceHeating.zones = src.actionData[2];
            break;

        default:
            // Unknown type (LIGHTING, VENTILATION, CUSTOM) - leave action data as default
            break;
    }
    return true;
}

} // namespace schedules