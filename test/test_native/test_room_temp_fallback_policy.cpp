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

// ---------------------------------------------------------------------------------------
// resolve() against the REAL SharedSensorReadings struct.
//
// The 2026-10-25 review's test-suite finding was that a native test of
// HeatingControlModule::resolveRoomForControl() "cannot even be written": the module needs
// FreeRTOS and SRP, and the copied MockSharedSensorReadings was missing insideTempGoodMs, the
// field the whole timestamp decision turns on. Both halves are now fixed - the decision moved
// into resolve(), and the real struct compiles natively - so here is that test.
// ---------------------------------------------------------------------------------------

#include "shared/SharedSensorReadings.h"

static RoomTempFallbackPolicy::Substitute resolveFrom(RoomTempFallbackPolicy::Sample& sample,
                                                      const SharedSensorReadings& r,
                                                      uint32_t nowMs, Temperature_t target) {
    return RoomTempFallbackPolicy::resolve(sample, r.isInsideTempValid, r.insideTemp,
                                           r.insideTempGoodMs, nowMs, target);
}

void test_room_resolve_credits_the_sensors_read_time_not_now() {
    // THE POINT OF insideTempGoodMs. isInsideTempValid stays true through up to three failed
    // reads (~15 s in ANDRTF3Task), so crediting millis() on every valid-looking cycle would
    // keep resetting the hold window against data that was already going stale - the 10-minute
    // hold would then start when the FLAG dropped, not at the last good sample, and run up to
    // 15 s long.
    SharedSensorReadings r;
    RoomTempFallbackPolicy::Sample sample;

    r.isInsideTempValid = true;
    r.insideTemp = 198;
    r.insideTempGoodMs = 1000;          // the sensor's last GOOD read
    (void)resolveFrom(sample, r, 16000, 192);   // ...observed 15 s later, flag still true
    TEST_ASSERT_EQUAL_UINT32(1000, sample.atMs);

    // So the hold expires 10 minutes after the READ, not after the observation.
    r.isInsideTempValid = false;
    const uint32_t lastHeld = 1000 + RoomTempFallbackPolicy::MAX_HOLD_MS;
    TEST_ASSERT_EQUAL(RoomTempFallbackPolicy::Source::HELD,
                      resolveFrom(sample, r, lastHeld, 192).source);
    TEST_ASSERT_EQUAL(RoomTempFallbackPolicy::Source::TARGET,
                      resolveFrom(sample, r, lastHeld + 1, 192).source);

    // Had it credited the observation time (16000) the hold would still be alive here - that
    // difference is exactly the defect this field exists to prevent.
    TEST_ASSERT_TRUE(16000u + RoomTempFallbackPolicy::MAX_HOLD_MS > lastHeld);
}

void test_room_resolve_falls_back_to_now_before_the_driver_ever_wrote() {
    // insideTempGoodMs == 0 means the sensor driver has never written it - at boot, or with an
    // older driver. Then there is nothing better than now, and it must NOT be read as "epoch
    // zero", which would make the sample instantly older than the hold window.
    SharedSensorReadings r;
    RoomTempFallbackPolicy::Sample sample;
    r.isInsideTempValid = true;
    r.insideTemp = 205;
    r.insideTempGoodMs = 0;

    const uint32_t now = 900000;  // well past MAX_HOLD_MS
    TEST_ASSERT_EQUAL(RoomTempFallbackPolicy::Source::MEASURED,
                      resolveFrom(sample, r, now, 192).source);
    TEST_ASSERT_EQUAL_UINT32(now, sample.atMs);

    r.isInsideTempValid = false;
    TEST_ASSERT_EQUAL(RoomTempFallbackPolicy::Source::HELD,
                      resolveFrom(sample, r, now + 1000, 192).source);
}

