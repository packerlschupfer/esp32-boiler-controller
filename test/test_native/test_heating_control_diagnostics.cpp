/**
 * @file test_heating_control_diagnostics.cpp
 * @brief Unit tests for the boiler/status/heating/control payload (2026-10-18)
 */

#include <unity.h>
#include <cstdint>
#include <cstring>

#include "../../include/modules/control/HeatingControlDiagnostics.h"

// setUp and tearDown are defined in test_main.cpp

using namespace HeatingControlDiagnostics;

static State weatherFloorState() {
    State s;
    s.weatherMode = true; s.demand = true; s.block = Block::OK; s.clamp = Clamp::AT_FLOOR;
    s.rawTarget = 364; s.shift = 38; s.commanded = 400;
    s.outside = 130; s.room = 199; s.roomTarget = 192;
    return s;
}

void test_heating_diag_payload_is_the_agreed_schema() {
    char buf[160];
    const size_t n = format(buf, sizeof(buf), weatherFloorState());
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING(
        "{\"mode\":\"weather\",\"dem\":true,\"blk\":\"none\",\"raw\":364,\"shift\":38,"
        "\"cmd\":400,\"clp\":\"floor\",\"o\":130,\"i\":199,\"isrc\":\"meas\",\"tgt\":192}", buf);
    TEST_ASSERT_EQUAL_size_t(strlen(buf), n);
}

void test_heating_diag_fits_the_mqtt_payload_limit() {
    // MQTTTask's payload[320] REFUSES anything >= 320 since 2026-10-18, so an oversized
    // payload here becomes a dropped publish. Worst realistic case: every field wide and
    // negative, longest strings.
    State s;
    s.weatherMode = true; s.demand = false;
    s.block = Block::ROOM_BACKSTOP;      // "backstop", the longest block string
    s.clamp = Clamp::AT_FLOOR;
    s.rawTarget = -32768; s.shift = -2000; s.commanded = 32767;
    s.outside = -32768; s.room = -32768; s.roomTarget = -32768;
    s.roomSource = RoomTempFallbackPolicy::Source::MEASURED;  // "meas", the longest isrc
    char buf[160];
    const size_t n = format(buf, sizeof(buf), s);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE_MESSAGE(n < 319, "payload must fit MQTTTask's 319-byte usable payload");
}

void test_heating_diag_refuses_a_too_small_buffer_instead_of_truncating() {
    // A fragment of JSON is worse than no publish - the caller checks for 0.
    char small[32];
    TEST_ASSERT_EQUAL_size_t(0, format(small, sizeof(small), weatherFloorState()));
    TEST_ASSERT_EQUAL_size_t(0, format(nullptr, 0, weatherFloorState()));
}

void test_heating_diag_block_reasons_are_distinct_strings() {
    // The whole point of the topic: outside gate and room backstop must be
    // distinguishable, which no HA template could do.
    TEST_ASSERT_EQUAL_STRING("outside",  toString(Block::OUTSIDE_GATE));
    TEST_ASSERT_EQUAL_STRING("backstop", toString(Block::ROOM_BACKSTOP));
    TEST_ASSERT_EQUAL_STRING("roomsat",  toString(Block::ROOM_SATISFIED));
    TEST_ASSERT_EQUAL_STRING("sensor",   toString(Block::SENSOR_INVALID));
    TEST_ASSERT_EQUAL_STRING("override", toString(Block::SUMMER_OVERRIDE));
    TEST_ASSERT_EQUAL_STRING("disabled", toString(Block::HEATING_DISABLED));
    TEST_ASSERT_EQUAL_STRING("none",     toString(Block::OK));
}

void test_heating_diag_room_mode_and_negative_raw_serialise() {
    // Mild weather in room mode: the curve wants far below the floor, which is exactly
    // what the clamped value hides and this topic exists to expose.
    State s;
    s.weatherMode = false; s.demand = false; s.block = Block::ROOM_SATISFIED;
    s.clamp = Clamp::AT_FLOOR;
    s.rawTarget = -205; s.shift = -200; s.commanded = 400;
    s.outside = 211; s.room = 205; s.roomTarget = 192;
    char buf[160];
    TEST_ASSERT_TRUE(format(buf, sizeof(buf), s) > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"mode\":\"room\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"raw\":-205"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"blk\":\"roomsat\""));
}

void test_heating_diag_differs_detects_every_field() {
    const State base = weatherFloorState();
    TEST_ASSERT_FALSE(differs(base, base));
    State t;
    t = base; t.weatherMode = false;            TEST_ASSERT_TRUE(differs(base, t));
    t = base; t.demand = false;                 TEST_ASSERT_TRUE(differs(base, t));
    t = base; t.block = Block::OUTSIDE_GATE;    TEST_ASSERT_TRUE(differs(base, t));
    t = base; t.clamp = Clamp::NO_CLAMP;        TEST_ASSERT_TRUE(differs(base, t));
    t = base; t.rawTarget += 1;                 TEST_ASSERT_TRUE(differs(base, t));
    t = base; t.shift += 1;                     TEST_ASSERT_TRUE(differs(base, t));
    t = base; t.commanded += 1;                 TEST_ASSERT_TRUE(differs(base, t));
    t = base; t.outside += 1;                   TEST_ASSERT_TRUE(differs(base, t));
    t = base; t.room += 1;                      TEST_ASSERT_TRUE(differs(base, t));
    t = base; t.roomTarget += 1;                TEST_ASSERT_TRUE(differs(base, t));
    t = base; t.roomSource = RoomTempFallbackPolicy::Source::HELD;
                                                TEST_ASSERT_TRUE(differs(base, t));
}

void test_heating_diag_reports_a_substituted_room_reading_as_such() {
    // A held or assumed room value is what `raw` and `shift` were computed from, so the
    // topic publishes it in `i` - and `isrc` is what stops that from being a quiet lie.
    State s = weatherFloorState();
    s.room = 199; s.roomSource = RoomTempFallbackPolicy::Source::HELD;
    char buf[160];
    TEST_ASSERT_TRUE(format(buf, sizeof(buf), s) > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"i\":199,\"isrc\":\"held\""));

    s.room = 192; s.roomSource = RoomTempFallbackPolicy::Source::TARGET;
    TEST_ASSERT_TRUE(format(buf, sizeof(buf), s) > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"isrc\":\"tgt\""));

    s.room = 200; s.roomSource = RoomTempFallbackPolicy::Source::NOMINAL;
    TEST_ASSERT_TRUE(format(buf, sizeof(buf), s) > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"isrc\":\"nom\""));

    // All four strings distinct, or a subscriber cannot tell the cases apart
    TEST_ASSERT_EQUAL_STRING("meas", RoomTempFallbackPolicy::toString(RoomTempFallbackPolicy::Source::MEASURED));
    TEST_ASSERT_EQUAL_STRING("held", RoomTempFallbackPolicy::toString(RoomTempFallbackPolicy::Source::HELD));
    TEST_ASSERT_EQUAL_STRING("tgt",  RoomTempFallbackPolicy::toString(RoomTempFallbackPolicy::Source::TARGET));
    TEST_ASSERT_EQUAL_STRING("nom",  RoomTempFallbackPolicy::toString(RoomTempFallbackPolicy::Source::NOMINAL));
}
