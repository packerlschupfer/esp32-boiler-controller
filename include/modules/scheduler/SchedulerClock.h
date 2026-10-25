// include/modules/scheduler/SchedulerClock.h
#ifndef SCHEDULER_CLOCK_H
#define SCHEDULER_CLOCK_H

#include <cstdint>
#include <ctime>
#include <stdlib.h>   // setenv() is POSIX, not in <cstdlib> on every toolchain

/**
 * @brief Converting the RTC's instant into the wall-clock a schedule is written in.
 *
 * WHY THIS EXISTS (2026-10-25)
 *
 * The DS3231 used to hold LOCAL time: updateRTCFromNTP() computed the CET/CEST offset at the
 * moment of the sync and wrote local wall-clock into the chip. That is lossy. A wall-clock
 * reading carries no record of which offset produced it, so when the DST rule changes the
 * offset the chip keeps ticking the OLD one and there is no way to notice from the stored
 * fields alone - 14:00 and 15:00 are equally plausible times of day.
 *
 * With a network that self-healed within the hourly NTP sync, so the bug was easy to miss.
 * Without one it did not heal at all: this controller is required to work with no internet
 * and no Home Assistant, and in that configuration every schedule ran an hour off from the
 * last Sunday in March to the last Sunday in October, and an hour off the other way for the
 * rest of the year - whichever side of a transition the last successful sync fell on.
 *
 * So the RTC now holds UTC, which is monotonic and offset-free, and local time is DERIVED
 * here from the POSIX rule in TZ_RULE. That rule is compiled into the firmware: deriving the
 * offset needs no network, no NTP and no Home Assistant, which is the whole point. NTP is
 * then only needed for what it is actually good for - correcting the crystal's drift.
 *
 * ONE-TIME UPGRADE WINDOW: a DS3231 written by the OLD firmware holds local time, and this
 * code will read those fields as UTC until the first NTP sync of the new firmware rewrites
 * them. During that window derived local time runs fast by the current UTC offset (1 h in
 * winter, 2 h in summer), i.e. schedules would fire early. It closes at the first sync, which
 * on an OTA upgrade is seconds away because OTA needs the network anyway, and there is no
 * persisted format marker precisely because a marker would be new non-volatile state with its
 * own failure mode (FRAM and the RTC are separate chips and can disagree after a battery or
 * board swap) guarding a window that only exists once per device. Schedules were empty when
 * this landed, so the window had nothing to get wrong.
 */
namespace SchedulerClock {

    /// The single source of truth for this installation's timezone. Also applied to the
    /// process via applyTimezone() during hardware init, because localtime_r() reads it from
    /// the environment - keeping the string in one place stops the derived offset here and
    /// the system clock's idea of local time from drifting apart.
    ///
    /// CET-1CEST,M3.5.0,M10.5.0/3:
    ///   CET  = UTC+1 standard, CEST = UTC+2 summer (POSIX signs offsets the other way round,
    ///          hence the -1)
    ///   M3.5.0  = DST starts  last Sunday in March   at 02:00 local
    ///   M10.5.0/3 = DST ends  last Sunday in October at 03:00 local
    constexpr const char* TZ_RULE = "CET-1CEST,M3.5.0,M10.5.0/3";

    /// Must run before any localtime_r()/mktime() call, and before the RTC is read.
    inline void applyTimezone() {
        setenv("TZ", TZ_RULE, 1);
        tzset();
    }

    /**
     * @brief Civil UTC fields -> Unix epoch, i.e. timegm().
     *
     * Hand-rolled because timegm() is not declared by this toolchain, and mktime() is the
     * wrong tool: mktime() interprets its input as LOCAL time, so feeding it the RTC's UTC
     * fields would add the CET/CEST offset a second time. Howard Hinnant's days_from_civil,
     * exact for any proleptic-Gregorian date and free of libc state.
     *
     * @param month 1-12, @param day 1-31 - fields as the DS3231 reports them, not tm's
     *        0-based month and 1900-based year.
     */
    inline int64_t epochFromUtcFields(int year, unsigned month, unsigned day,
                                      unsigned hour, unsigned minute, unsigned second) {
        int y = year;
        y -= (month <= 2) ? 1 : 0;
        const int64_t era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = static_cast<unsigned>(y - era * 400);                  // [0,399]
        const unsigned mp  = (month + 9u) % 12u;                                     // Mar = 0
        const unsigned doy = (153u * mp + 2u) / 5u + day - 1u;                      // [0,365]
        const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;              // [0,146096]
        const int64_t days = era * 146097 + static_cast<int64_t>(doe) - 719468;
        return days * 86400 + static_cast<int64_t>(hour) * 3600
                            + static_cast<int64_t>(minute) * 60
                            + static_cast<int64_t>(second);
    }

    /// The fields a schedule is compared against. dayOfWeek is 0 = Sunday, matching both
    /// tm_wday and TimerSchedule::dayMask bit 0. The date and dst flag are carried only so the
    /// clock can be published - nothing in the schedule comparison reads them.
    struct WallClock {
        bool     valid     = false;
        int      year      = 0;
        uint8_t  month     = 0;   // 1-12
        uint8_t  day       = 0;   // 1-31
        uint8_t  hour      = 0;
        uint8_t  minute    = 0;
        uint8_t  second    = 0;
        uint8_t  dayOfWeek = 0;   // 0 = Sunday
        bool     dst       = false;
    };

    /// Years before this mean the RTC lost its battery and NTP has never run: there is no
    /// trustworthy time, so the caller must suspend schedules rather than act on a guess.
    constexpr int MIN_PLAUSIBLE_YEAR = 2020;

    /**
     * @brief The local wall-clock at a UTC instant, with the DST rule applied.
     *
     * The plausibility check is on the UTC epoch rather than on the converted year so that a
     * zeroed RTC is rejected before localtime_r() is asked about 1970 - and so the answer does
     * not depend on which side of New Year the offset puts it.
     */
    inline WallClock localWallClock(int64_t utcEpoch) {
        WallClock out;
        if (utcEpoch < epochFromUtcFields(MIN_PLAUSIBLE_YEAR, 1, 1, 0, 0, 0)) {
            return out;  // valid stays false
        }
        const time_t t = static_cast<time_t>(utcEpoch);
        struct tm local {};
        if (localtime_r(&t, &local) == nullptr) {
            return out;
        }
        out.valid     = true;
        out.year      = local.tm_year + 1900;
        out.month     = static_cast<uint8_t>(local.tm_mon + 1);
        out.day       = static_cast<uint8_t>(local.tm_mday);
        out.hour      = static_cast<uint8_t>(local.tm_hour);
        out.minute    = static_cast<uint8_t>(local.tm_min);
        out.second    = static_cast<uint8_t>(local.tm_sec);
        out.dayOfWeek = static_cast<uint8_t>(local.tm_wday);
        out.dst       = local.tm_isdst > 0;
        return out;
    }

} // namespace SchedulerClock

#endif // SCHEDULER_CLOCK_H
