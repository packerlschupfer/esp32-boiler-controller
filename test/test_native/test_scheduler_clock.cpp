/**
 * @file test_scheduler_clock.cpp
 * @brief Unit tests for SchedulerClock - the RTC's UTC -> local schedule wall-clock (2026-10-25)
 *
 * These run the REAL localtime_r() against the REAL TZ rule, not a model of it: the host has
 * the same POSIX rule parser the ESP32's newlib does, so the DST instants pinned below are the
 * ones the device will actually use. That is the whole reason this logic was moved out of
 * TimerSchedulerTask - there it needed FreeRTOS, I2C and a DS3231 to reach.
 */

#include <unity.h>
#include <cstdint>
#include <ctime>

#include "../../include/modules/scheduler/SchedulerClock.h"

// setUp and tearDown are defined in test_main.cpp

using namespace SchedulerClock;

// The EU rule switches at 01:00 UTC in both directions. 2026's transitions:
static constexpr int64_t SPRING_2026 = 1774746000;  // Sun 2026-03-29 01:00:00 UTC
static constexpr int64_t FALL_2026   = 1792890000;  // Sun 2026-10-25 01:00:00 UTC

void test_scheduler_clock_spring_forward_skips_an_hour() {
    applyTimezone();

    // One second before the switch it is still CET (UTC+1).
    const WallClock before = localWallClock(SPRING_2026 - 1);
    TEST_ASSERT_TRUE(before.valid);
    TEST_ASSERT_EQUAL_UINT8(1, before.hour);
    TEST_ASSERT_EQUAL_UINT8(59, before.minute);

    // At the switch local jumps straight from 01:59:59 to 03:00:00 - CEST (UTC+2).
    const WallClock after = localWallClock(SPRING_2026);
    TEST_ASSERT_TRUE(after.valid);
    TEST_ASSERT_EQUAL_UINT8(3, after.hour);
    TEST_ASSERT_EQUAL_UINT8(0, after.minute);

    // 02:00 local does not exist that day. Sweep the whole gap to prove no instant lands in it.
    for (int64_t t = SPRING_2026 - 3600; t < SPRING_2026 + 3600; t += 60) {
        const WallClock w = localWallClock(t);
        TEST_ASSERT_TRUE(w.valid);
        TEST_ASSERT_TRUE_MESSAGE(w.hour != 2, "02:xx local is skipped by the spring switch");
    }
}

void test_scheduler_clock_fall_back_repeats_an_hour() {
    applyTimezone();

    // 02:59:59 CEST ...
    const WallClock before = localWallClock(FALL_2026 - 1);
    TEST_ASSERT_TRUE(before.valid);
    TEST_ASSERT_EQUAL_UINT8(2, before.hour);
    TEST_ASSERT_EQUAL_UINT8(59, before.minute);

    // ... becomes 02:00:00 CET. The local hour 02:00-03:00 happens twice, an hour apart in UTC.
    const WallClock after = localWallClock(FALL_2026);
    TEST_ASSERT_TRUE(after.valid);
    TEST_ASSERT_EQUAL_UINT8(2, after.hour);
    TEST_ASSERT_EQUAL_UINT8(0, after.minute);

    // Both 02:30 locals are an hour apart in UTC - the ambiguity a wall-clock RTC could not
    // represent, and why mktime(tm_isdst = -1) on stored local fields was a coin flip.
    const WallClock firstHalf  = localWallClock(FALL_2026 - 1800);
    const WallClock secondHalf = localWallClock(FALL_2026 + 1800);
    TEST_ASSERT_EQUAL_UINT8(2, firstHalf.hour);
    TEST_ASSERT_EQUAL_UINT8(30, firstHalf.minute);
    TEST_ASSERT_EQUAL_UINT8(2, secondHalf.hour);
    TEST_ASSERT_EQUAL_UINT8(30, secondHalf.minute);
}

