// include/TimerSchedule.h
// Extended schedule structure for the generic timer scheduler
#ifndef TIMER_SCHEDULE_H
#define TIMER_SCHEDULE_H

#include <Arduino.h>
#include "IScheduleAction.h"

/**
 * @brief Extended schedule structure with type information
 * 
 * This structure extends the basic schedule with type information
 * and type-specific action data.
 */
struct TimerSchedule {
    // Schedule identification
    uint8_t id;                  // Unique schedule ID (0-255)
    ScheduleType type;           // Type of schedule (water, heating, etc.)
    
    // Basic schedule data
    String name;                 // Human-readable schedule name
    uint8_t dayMask;            // Bit mask for days (bit 0 = Sunday)
    uint8_t startHour;          // Start hour (0-23)
    uint8_t startMinute;        // Start minute (0-59)
    uint8_t endHour;            // End hour (0-23)  
    uint8_t endMinute;          // End minute (0-59)
    bool enabled;               // Schedule active flag
    
    // Type-specific action data
    union ActionData {
        // Water heating specific
        struct {
            uint8_t targetTempC;     // Target temperature in Celsius
            uint8_t priority;        // Priority mode (0=normal, 1=priority)
            uint8_t reserved[6];
        } waterHeating;
        
        // Space heating specific  
        struct {
            uint8_t targetTempC;     // Target room temperature
            uint8_t mode;            // 0=comfort, 1=eco, 2=frost protection
            uint8_t zones;           // Zone bitmask for multi-zone systems
            uint8_t reserved[5];
        } spaceHeating;
        
        // Raw data access
        uint8_t raw[8];
    } actionData;
    
    // Constructor
    TimerSchedule() : id(0), type(ScheduleType::WATER_HEATING), 
                      dayMask(0), startHour(0), startMinute(0),
                      endHour(0), endMinute(0), enabled(false) {
        memset(&actionData, 0, sizeof(actionData));
    }
    
    // Helper methods
    bool isDayEnabled(uint8_t dayOfWeek) const {
        return (dayMask & (1 << (dayOfWeek % 7))) != 0;
    }
    
    void setDay(uint8_t dayOfWeek, bool enable) {
        if (enable) {
            dayMask |= (1 << (dayOfWeek % 7));
        } else {
            dayMask &= ~(1 << (dayOfWeek % 7));
        }
    }
    
    bool isActiveNow(uint8_t currentHour, uint8_t currentMinute, uint8_t currentDayOfWeek) const {
        if (!enabled) {
            return false;
        }

        uint16_t currentTime = currentHour * 60 + currentMinute;
        uint16_t startTime = startHour * 60 + startMinute;
        uint16_t endTime = endHour * 60 + endMinute;

        if (startTime <= endTime) {
            // Normal case: schedule doesn't cross midnight
            // Only check if today is enabled
            return isDayEnabled(currentDayOfWeek) &&
                   currentTime >= startTime && currentTime < endTime;
        } else {
            // Schedule crosses midnight - need to check BOTH days
            // Example: Sunday 22:00 to Monday 02:00
            // - On Sunday after 22:00: currentDay=Sunday must be enabled
            // - On Monday before 02:00: previousDay=Sunday must be enabled
            uint8_t previousDay = (currentDayOfWeek == 0) ? 6 : (currentDayOfWeek - 1);

            // In the "start" period (after startTime on start day)
            bool inStartPeriod = isDayEnabled(currentDayOfWeek) && currentTime >= startTime;

            // In the "end" period (before endTime, continuing from previous day)
            bool inEndPeriod = isDayEnabled(previousDay) && currentTime < endTime;

            return inStartPeriod || inEndPeriod;
        }
    }
    
    uint16_t getDurationMinutes() const {
        uint16_t start = startHour * 60 + startMinute;
        uint16_t end = endHour * 60 + endMinute;
        
        if (end >= start) {
            return end - start;
        } else {
            // Crosses midnight
            return (24 * 60 - start) + end;
        }
    }
};

// Maximum number of schedules per type
static constexpr uint8_t MAX_SCHEDULES_PER_TYPE = 10;

// Total maximum schedules across all types
static constexpr uint8_t MAX_TOTAL_SCHEDULES = 30;

#endif // TIMER_SCHEDULE_H