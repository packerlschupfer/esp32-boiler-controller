/**
 * @file test_room_temp_fallback_policy.cpp
 * @brief Unit tests for RoomTempFallbackPolicy (hold the last good room reading, 2026-10-23)
 */

#include <unity.h>
#include <cstdint>

#include "../../include/modules/control/RoomTempFallbackPolicy.h"

// setUp and tearDown are defined in test_main.cpp

using RoomTempFallbackPolicy::Sample;
using RoomTempFallbackPolicy::Source;
using RoomTempFallbackPolicy::Substitute;
using RoomTempFallbackPolicy::record;
using RoomTempFallbackPolicy::holdUsable;
using RoomTempFallbackPolicy::substitute;
using RoomTempFallbackPolicy::isMeasurement;
using RoomTempFallbackPolicy::MAX_HOLD_MS;

static constexpr Temperature_t ROOM_TARGET = 192;  // 19.2, the deployed value

void test_room_fallback_holds_the_last_reading_through_a_short_dropout() {
    // The failure that actually happens: 2026-10-23 08:34, the room sensor dropped out for
    // about 5 s mid-cycle. Holding the reading makes the curve output identical to the
    // previous cycle's, so the flow setpoint does not move at all.
    Sample s;
    record(s, 197, 100000);                       // 19.7 at t = 100 s
    const Substitute sub = substitute(s, 105000, ROOM_TARGET);  // 5 s later
    TEST_ASSERT_EQUAL_INT16(197, sub.value);
    TEST_ASSERT_TRUE(sub.source == Source::HELD);
    TEST_ASSERT_TRUE(isMeasurement(sub.source));  // so the room-deviation shift still applies
}

void test_room_fallback_expires_the_hold_and_falls_back_to_target() {
    // A held reading is a measurement with an age. Past MAX_HOLD_MS the room has had time
    // to move and the nominal line becomes the better guess - and there is no room feedback
    // to notice the bias, which is why this expires at all.
    Sample s;
    record(s, 205, 1000);
    TEST_ASSERT_TRUE(holdUsable(s, 1000 + MAX_HOLD_MS));          // inclusive at the edge
    TEST_ASSERT_FALSE(holdUsable(s, 1000 + MAX_HOLD_MS + 1));

    const Substitute justInside = substitute(s, 1000 + MAX_HOLD_MS, ROOM_TARGET);
    TEST_ASSERT_EQUAL_INT16(205, justInside.value);
    TEST_ASSERT_TRUE(justInside.source == Source::HELD);

    const Substitute expired = substitute(s, 1000 + MAX_HOLD_MS + 1, ROOM_TARGET);
    TEST_ASSERT_EQUAL_INT16(ROOM_TARGET, expired.value);
    TEST_ASSERT_TRUE(expired.source == Source::TARGET);
    TEST_ASSERT_FALSE(isMeasurement(expired.source));  // deviation is zero by construction
}

void test_room_fallback_with_nothing_remembered_uses_target_then_nominal() {
    // Sensor dead from boot: nothing has ever been recorded.
    Sample never;
    TEST_ASSERT_FALSE(holdUsable(never, 0));
    TEST_ASSERT_FALSE(holdUsable(never, 999999));

    const Substitute toTarget = substitute(never, 50000, ROOM_TARGET);
    TEST_ASSERT_EQUAL_INT16(ROOM_TARGET, toTarget.value);
    TEST_ASSERT_TRUE(toTarget.source == Source::TARGET);

    // No room target either (0 or negative = unset)
    const Substitute toNominal = substitute(never, 50000, 0);
    TEST_ASSERT_EQUAL_INT16(HeatingCurve::NOMINAL_ROOM_TENTHS, toNominal.value);
    TEST_ASSERT_TRUE(toNominal.source == Source::NOMINAL);
    TEST_ASSERT_EQUAL_INT16(200, toNominal.value);

    const Substitute negTarget = substitute(never, 50000, -50);
    TEST_ASSERT_TRUE(negTarget.source == Source::NOMINAL);
}

void test_room_fallback_survives_the_millis_rollover() {
    // millis() wraps every 49.7 days. The age is computed as an unsigned difference, so a
    // wrap mid-dropout still gives the true age; computing `atMs + MAX_HOLD_MS` instead
    // would overflow and hand back a reading up to 49 days old as if it were fresh.
    Sample s;
    record(s, 201, 0xFFFFFF00u);                  // recorded just before the wrap
    const uint32_t afterWrap = 0x00000100u;       // 512 ms later in real time
    TEST_ASSERT_TRUE(holdUsable(s, afterWrap));
    const Substitute sub = substitute(s, afterWrap, ROOM_TARGET);
    TEST_ASSERT_EQUAL_INT16(201, sub.value);
    TEST_ASSERT_TRUE(sub.source == Source::HELD);

    // And it still expires on the far side of the wrap
    TEST_ASSERT_FALSE(holdUsable(s, 0xFFFFFF00u + MAX_HOLD_MS + 1));
}

void test_room_fallback_recording_a_new_reading_refreshes_the_age() {
    Sample s;
    record(s, 190, 1000);
    record(s, 196, 1000 + MAX_HOLD_MS);           // a fresh read just before expiry
    TEST_ASSERT_TRUE(holdUsable(s, 1000 + MAX_HOLD_MS + 5000));
    const Substitute sub = substitute(s, 1000 + MAX_HOLD_MS + 5000, ROOM_TARGET);
    TEST_ASSERT_EQUAL_INT16(196, sub.value);      // the newer value, not the older one
}

void test_room_fallback_never_returns_an_invalid_marker() {
    // The whole reason this exists: TEMP_INVALID (INT16_MIN) reaching HeatingCurve overflowed
    // the polynomial. No path may hand one back, including an un-recorded sample whose
    // default value IS the invalid marker.
    Sample never;
    TEST_ASSERT_EQUAL_INT16(TEMP_INVALID, never.value);  // the trap this guards
    TEST_ASSERT_TRUE(substitute(never, 0, ROOM_TARGET).value != TEMP_INVALID);
    TEST_ASSERT_TRUE(substitute(never, 0, 0).value != TEMP_INVALID);
    Sample stale;
    record(stale, 195, 0);
    TEST_ASSERT_TRUE(substitute(stale, MAX_HOLD_MS * 10, 0).value != TEMP_INVALID);

    // A plausibility floor/ceiling is the sensor driver's job, but hold must at least not
    // resurrect a value the driver already rejected: only valid readings are recorded, and
    // substitute() reads nothing else.
    const Substitute held = substitute(stale, 1000, ROOM_TARGET);
    TEST_ASSERT_EQUAL_INT16(195, held.value);
}