void test_scheduler_clock_is_the_dst_fix_a_schedule_holds_its_wall_clock() {
    applyTimezone();

    // THE REGRESSION TEST. A schedule set for 06:00 local must fire at 06:00 local all year,
    // which means the UTC instant behind it MOVES by an hour across a transition. The old
    // design stored local wall-clock in the DS3231 and could not notice the rule changing, so
    // with no NTP to overwrite it the chip kept ticking the stale offset and every schedule
    // ran an hour off for months.
    //
    // 06:00 CEST on 2026-10-24 (the day before the switch) is 04:00 UTC.
    const WallClock summerSide = localWallClock(epochFromUtcFields(2026, 10, 24, 4, 0, 0));
    TEST_ASSERT_TRUE(summerSide.valid);
    TEST_ASSERT_EQUAL_UINT8(6, summerSide.hour);

    // 06:00 CET on 2026-10-26 (the day after) is 05:00 UTC - one hour later in UTC, same
    // wall-clock. Feeding the summer instant in after the switch gives 05:00 local, i.e. the
    // schedule would have fired an hour early: exactly the observed defect.
    const WallClock winterSide = localWallClock(epochFromUtcFields(2026, 10, 26, 5, 0, 0));
    TEST_ASSERT_TRUE(winterSide.valid);
    TEST_ASSERT_EQUAL_UINT8(6, winterSide.hour);

    const WallClock stale = localWallClock(epochFromUtcFields(2026, 10, 26, 4, 0, 0));
    TEST_ASSERT_EQUAL_UINT8(5, stale.hour);
}

void test_scheduler_clock_epoch_from_utc_fields_matches_known_instants() {
    // Pure arithmetic - no libc, no TZ. Deliberately asserted against epochs computed
    // independently, not against mktime/timegm on the host.
    TEST_ASSERT_EQUAL_INT64(0, epochFromUtcFields(1970, 1, 1, 0, 0, 0));
    TEST_ASSERT_EQUAL_INT64(1577836800, epochFromUtcFields(2020, 1, 1, 0, 0, 0));
    TEST_ASSERT_EQUAL_INT64(1577836799, epochFromUtcFields(2019, 12, 31, 23, 59, 59));
    TEST_ASSERT_EQUAL_INT64(951868800, epochFromUtcFields(2000, 3, 1, 0, 0, 0));
    TEST_ASSERT_EQUAL_INT64(SPRING_2026, epochFromUtcFields(2026, 3, 29, 1, 0, 0));
    TEST_ASSERT_EQUAL_INT64(FALL_2026, epochFromUtcFields(2026, 10, 25, 1, 0, 0));

    // 2024 was a leap year and 2100 will not be - the two cases a naive /4 gets wrong. The
    // leap day has to be a real date, and March 1st has to sit one day after it.
    TEST_ASSERT_EQUAL_INT64(1709208000, epochFromUtcFields(2024, 2, 29, 12, 0, 0));
    TEST_ASSERT_EQUAL_INT64(86400, epochFromUtcFields(2024, 3, 1, 12, 0, 0)
                                   - epochFromUtcFields(2024, 2, 29, 12, 0, 0));
    TEST_ASSERT_EQUAL_INT64(86400, epochFromUtcFields(2100, 3, 1, 0, 0, 0)
                                   - epochFromUtcFields(2100, 2, 28, 0, 0, 0));
}

void test_scheduler_clock_epoch_from_utc_fields_applies_no_timezone() {
    applyTimezone();

    // This is the bug in the two RTC -> system-clock seeds: both used mktime(), which reads
    // its fields as LOCAL time. Now that the chip holds UTC, mktime() would subtract the
    // offset from fields that never carried it. Prove the two really differ, by exactly the
    // offset in force, in both seasons.
    struct tm winter = {};
    winter.tm_year = 2026 - 1900; winter.tm_mon = 0; winter.tm_mday = 15;
    winter.tm_hour = 12; winter.tm_isdst = -1;
    TEST_ASSERT_EQUAL_INT64(3600, epochFromUtcFields(2026, 1, 15, 12, 0, 0)
                                  - static_cast<int64_t>(mktime(&winter)));

    struct tm summer = {};
    summer.tm_year = 2026 - 1900; summer.tm_mon = 6; summer.tm_mday = 15;
    summer.tm_hour = 12; summer.tm_isdst = -1;
    TEST_ASSERT_EQUAL_INT64(7200, epochFromUtcFields(2026, 7, 15, 12, 0, 0)
                                  - static_cast<int64_t>(mktime(&summer)));
}

