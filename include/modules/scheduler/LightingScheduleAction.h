// include/modules/scheduler/LightingScheduleAction.h
// Example of a simple schedule handler that doesn't need preheating
#pragma once

#include "scheduler/IScheduleHandler.h"

/**
 * @brief Simple lighting schedule handler - demonstrates ISP benefits
 * 
 * This handler only implements the interfaces it needs:
 * - IScheduleHandler for basic start/stop
 * - IScheduleMetadata for type information
 * 
 * It does NOT implement:
 * - IPreheatable (lights don't need preheating)
 * - IScheduleSerializable (no custom data beyond standard fields)
 * 
 * This results in a simpler, more focused class.
 */
class LightingScheduleAction : public ScheduleActionBase {
public:
    LightingScheduleAction() = default;
    virtual ~LightingScheduleAction() = default;
    
    // IScheduleHandler interface (required)
    void onScheduleStart(const TimerSchedule& schedule) override {
        // Turn on lights - simple implementation
        LOG_INFO("Lighting", "Schedule '%s' starting - turning lights ON", 
                 schedule.name.c_str());
        // In real implementation: control GPIO or send command to lighting controller
    }
    
    void onScheduleEnd(const TimerSchedule& schedule) override {
        // Turn off lights - simple implementation
        LOG_INFO("Lighting", "Schedule '%s' ending - turning lights OFF", 
                 schedule.name.c_str());
        // In real implementation: control GPIO or send command to lighting controller
    }
    
    // IScheduleMetadata interface (required)
    const char* getTypeName() const override { return "lighting"; }
    ScheduleType getType() const override { return ScheduleType::LIGHTING; }
    
    // That's it! No preheating methods to implement.
    // No serialization needed - standard schedule fields are sufficient.
    // This class is much simpler than if it had to implement all methods.
};