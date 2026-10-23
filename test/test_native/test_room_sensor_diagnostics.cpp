/**
 * @file test_room_sensor_diagnostics.cpp
 * @brief Unit tests for the boiler/status/sensors/room payload (2026-10-24)
 */

#include <unity.h>
#include <cstdint>
#include <cstring>

#include "../../include/modules/control/RoomSensorDiagnostics.h"

// setUp and tearDown are defined in test_main.cpp

using namespace RoomSensorDiagnostics;

static State healthy() {
    State s;
    s.available = true;
    s.counts[0] = 17280;        // ~24 h of successful reads at 5 s
    s.lastGoodAgeMs = 3000;
    s.lastKind = 0;
    return s;
}

void test_room_diag_healthy_payload_is_tiny_and_has_no_error_keys() {
    // The steady state is the common case, so it must stay small and say "no failures" by
    // the ABSENCE of the keys rather than by a row of zeros.
    char buf[200];
    const size_t n = format(buf, sizeof(buf), healthy());
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING("{\"ok\":17280,\"age\":3,\"last\":\"ok\"}", buf);
    TEST_ASSERT_TRUE_MESSAGE(n < 40, "healthy payload should stay tiny");
    TEST_ASSERT_NULL(strstr(buf, "busy"));
    TEST_ASSERT_NULL(strstr(buf, "crc"));
}

void test_room_diag_reports_the_contention_hypothesis() {
    // The whole point: after the next dropout, `busy` either confirms BUS_BUSY (the
    // maintainer's hypothesis for 2026-10-23 08:34) or kills it.
    State s = healthy();
    s.counts[1] = 3;            // BUS_BUSY
    s.lastKind = 1;
    s.lastGoodAgeMs = 15000;
    char buf[200];
    TEST_ASSERT_TRUE(format(buf, sizeof(buf), s) > 0);
    TEST_ASSERT_EQUAL_STRING("{\"ok\":17280,\"busy\":3,\"age\":15,\"last\":\"busy\"}", buf);
}

void test_room_diag_age_is_seconds_and_omitted_when_never_read() {
    State s = healthy();
    s.lastGoodAgeMs = 95000;    // 95 s
    char buf[200];
    TEST_ASSERT_TRUE(format(buf, sizeof(buf), s) > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"age\":95"));

    // Never read successfully: no age key at all, rather than a misleading 4294967 s
    s.lastGoodAgeMs = UINT32_MAX;
    s.counts[0] = 0;
    TEST_ASSERT_TRUE(format(buf, sizeof(buf), s) > 0);
    TEST_ASSERT_NULL(strstr(buf, "age"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ok\":0"));
}

void test_room_diag_worst_case_fits_the_mqtt_payload_limit() {
    // MQTTTask REFUSES a payload >= 320 B rather than truncating, so an oversized payload
    // becomes a dropped publish. Every kind non-zero with ten-digit counts.
    State s;
    s.available = true;
    for (size_t i = 0; i < KIND_KEY_COUNT; ++i) {
        s.counts[i] = 4294967295u;
    }
    s.lastGoodAgeMs = 4294967000u;
    s.lastKind = static_cast<uint8_t>(KIND_KEY_COUNT - 1);
    char buf[400];
    const size_t n = format(buf, sizeof(buf), s);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE_MESSAGE(n < 319, "payload must fit MQTTTask's usable payload");
}

void test_room_diag_refuses_a_too_small_buffer_instead_of_truncating() {
    // A fragment of JSON is worse than no publish - the caller checks for 0. Each snprintf
    // step has to be checked, not just the first.
    State s = healthy();
    s.counts[1] = 7; s.counts[2] = 9;
    char tiny[12];
    TEST_ASSERT_EQUAL_size_t(0, format(tiny, sizeof(tiny), s));
    char mid[28];                       // enough for "ok" but not the rest
    TEST_ASSERT_EQUAL_size_t(0, format(mid, sizeof(mid), s));
    TEST_ASSERT_EQUAL_size_t(0, format(nullptr, 0, s));
}

void test_room_diag_differs_ignores_age_and_the_success_count() {
    // Both move on every single read, so including either turns "on change" into "every
    // cycle". Measured on hardware 2026-10-24: with the success count in, this topic
    // published every 5 s - ~17 000 retained publishes a day for a number nobody watches
    // live. Every ERROR counter must still trigger immediately.
    const State base = healthy();
    TEST_ASSERT_FALSE(differs(base, base));

    State t = base;
    t.lastGoodAgeMs += 60000;
    TEST_ASSERT_FALSE_MESSAGE(differs(base, t), "age alone must not trigger a publish");

    t = base;
    t.counts[0] += 1;
    TEST_ASSERT_FALSE_MESSAGE(differs(base, t), "a successful read must not trigger a publish");

    for (size_t i = 1; i < KIND_KEY_COUNT; ++i) {
        t = base;
        t.counts[i] += 1;
        TEST_ASSERT_TRUE_MESSAGE(differs(base, t), "every error kind must trigger a publish");
    }
    t = base; t.lastKind = 3;        TEST_ASSERT_TRUE(differs(base, t));
    t = base; t.available = false;   TEST_ASSERT_TRUE(differs(base, t));

    // The success count still reaches the wire, it just does not force the publish
    char buf[200];
    TEST_ASSERT_TRUE(format(buf, sizeof(buf), base) > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ok\":17280"));
}

void test_room_diag_key_table_matches_the_state_array() {
    // The keys are a hand-kept mirror of the library's ReadError order, so pin the length.
    // ANDRTF3Task.cpp carries a static_assert against the real enum; this catches a key
    // being added or removed here without the array growing.
    TEST_ASSERT_EQUAL_size_t(9, KIND_KEY_COUNT);
    TEST_ASSERT_EQUAL_STRING("ok", KIND_KEYS[0]);
    TEST_ASSERT_EQUAL_STRING("busy", KIND_KEYS[1]);
    TEST_ASSERT_EQUAL_STRING("oth", KIND_KEYS[KIND_KEY_COUNT - 1]);
    State s;
    TEST_ASSERT_EQUAL_size_t(KIND_KEY_COUNT, sizeof(s.counts) / sizeof(s.counts[0]));
}
