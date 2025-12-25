// include/IScheduleAction.h
// Interface for schedule action handlers in the generic timer scheduler system
#ifndef ISCHEDULE_ACTION_H
#define ISCHEDULE_ACTION_H

#include <Arduino.h>

// Forward declaration
struct TimerSchedule;

/**
 * @brief Schedule type enumeration
 */
enum class ScheduleType : uint8_t {
    WATER_HEATING = 0,    // Hot water heating schedules
    SPACE_HEATING = 1,    // Room/space heating schedules
    LIGHTING = 2,         // Future: lighting control
    VENTILATION = 3,      // Future: ventilation control
    CUSTOM = 255          // User-defined schedule types
};

/**
 * @brief Interface for handling schedule actions
 * 
 * Implement this interface to define what happens when a schedule
 * starts, ends, or needs preheating preparation.
 */
class IScheduleAction {
public:
    /**
     * @brief Called when a schedule period starts
     * @param schedule The schedule that is starting
     */
    virtual void onScheduleStart(const TimerSchedule& schedule) = 0;
    
    /**
     * @brief Called when a schedule period ends
     * @param schedule The schedule that is ending
     */
    virtual void onScheduleEnd(const TimerSchedule& schedule) = 0;
    
    /**
     * @brief Called when preheating should begin
     * @param schedule The upcoming schedule
     * @param minutesUntilStart Minutes until the schedule starts
     */
    virtual void onPreheatingStart(const TimerSchedule& schedule, uint32_t minutesUntilStart) = 0;
    
    /**
     * @brief Check if this action type requires preheating
     * @return true if preheating is needed
     */
    virtual bool needsPreheating() const = 0;
    
    /**
     * @brief Get the required preheating time in minutes
     * @return Minutes of preheating needed (0 if no preheating)
     */
    virtual uint32_t getPreheatingMinutes() const = 0;
    
    /**
     * @brief Get a human-readable name for this schedule type
     * @return Type name string
     */
    virtual const char* getTypeName() const = 0;
    
    /**
     * @brief Get the schedule type enum value
     * @return The ScheduleType this handler manages
     */
    virtual ScheduleType getType() const = 0;
    
    /**
     * @brief Serialize type-specific data for a schedule
     * @param schedule The schedule to serialize data from
     * @param buffer Buffer to write serialized data
     * @param size Size of the buffer
     * @return Number of bytes written
     */
    virtual size_t serializeActionData(const TimerSchedule& schedule, uint8_t* buffer, size_t size) const = 0;
    
    /**
     * @brief Deserialize type-specific data for a schedule
     * @param schedule The schedule to populate with data
     * @param buffer Buffer containing serialized data
     * @param size Size of the data
     * @return true if deserialization successful
     */
    virtual bool deserializeActionData(TimerSchedule& schedule, const uint8_t* buffer, size_t size) = 0;
    
    /**
     * @brief Virtual destructor
     */
    virtual ~IScheduleAction() = default;
};

#endif // ISCHEDULE_ACTION_H