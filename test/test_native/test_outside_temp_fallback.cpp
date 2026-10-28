// test/test_native/test_outside_temp_fallback.cpp
// OutsideTempFallbackPolicy + SpaceHeatingPolicy::roomThermostatWantsHeat.
//
// The behaviour under test is the one a review called the worst non-latent defect in the
// system: in weather mode a dead outside sensor used to stop space heating COMPLETELY for as
// long as the hardware fault lasted, letting an occupied winter house settle into the 5-10 C
// frost band while a healthy room sensor and a valid room target sat unused.

#include <unity.h>
#include "modules/control/OutsideTempFallbackPolicy.h"
#include "modules/control/SpaceHeatingPolicy.h"

using OutsideTempFallbackPolicy::Sample;
using OutsideTempFallbackPolicy::Source;
using OutsideTempFallbackPolicy::resolve;
using OutsideTempFallbackPolicy::curveUsable;
using OutsideTempFallbackPolicy::MAX_HOLD_MS;

static constexpr int16_t TARGET = 194;   // 19.4 C
static constexpr int16_t HYST   = 5;     // 0.5 K

void test_outside_valid_reading_is_used_and_recorded() {
    Sample s;
    const auto d = resolve(s, true, 59, 1000, true, TARGET);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Source::MEASURED), static_cast<int>(d.source));
    TEST_ASSERT_EQUAL_INT16(59, d.outside);
    TEST_ASSERT_TRUE(curveUsable(d.source));
    // Recorded, so the next invalid cycle can hold it.
    TEST_ASSERT_TRUE(s.valid);
    TEST_ASSERT_EQUAL_INT16(59, s.value);
}

void test_outside_dropout_holds_the_last_reading_so_the_curve_keeps_working() {
    // A bus glitch must change NOTHING. The MB8ART regains validity on the first in-range
    // read, so most dropouts are seconds long.
    Sample s;
    (void)resolve(s, true, 59, 1000, true, TARGET);
    const auto d = resolve(s, false, 0, 1000 + 5000, true, TARGET);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Source::HELD), static_cast<int>(d.source));
    TEST_ASSERT_EQUAL_INT16(59, d.outside);     // the held value, not 0
    TEST_ASSERT_TRUE(curveUsable(d.source));    // full weather compensation continues
}

void test_outside_hold_lasts_an_hour_then_expires() {
    Sample s;
    (void)resolve(s, true, 59, 1000, true, TARGET);
    // Just inside the hold.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Source::HELD),
        static_cast<int>(resolve(s, false, 0, 1000 + MAX_HOLD_MS, true, TARGET).source));
    // One millisecond past it.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Source::ROOM_THERMOSTAT),
        static_cast<int>(resolve(s, false, 0, 1000 + MAX_HOLD_MS + 1, true, TARGET).source));
}

void test_outside_gone_falls_back_to_the_room_not_to_no_heat() {
    // THE DEFECT. Past the hold, with a real room reading and a configured target, the answer
    // must be "control on the room", never "stop heating".
    Sample s;
    (void)resolve(s, true, 59, 0, true, TARGET);
    const auto d = resolve(s, false, 0, MAX_HOLD_MS + 1, /*roomIsMeasurement=*/true, TARGET);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Source::ROOM_THERMOSTAT), static_cast<int>(d.source));
    TEST_ASSERT_FALSE(curveUsable(d.source));   // but the CURVE cannot be evaluated
}

void test_outside_gone_and_no_usable_room_is_the_only_no_heat_case() {
    Sample s;  // never had a reading
    // No room measurement.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Source::UNAVAILABLE),
        static_cast<int>(resolve(s, false, 0, 10000, /*roomIsMeasurement=*/false, TARGET).source));
    // Room measurement but no configured target: nothing to control on.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Source::UNAVAILABLE),
        static_cast<int>(resolve(s, false, 0, 10000, true, /*roomTarget=*/0).source));
}

void test_outside_hold_survives_the_millis_rollover() {
    // Unsigned subtraction, same reasoning as RoomTempFallbackPolicy: a reading taken just
    // before the 49.7-day wrap must not read as an hour old just after it.
    Sample s;
    const uint32_t beforeWrap = 0xFFFFFF00u;
    (void)resolve(s, true, 59, beforeWrap, true, TARGET);
    const uint32_t afterWrap = beforeWrap + 5000;  // wraps through zero
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Source::HELD),
        static_cast<int>(resolve(s, false, 0, afterWrap, true, TARGET).source));
}

void test_room_thermostat_predicate_is_asymmetric() {
    using SpaceHeatingPolicy::roomThermostatWantsHeat;
    // OFF: start only below the target.
    TEST_ASSERT_TRUE (roomThermostatWantsHeat(false, TARGET - 1, TARGET, HYST));
    TEST_ASSERT_FALSE(roomThermostatWantsHeat(false, TARGET,     TARGET, HYST));
    // ON: keep going until target + hysteresis.
    TEST_ASSERT_TRUE (roomThermostatWantsHeat(true,  TARGET,            TARGET, HYST));
    TEST_ASSERT_TRUE (roomThermostatWantsHeat(true,  TARGET + HYST - 1, TARGET, HYST));
    TEST_ASSERT_FALSE(roomThermostatWantsHeat(true,  TARGET + HYST,     TARGET, HYST));
    // The asymmetry is the point: at exactly the target the answer DEPENDS on the state,
    // which is what stops it chattering at one threshold.
    TEST_ASSERT_NOT_EQUAL(roomThermostatWantsHeat(false, TARGET, TARGET, HYST),
                          roomThermostatWantsHeat(true,  TARGET, TARGET, HYST));
}

void test_room_thermostat_fallback_would_have_heated_the_house() {
    using SpaceHeatingPolicy::roomThermostatWantsHeat;
    // The concrete scenario: outside sensor dead for hours on a winter night, room falling.
    // Old behaviour: heating off unconditionally, house drifts to the 5 C frost trigger.
    // New behaviour: the thermostat asks for heat as soon as the room is below target.
    Sample s;
    (void)resolve(s, true, 20, 0, true, TARGET);
    const auto d = resolve(s, false, 0, MAX_HOLD_MS * 4, true, TARGET);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Source::ROOM_THERMOSTAT), static_cast<int>(d.source));
    TEST_ASSERT_TRUE(roomThermostatWantsHeat(false, 180, TARGET, HYST));  // 18.0 < 19.4 -> heat
    TEST_ASSERT_TRUE(roomThermostatWantsHeat(false,  60, TARGET, HYST));  // and certainly at 6.0
}
