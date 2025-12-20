// include/events/EventTypeSystem.h
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <type_traits>

/**
 * Type-safe Event System for FreeRTOS Event Groups
 * 
 * This replaces manual bit definitions with a type-safe system that:
 * 1. Prevents mixing events from different groups
 * 2. Automatically manages bit positions
 * 3. Provides compile-time type checking
 * 4. Centralizes event definitions
 */

// Base template for event types
template<typename T>
class EventType {
private:
    EventBits_t bit_;
    const char* name_;
    
public:
    constexpr EventType(EventBits_t bit, const char* name) 
        : bit_(bit), name_(name) {}
    
    constexpr EventBits_t bit() const { return bit_; }
    constexpr const char* name() const { return name_; }
    
    // Combine events with | operator
    constexpr EventBits_t operator|(const EventType<T>& other) const {
        return bit_ | other.bit_;
    }
    
    constexpr EventBits_t operator|(EventBits_t bits) const {
        return bit_ | bits;
    }
};

// Type-safe event group wrapper
template<typename EventTypeEnum>
class TypedEventGroup {
private:
    EventGroupHandle_t handle_;
    const char* name_;
    
public:
    TypedEventGroup(const char* name) : name_(name) {
        handle_ = xEventGroupCreate();
    }
    
    ~TypedEventGroup() {
        if (handle_) {
            vEventGroupDelete(handle_);
        }
    }
    
    // Type-safe set bits - only accepts correct event type
    void setBits(EventType<EventTypeEnum> event) {
        xEventGroupSetBits(handle_, event.bit());
    }
    
    void setBits(EventBits_t bits) {
        xEventGroupSetBits(handle_, bits);
    }
    
    // Type-safe clear bits
    void clearBits(EventType<EventTypeEnum> event) {
        xEventGroupClearBits(handle_, event.bit());
    }
    
    // Type-safe wait for bits
    EventBits_t waitForBits(EventType<EventTypeEnum> event, 
                           bool clearOnExit = true,
                           bool waitForAll = false,
                           TickType_t timeout = portMAX_DELAY) {
        return xEventGroupWaitBits(handle_, event.bit(), 
                                  clearOnExit ? pdTRUE : pdFALSE,
                                  waitForAll ? pdTRUE : pdFALSE,
                                  timeout);
    }
    
    EventBits_t waitForBits(EventBits_t bits,
                           bool clearOnExit = true,
                           bool waitForAll = false,
                           TickType_t timeout = portMAX_DELAY) {
        return xEventGroupWaitBits(handle_, bits,
                                  clearOnExit ? pdTRUE : pdFALSE,
                                  waitForAll ? pdTRUE : pdFALSE,
                                  timeout);
    }
    
    // Get current bits
    EventBits_t getBits() const {
        return xEventGroupGetBits(handle_);
    }
    
    // Check if specific event is set
    bool isSet(EventType<EventTypeEnum> event) const {
        return (getBits() & event.bit()) != 0;
    }
    
    EventGroupHandle_t handle() { return handle_; }
};

// Macro to define events with automatic bit assignment
#define DEFINE_EVENT_TYPE(TypeName) \
    struct TypeName##Tag {}; \
    using TypeName = EventType<TypeName##Tag>;

#define EVENT(type, name) \
    constexpr type name((1 << __COUNTER__), #name)

// Example: Define System State Events
namespace SystemStateEvents {
    DEFINE_EVENT_TYPE(SystemState);
    
    // Events automatically get unique bit positions
    EVENT(SystemState, BOILER_ENABLED);
    EVENT(SystemState, HEATING_ENABLED);
    EVENT(SystemState, WATER_ENABLED);
    EVENT(SystemState, BOILER_ON);
    EVENT(SystemState, HEATING_ON);
    EVENT(SystemState, WATER_ON);
    EVENT(SystemState, EMERGENCY_STOP);
    EVENT(SystemState, ERROR_ACTIVE);
}

// Example: Define Burner Request Events
namespace BurnerRequestEvents {
    DEFINE_EVENT_TYPE(BurnerRequest);
    
    EVENT(BurnerRequest, HEATING);
    EVENT(BurnerRequest, WATER);
    EVENT(BurnerRequest, POWER_LOW);
    EVENT(BurnerRequest, POWER_HIGH);
    EVENT(BurnerRequest, IGNITION);
    EVENT(BurnerRequest, SHUTDOWN);
}

// Example: Define Sensor Events
namespace SensorEvents {
    DEFINE_EVENT_TYPE(SensorEvent);
    
    EVENT(SensorEvent, BOILER_TEMP_UPDATE);
    EVENT(SensorEvent, WATER_TEMP_UPDATE);
    EVENT(SensorEvent, OUTSIDE_TEMP_UPDATE);
    EVENT(SensorEvent, RETURN_TEMP_UPDATE);
    EVENT(SensorEvent, INSIDE_TEMP_UPDATE);
    EVENT(SensorEvent, ALL_TEMPS_UPDATE);
}

// Usage example:
/*
class ExampleUsage {
    TypedEventGroup<SystemStateEvents::SystemStateTag> systemStateGroup{"SystemState"};
    TypedEventGroup<BurnerRequestEvents::BurnerRequestTag> burnerRequestGroup{"BurnerRequest"};
    
    void example() {
        // Type-safe - can only set system state events on system state group
        systemStateGroup.setBits(SystemStateEvents::BOILER_ENABLED);
        
        // This would not compile - type mismatch!
        // systemStateGroup.setBits(BurnerRequestEvents::HEATING);  // ERROR!
        
        // Wait for multiple events
        auto bits = systemStateGroup.waitForBits(
            SystemStateEvents::BOILER_ENABLED | SystemStateEvents::HEATING_ENABLED
        );
        
        // Check specific event
        if (systemStateGroup.isSet(SystemStateEvents::EMERGENCY_STOP)) {
            // Handle emergency
        }
    }
};
*/