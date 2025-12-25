// src/hal/DS3231RTC.cpp
// HAL implementation for DS3231 RTC

#include "hal/DS3231RTC.h"
#include "LoggingMacros.h"
#include <cmath>

namespace HAL {

DS3231RTC::DS3231RTC(DS3231Controller* ds3231)
    : rtc(ds3231), initialized(false) {}

bool DS3231RTC::initialize() {
    // Guard against double initialization
    if (initialized) {
        return true;
    }

    if (!rtc) {
        LOG_ERROR(TAG, "RTC pointer is null");
        return false;
    }

    // Initialize the DS3231
    if (rtc->begin()) {
        initialized = true;

        // Check if RTC is running
        if (!rtc->isRunning()) {
            LOG_WARN(TAG, "RTC not running - time may be incorrect");
        }

        LOG_INFO(TAG, "DS3231 HAL initialized");
        return true;
    }

    LOG_ERROR(TAG, "Failed to initialize DS3231");
    return false;
}

IRTC::DateTime DS3231RTC::getDateTime() {
    DateTime dt = {0};

    if (!initialized || !rtc) {
        return dt;
    }

    // Get current time from RTC
    ::DateTime rtcTime = rtc->now();

    dt.year = rtcTime.year();
    dt.month = rtcTime.month();
    dt.day = rtcTime.day();
    dt.hour = rtcTime.hour();
    dt.minute = rtcTime.minute();
    dt.second = rtcTime.second();
    dt.dayOfWeek = rtcTime.dayOfTheWeek();

    return dt;
}

bool DS3231RTC::setDateTime(const DateTime& dt) {
    if (!initialized || !rtc) {
        return false;
    }

    // Create RTClib DateTime and set
    ::DateTime newTime(dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    (void)rtc->setTime(newTime);

    LOG_INFO(TAG, "RTC time set to %04d-%02d-%02d %02d:%02d:%02d",
             dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);

    return true;
}

bool DS3231RTC::hasLostPower() {
    if (!initialized || !rtc) {
        return true;  // Assume power loss if not initialized
    }

    // DS3231Controller doesn't have isLostPower, check if running
    return !rtc->isRunning();
}

float DS3231RTC::getTemperature() {
    if (!initialized || !rtc) {
        return NAN;
    }

    return rtc->getTemperatureCelsius();
}

IRTC* createDS3231RTC(DS3231Controller* device) {
    return new DS3231RTC(device);
}

} // namespace HAL
