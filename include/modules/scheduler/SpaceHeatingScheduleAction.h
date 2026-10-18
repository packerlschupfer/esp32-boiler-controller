// include/modules/scheduler/SpaceHeatingScheduleAction.h
// Space heating schedule action handler
#ifndef SPACE_HEATING_SCHEDULE_ACTION_H
#define SPACE_HEATING_SCHEDULE_ACTION_H

#include "IScheduleAction.h"
#include <Arduino.h>

/**
 * @brief Action handler for space heating schedules
 * 
 * This class implements the IScheduleAction interface for space heating
 * schedules. It manages the heating requests and temperature control for
 * room/space heating.
 */
class SpaceHeatingScheduleAction : public IScheduleAction {
public:
    SpaceHeatingScheduleAction();
    virtual ~SpaceHeatingScheduleAction() = default;
    
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
    
    // Space heating specific methods
    const String& getActiveScheduleName() const;
    bool isPreheating() const;
    uint8_t getActiveMode() const;
    
private:
    String activeScheduleName_;
    bool isPreheating_ = false;
    uint8_t activeMode_ = 0; // 0=comfort, 1=eco, 2=frost protection
    
    // Helper method to apply temperature based on mode
    void applyHeatingMode(uint8_t mode, uint8_t targetTempC);
};

#endif // SPACE_HEATING_SCHEDULE_ACTION_H