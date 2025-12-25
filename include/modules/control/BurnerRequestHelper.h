// include/modules/control/BurnerRequestHelper.h
#ifndef BURNER_REQUEST_HELPER_H
#define BURNER_REQUEST_HELPER_H

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include "bits/BurnerRequestBits.h"
#include "shared/Temperature.h"
#include "core/SystemResourceProvider.h"
#include "LoggingMacros.h"

/**
 * @brief Helper class for managing burner requests with proper event-driven semantics
 * 
 * This class ensures that when burner requests change, the appropriate change
 * event bits are set to wake up the BurnerControlTask.
 */
class BurnerRequestHelper {
public:
    /**
     * @brief Set heating request with change detection
     * @param enable True to request heating, false to cancel
     * @param targetTemp Target temperature in Temperature_t format
     * @param highPower True for high power mode
     * @return True if request was changed
     */
    static bool setHeatingRequest(bool enable, Temperature_t targetTemp = 700, bool highPower = false) {
        const char* TAG = "BurnerReqHelper";
        EventGroupHandle_t eventGroup = SRP::getBurnerRequestEventGroup();
        if (!eventGroup) return false;
        
        // Get current state
        EventBits_t currentBits = xEventGroupGetBits(eventGroup);
        EventBits_t newBits = currentBits;
        
        // Clear heating-related bits
        newBits &= ~(BURNER_REQUEST_HEATING_BIT | BURNER_REQUEST_POWER_BITS | BURNER_REQUEST_TEMP_MASK);
        
        if (enable) {
            // Set new request
            newBits |= BURNER_REQUEST_HEATING_BIT;
            newBits |= ENCODE_BURNER_TEMP(targetTemp);
            newBits |= highPower ? BURNER_REQUEST_POWER_HIGH_BIT : BURNER_REQUEST_POWER_LOW_BIT;
        }
        
        // Check if anything changed
        bool changed = (newBits != currentBits);
        
        if (changed) {
            LOG_INFO(TAG, "Heating request changed: %s, target: %.1f°C, power: %s",
                     enable ? "ON" : "OFF",
                     enable ? tempToFloat(targetTemp) : 0.0f,
                     enable ? (highPower ? "HIGH" : "LOW") : "N/A");
            
            // Set the change event bits BEFORE updating the request
            xEventGroupSetBits(eventGroup, BURNER_REQUEST_HEATING_CHANGED_BIT | BURNER_REQUEST_CHANGED_BIT);
            
            // Update the request bits
            if (enable) {
                xEventGroupSetBits(eventGroup, newBits & BURNER_REQUEST_ALL_BITS);
            } else {
                xEventGroupClearBits(eventGroup, BURNER_REQUEST_HEATING_BIT | BURNER_REQUEST_POWER_BITS | BURNER_REQUEST_TEMP_MASK);
            }
        }
        
        return changed;
    }
    
    /**
     * @brief Set water heating request with change detection
     * @param enable True to request water heating, false to cancel
     * @param targetTemp Target temperature in Temperature_t format
     * @param highPower True for high power mode
     * @return True if request was changed
     *
     * Note: Water priority is read from SystemState::WATER_PRIORITY (single source of truth)
     */
    static bool setWaterRequest(bool enable, Temperature_t targetTemp = 600, bool highPower = false) {
        const char* TAG = "BurnerReqHelper";
        EventGroupHandle_t eventGroup = SRP::getBurnerRequestEventGroup();
        if (!eventGroup) return false;

        // Get current state
        EventBits_t currentBits = xEventGroupGetBits(eventGroup);
        EventBits_t newBits = currentBits;

        // Clear water-related bits
        newBits &= ~BURNER_REQUEST_WATER_BIT;

        // For water requests, we need to preserve heating bits but update shared bits (temp, power)
        if (enable) {
            newBits |= BURNER_REQUEST_WATER_BIT;

            // Update temperature and power (these are shared)
            newBits &= ~(BURNER_REQUEST_POWER_BITS | BURNER_REQUEST_TEMP_MASK);
            newBits |= ENCODE_BURNER_TEMP(targetTemp);
            newBits |= highPower ? BURNER_REQUEST_POWER_HIGH_BIT : BURNER_REQUEST_POWER_LOW_BIT;
        }

        // Check if anything changed
        bool changed = (newBits != currentBits);

        if (changed) {
            LOG_INFO(TAG, "Water request changed: %s, target: %.1f°C, power: %s",
                     enable ? "ON" : "OFF",
                     enable ? tempToFloat(targetTemp) : 0.0f,
                     enable ? (highPower ? "HIGH" : "LOW") : "N/A");

            // Set the change event bits BEFORE updating the request
            xEventGroupSetBits(eventGroup, BURNER_REQUEST_WATER_CHANGED_BIT | BURNER_REQUEST_CHANGED_BIT);

            // Update the request bits
            if (enable) {
                xEventGroupSetBits(eventGroup, newBits & BURNER_REQUEST_ALL_BITS);
            } else {
                xEventGroupClearBits(eventGroup, BURNER_REQUEST_WATER_BIT);

                // If no heating request either, clear shared bits
                if (!(currentBits & BURNER_REQUEST_HEATING_BIT)) {
                    xEventGroupClearBits(eventGroup, BURNER_REQUEST_POWER_BITS | BURNER_REQUEST_TEMP_MASK);
                }
            }
        }

        return changed;
    }
    
    /**
     * @brief Clear all burner requests
     */
    static void clearAllRequests() {
        EventGroupHandle_t eventGroup = SRP::getBurnerRequestEventGroup();
        if (!eventGroup) return;
        
        EventBits_t currentBits = xEventGroupGetBits(eventGroup);
        if (currentBits & BURNER_REQUEST_ALL_BITS) {
            // Set change event before clearing
            xEventGroupSetBits(eventGroup, BURNER_REQUEST_CHANGED_BIT);
            xEventGroupClearBits(eventGroup, BURNER_REQUEST_ALL_BITS);
        }
    }
    
    /**
     * @brief Check if any burner request is active
     */
    static bool isAnyRequestActive() {
        EventGroupHandle_t eventGroup = SRP::getBurnerRequestEventGroup();
        if (!eventGroup) return false;
        
        EventBits_t bits = xEventGroupGetBits(eventGroup);
        return (bits & BURNER_REQUEST_ANY_BIT) != 0;
    }
};

#endif // BURNER_REQUEST_HELPER_H