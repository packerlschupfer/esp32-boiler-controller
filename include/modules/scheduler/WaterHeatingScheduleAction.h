// include/modules/scheduler/WaterHeatingScheduleAction.h
// Water heating schedule action handler
#ifndef WATER_HEATING_SCHEDULE_ACTION_H
#define WATER_HEATING_SCHEDULE_ACTION_H

#include "IScheduleAction.h"
#include <Arduino.h>

/**
 * @brief Action handler for water heating schedules
 * 
 * This class implements the IScheduleAction interface for water heating
 * schedules. It manages the burner requests and preheating logic for
 * hot water production.
 */
class WaterHeatingScheduleAction : public IScheduleAction {
public:
    WaterHeatingScheduleAction();
    virtual ~WaterHeatingScheduleAction() = default;
    
    // IScheduleAction interface
    void onScheduleStart(const TimerSchedule& schedule) override;
    void onScheduleEnd(const TimerSchedule& schedule) override;
    void onPreheatingStart(const TimerSchedule& schedule, uint32_t minutesUntilStart) override;
    
    bool needsPreheating() const override;
    uint32_t getPreheatingMinutes() const override;
    
    const char* getTypeName() const override;
    ScheduleType getType() const override;
    
    size_t serializeActionData(const TimerSchedule& schedule, uint8_t* buffer, size_t size) const override;
    bool deserializeActionData(TimerSchedule& schedule, const uint8_t* buffer, size_t size) override;
    
    // Water heating specific methods
    const String& getActiveScheduleName() const;
    bool isPreheating() const;
    
private:
    String activeScheduleName_;
    bool isPreheating_ = false;
};

#endif // WATER_HEATING_SCHEDULE_ACTION_H