void test_scheduler_clock_rejects_an_unset_rtc() {
    applyTimezone();

    // A DS3231 that lost its battery reads back some epoch near zero, and NTP may never run
    // on this installation. There is no trustworthy time, so the caller must suspend
    // schedules rather than act on a guess - valid = false is what makes it do that.
    TEST_ASSERT_FALSE(localWallClock(0).valid);
    TEST_ASSERT_FALSE(localWallClock(-1).valid);
    TEST_ASSERT_FALSE(localWallClock(epochFromUtcFields(2000, 1, 1, 0, 0, 0)).valid);
    TEST_ASSERT_FALSE(localWallClock(epochFromUtcFields(2019, 12, 31, 23, 59, 59)).valid);

    // The boundary is checked on the UTC epoch, not on the converted local year, so the
    // answer cannot depend on which side of New Year the offset lands.
    const WallClock first = localWallClock(epochFromUtcFields(MIN_PLAUSIBLE_YEAR, 1, 1, 0, 0, 0));
    TEST_ASSERT_TRUE(first.valid);
    TEST_ASSERT_EQUAL_INT(2020, first.year);
    TEST_ASSERT_EQUAL_UINT8(1, first.hour);  // 00:00 UTC -> 01:00 CET
}

void test_scheduler_clock_day_of_week_is_sunday_zero_and_local() {
    applyTimezone();

    // TimerSchedule::isDayEnabled() tests bit (dayOfWeek % 7) of dayMask, and the day bits
    // were written against RTClib's dayOfTheWeek() where Sunday is 0. tm_wday agrees, but
    // nothing in the type system says so - if this ever flips, every day-of-week schedule
    // silently shifts by a day.
    TEST_ASSERT_EQUAL_UINT8(0, localWallClock(FALL_2026).dayOfWeek);        // Sun 2026-10-25
    TEST_ASSERT_EQUAL_UINT8(0, localWallClock(SPRING_2026).dayOfWeek);      // Sun 2026-03-29
    TEST_ASSERT_EQUAL_UINT8(1, localWallClock(epochFromUtcFields(2026, 10, 26, 12, 0, 0)).dayOfWeek);
    TEST_ASSERT_EQUAL_UINT8(6, localWallClock(epochFromUtcFields(2026, 10, 24, 12, 0, 0)).dayOfWeek);

    // The day must roll at LOCAL midnight, not UTC midnight. 2026-01-05 23:30 UTC is already
    // Tuesday 00:30 CET, so a Monday-only schedule must have ended and a Tuesday-only one
    // begun - an hour before UTC agrees.
    const WallClock justBefore = localWallClock(epochFromUtcFields(2026, 1, 5, 22, 30, 0));
    const WallClock justAfter  = localWallClock(epochFromUtcFields(2026, 1, 5, 23, 30, 0));
    TEST_ASSERT_EQUAL_UINT8(1, justBefore.dayOfWeek);   // Mon 23:30 local
    TEST_ASSERT_EQUAL_UINT8(23, justBefore.hour);
    TEST_ASSERT_EQUAL_UINT8(2, justAfter.dayOfWeek);    // Tue 00:30 local
    TEST_ASSERT_EQUAL_UINT8(0, justAfter.hour);
}

void test_scheduler_clock_transitions_hold_for_later_years() {
    applyTimezone();

    // The rule is a rule, not a table, so it must keep working without a firmware update -
    // this controller is required to run with no internet, and a DST table would be the one
    // thing that silently expired. 2027 moves to March 28 / October 31.
    TEST_ASSERT_EQUAL_UINT8(1, localWallClock(1806195600 - 1).hour);  // 01:59 CET
    TEST_ASSERT_EQUAL_UINT8(3, localWallClock(1806195600).hour);      // 03:00 CEST
    TEST_ASSERT_EQUAL_UINT8(2, localWallClock(1824944400 - 1).hour);  // 02:59 CEST
    TEST_ASSERT_EQUAL_UINT8(2, localWallClock(1824944400).hour);      // 02:00 CET

    // And the switch is on the LAST Sunday, not simply the one after the 24th: in 2027 the
    // October switch is the 31st, so the 24th is still summer time.
    TEST_ASSERT_EQUAL_UINT8(14, localWallClock(epochFromUtcFields(2027, 10, 24, 12, 0, 0)).hour);
    TEST_ASSERT_EQUAL_UINT8(13, localWallClock(epochFromUtcFields(2027, 11, 1, 12, 0, 0)).hour);
}
