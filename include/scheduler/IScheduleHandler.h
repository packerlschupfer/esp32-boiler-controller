// include/scheduler/IScheduleHandler.h
#pragma once

#include "TimerSchedule.h"

/**
 * @brief Core interface for handling schedule lifecycle events
 * 
 * This is the minimal interface that all schedule handlers must implement.
 * Following ISP (Interface Segregation Principle) - clients should not be
 * forced to depend on interfaces they don't use.
 */
class IScheduleHandler {
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
     * @brief Virtual destructor
     */
    virtual ~IScheduleHandler() = default;
};

/**
 * @brief Optional interface for schedules that support preheating
 * 
 * Implement this interface only if your schedule type needs preheating.
 * This follows ISP by not forcing all handlers to implement preheating.
 */
class IPreheatable {
public:
    /**
     * @brief Called when preheating should begin
     * @param schedule The upcoming schedule
     * @param minutesUntilStart Minutes until the schedule starts
     */
    virtual void onPreheatingStart(const TimerSchedule& schedule, uint32_t minutesUntilStart) = 0;
    
    /**
     * @brief Get the required preheating time in minutes
     * @return Minutes of preheating needed (must be > 0)
     */
    virtual uint32_t getPreheatingMinutes() const = 0;
    
    virtual ~IPreheatable() = default;
};

/**
 * @brief Interface for schedule metadata
 * 
 * Provides type information about the schedule handler.
 */
class IScheduleMetadata {
public:
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
    
    virtual ~IScheduleMetadata() = default;
};

/**
 * @brief Optional interface for schedules that need custom serialization
 * 
 * Only implement if your schedule has type-specific data that needs
 * to be persisted beyond the standard TimerSchedule fields.
 */
class IScheduleSerializable {
public:
    /**
     * @brief Serialize type-specific data for a schedule
     * @param schedule The schedule to serialize data from
     * @param buffer Buffer to write serialized data
     * @param size Size of the buffer
     * @return Number of bytes written, or 0 on error
     */
    virtual size_t serializeActionData(const TimerSchedule& schedule, 
                                     uint8_t* buffer, size_t size) const = 0;
    
    /**
     * @brief Deserialize type-specific data for a schedule
     * @param schedule The schedule to populate with data
     * @param buffer Buffer containing serialized data
     * @param size Size of the data
     * @return true if deserialization successful
     */
    virtual bool deserializeActionData(TimerSchedule& schedule, 
                                     const uint8_t* buffer, size_t size) = 0;
    
    virtual ~IScheduleSerializable() = default;
};

/**
 * @brief Convenience base class that implements all interfaces
 * 
 * For backward compatibility and convenience. New implementations
 * should consider implementing only the interfaces they need.
 */
class ScheduleActionBase : public IScheduleHandler, 
                          public IScheduleMetadata,
                          public IScheduleSerializable {
public:
    // Default implementations for optional features
    
    // IScheduleSerializable defaults - no custom data
    size_t serializeActionData(const TimerSchedule&, uint8_t*, size_t) const override {
        return 0; // No custom data by default
    }
    
    bool deserializeActionData(TimerSchedule&, const uint8_t*, size_t) override {
        return true; // No custom data to deserialize
    }
};

/**
 * @brief Base class for preheat-capable schedule handlers
 */
class PreheatableScheduleActionBase : public ScheduleActionBase,
                                     public IPreheatable {
public:
    // Handlers that support preheating inherit from this
};