void test_room_resolve_walks_the_whole_ladder_on_a_pulled_sensor() {
    // The sequence actually observed on 2026-10-25 when the sensor was swapped: a good reading,
    // then the sensor gone. MEASURED -> HELD -> TARGET, and NOMINAL only when no target is set.
    SharedSensorReadings r;
    RoomTempFallbackPolicy::Sample sample;

    r.isInsideTempValid = true;
    r.insideTemp = 198;
    r.insideTempGoodMs = 50000;
    RoomTempFallbackPolicy::Substitute s = resolveFrom(sample, r, 50000, 192);
    TEST_ASSERT_EQUAL(RoomTempFallbackPolicy::Source::MEASURED, s.source);
    TEST_ASSERT_EQUAL_INT16(198, s.value);
    TEST_ASSERT_TRUE(RoomTempFallbackPolicy::isMeasurement(s.source));

    // Pulled. Inside the window the last real reading is used, and still counts as a
    // measurement - which is what keeps the weather-mode backstop armed.
    r.isInsideTempValid = false;
    s = resolveFrom(sample, r, 50000 + 60000, 192);
    TEST_ASSERT_EQUAL(RoomTempFallbackPolicy::Source::HELD, s.source);
    TEST_ASSERT_EQUAL_INT16(198, s.value);
    TEST_ASSERT_TRUE(RoomTempFallbackPolicy::isMeasurement(s.source));

    // Past the window it is a guess, and must NOT count as a measurement - this is the bit
    // that stops the curve applying a room-deviation shift to invented data.
    s = resolveFrom(sample, r, 50000 + RoomTempFallbackPolicy::MAX_HOLD_MS + 1, 192);
    TEST_ASSERT_EQUAL(RoomTempFallbackPolicy::Source::TARGET, s.source);
    TEST_ASSERT_EQUAL_INT16(192, s.value);
    TEST_ASSERT_FALSE(RoomTempFallbackPolicy::isMeasurement(s.source));

    // No target configured: the nominal 20.0 C, never TEMP_INVALID - feeding that into the
    // heating curve overflowed int32 and produced -755418.5 C on 2026-10-23.
    s = resolveFrom(sample, r, 50000 + RoomTempFallbackPolicy::MAX_HOLD_MS + 1, 0);
    TEST_ASSERT_EQUAL(RoomTempFallbackPolicy::Source::NOMINAL, s.source);
    TEST_ASSERT_EQUAL_INT16(200, s.value);

    // A recovered sensor goes straight back to MEASURED and re-arms the hold.
    r.isInsideTempValid = true;
    r.insideTemp = 201;
    r.insideTempGoodMs = 700000;
    s = resolveFrom(sample, r, 700000, 192);
    TEST_ASSERT_EQUAL(RoomTempFallbackPolicy::Source::MEASURED, s.source);
    TEST_ASSERT_EQUAL_UINT32(700000, sample.atMs);
}

void test_room_resolve_survives_the_millis_rollover() {
    // A hold that straddles the 49.7-day wrap. `atMs + MAX_HOLD_MS` would wrap and expire the
    // hold instantly; the unsigned difference does not.
    SharedSensorReadings r;
    RoomTempFallbackPolicy::Sample sample;
    const uint32_t beforeWrap = 0xFFFFFFFFu - 1000u;

    r.isInsideTempValid = true;
    r.insideTemp = 196;
    r.insideTempGoodMs = beforeWrap;
    TEST_ASSERT_EQUAL(RoomTempFallbackPolicy::Source::MEASURED,
                      resolveFrom(sample, r, beforeWrap, 192).source);

    r.isInsideTempValid = false;
    // 2 s later in real time, but a smaller number than the timestamp.
    const uint32_t afterWrap = beforeWrap + 2000u;  // wraps
    TEST_ASSERT_TRUE(afterWrap < beforeWrap);
    RoomTempFallbackPolicy::Substitute s = resolveFrom(sample, r, afterWrap, 192);
    TEST_ASSERT_EQUAL(RoomTempFallbackPolicy::Source::HELD, s.source);
    TEST_ASSERT_EQUAL_INT16(196, s.value);

    // And it still expires at the right moment on the far side of the wrap.
    TEST_ASSERT_EQUAL(RoomTempFallbackPolicy::Source::TARGET,
                      resolveFrom(sample, r, beforeWrap + RoomTempFallbackPolicy::MAX_HOLD_MS + 1,
                                  192).source);
}

void test_room_resolve_real_struct_has_the_field_the_mock_lacked() {
    // A guard, not a tautology: this suite used to compile against a hand-copied
    // MockSharedSensorReadings that did NOT have insideTempGoodMs, so every test above would
    // have been impossible to write and the resolver went untested through two defects. The
    // mock headers now forward to the real struct; if someone reintroduces a copy, the
    // assignment below stops compiling rather than silently testing fiction.
    SharedSensorReadings r;
    r.insideTempGoodMs = 12345;
    TEST_ASSERT_EQUAL_UINT32(12345, r.insideTempGoodMs);
    // Default-constructed, every field has a defined value - no garbage at startup.
    SharedSensorReadings fresh;
    TEST_ASSERT_EQUAL_UINT32(0, fresh.insideTempGoodMs);
    TEST_ASSERT_FALSE(fresh.isInsideTempValid);
    TEST_ASSERT_FALSE(fresh.isBoilerTempOutputValid);
}
