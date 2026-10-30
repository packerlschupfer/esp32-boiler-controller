// include/modules/scheduler/SpaceHeatingScheduleAction.h
// Space heating schedule action handler
#ifndef SPACE_HEATING_SCHEDULE_ACTION_H
#define SPACE_HEATING_SCHEDULE_ACTION_H

#include "IScheduleAction.h"
#include "shared/Temperature.h"  // Temperature_t for the pre-window target
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

    /**
     * @brief The room target in force before this window started, to restore at its end.
     *
     * A window end used to write DEFAULT_FROST_TEMP (10.0 C) into
     * settings.targetTemperatureInside and leave it there. The weather curve is anchored on
     * that target and the room backstop sits above it, so BETWEEN windows the controller
     * withheld heat until the room fell to roughly 11.5-12 C - for the documented Mon-Fri
     * 18:00-22:00 example, that is every night and all weekend. Nothing in any doc or recorded
     * decision says a window end should mean "let the house go to 10 C"; the ECO mode that
     * exists for a setback is a separate, configurable thing.
     *
     * It also became durable by accident: any save while parked - every HA
     * enable/priority/override toggle, params/save, autotune, or a debounced params/set of
     * another key - wrote 10.0 into NVS through the pre-save shadow sync. So a reboot's
     * setpoint depended on the minute: inside a window the start re-fired and set the window
     * target, outside one the stale NVS 10.0 was operative. Found by the 2026-10-28 review.
     *
     * 0 means "nothing saved" - restore nothing, which is the pre-first-window state.
     */
    
    // Helper method to apply temperature based on mode
    void applyHeatingMode(uint8_t mode, uint8_t targetTempC);
};

#endif // SPACE_HEATING_SCHEDULE_ACTION_H