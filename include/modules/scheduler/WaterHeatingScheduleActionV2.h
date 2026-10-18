// include/modules/scheduler/WaterHeatingScheduleActionV2.h
// Water heating schedule action handler - using segregated interfaces
#pragma once

#include "scheduler/IScheduleHandler.h"
#include <Arduino.h>

/**
 * @brief Action handler for water heating schedules - V2 with segregated interfaces
 * 
 * This class demonstrates the Interface Segregation Principle by only implementing
 * the interfaces it actually needs. Water heating schedules need preheating support,
 * so we inherit from PreheatableScheduleActionBase.
 */
class WaterHeatingScheduleActionV2 : public PreheatableScheduleActionBase {
public:
    WaterHeatingScheduleActionV2();
    virtual ~WaterHeatingScheduleActionV2() = default;
    
    // IScheduleHandler interface (required)
    void onScheduleStart(const TimerSchedule& schedule) override;
    void onScheduleEnd(const TimerSchedule& schedule) override;
    
    // IPreheatable interface (water heating needs preheating)
    void onPreheatingStart(const TimerSchedule& schedule, uint32_t minutesUntilStart) override;
    uint32_t getPreheatingMinutes() const override;
    
    // IScheduleMetadata interface (required)
    const char* getTypeName() const override { return "water_heating"; }
    ScheduleType getType() const override { return ScheduleType::WATER_HEATING; }
    
    // IScheduleSerializable - we use default implementation from base
    // (water heating data is stored in TimerSchedule.actionData union)
    
    // Water heating specific methods
    const String& getActiveScheduleName() const { return activeScheduleName_; }
    bool isPreheating() const { return isPreheating_; }
    
private:
    String activeScheduleName_;
    bool isPreheating_ = false;
    
    // Constants
    static constexpr uint32_t DEFAULT_PREHEAT_MINUTES = 30;
    static constexpr uint32_t MIN_PREHEAT_MINUTES = 10;
    static constexpr uint32_t MAX_PREHEAT_MINUTES = 60;
};