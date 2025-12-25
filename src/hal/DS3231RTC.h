// src/hal/DS3231RTC.h
// HAL interface for DS3231 RTC

#pragma once

#include "hal/HardwareAbstractionLayer.h"
#include <DS3231Controller.h>

namespace HAL {

/**
 * @brief DS3231 RTC HAL implementation
 *
 * This HAL wrapper provides a consistent interface for the DS3231
 * real-time clock module via I2C.
 */
class DS3231RTC : public IRTC {
private:
    DS3231Controller* rtc;
    bool initialized;
    static constexpr const char* TAG = "DS3231HAL";

public:
    /**
     * @brief Construct DS3231 RTC HAL
     * @param ds3231 Pointer to DS3231Controller instance
     */
    explicit DS3231RTC(DS3231Controller* ds3231);

    /**
     * @brief Initialize the RTC
     * @return true if successful
     */
    bool initialize() override;

    /**
     * @brief Get current date/time
     * @return Current date/time structure
     */
    DateTime getDateTime() override;

    /**
     * @brief Set date/time
     * @param dt New date/time
     * @return true if successful
     */
    bool setDateTime(const DateTime& dt) override;

    /**
     * @brief Check if RTC has lost power
     * @return true if power was lost (time may be incorrect)
     */
    bool hasLostPower() override;

    /**
     * @brief Get RTC temperature
     * @return Temperature in Celsius, or NaN if unavailable
     */
    float getTemperature() override;
};

/**
 * @brief Factory function to create DS3231 RTC HAL
 * @param device DS3231Controller instance
 * @return New DS3231RTC instance
 */
IRTC* createDS3231RTC(DS3231Controller* device);

} // namespace HAL
