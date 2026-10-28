// test/test_native/test_room_sensor_register_report.cpp
// RoomSensorRegisterReport::format() - the ANDRTF3 open firmware's own counters as one payload.
//
// The point of these tests is the two ways this report can lie: a zero that is really an
// absent key (so "no failures" must be distinguishable from "no answer"), and a payload that
// silently loses its tail at 320 B (so a truncated report must say it is truncated).

#include <unity.h>
#include <cstring>
#include <cstdio>

#include "modules/control/RoomSensorRegisterReport.h"

using RoomSensorRegisterReport::Snapshot;
using RoomSensorRegisterReport::format;

// The real budget: MQTTPublishRequest::payload is 320 bytes.
static constexpr size_t PAYLOAD = 320;

static Snapshot healthySnapshot() {
    Snapshot s;
    s.available = true;
    s.ok = true;
    s.openFirmware = true;
    s.firmwareKnown = true;
    s.firmwareVersion = 0x8001;
    s.uptimeSeconds = 74000;
    s.goodReadings = 61000;
    s.failedReadings = 0;
    s.runningSlot = 1;   // B
    s.resetCause = 0x0C; // power-on
    s.status = 0x0001;
    return s;
}

void test_report_no_device_is_not_a_zero_reading() {
    Snapshot s;  // available = false
    char buf[PAYLOAD];
    const size_t n = format(buf, sizeof(buf), s);
    TEST_ASSERT_GREATER_THAN(0u, n);
    // Must NOT look like "zero failures" - an absent sensor is a different answer.
    TEST_ASSERT_NOT_NULL(strstr(buf, "no_device"));
    TEST_ASSERT_NULL(strstr(buf, "\"fail\""));
}

void test_report_distinguishes_stock_firmware_from_a_failed_read() {
    char buf[PAYLOAD];

    // THREE distinct reasons, and conflating two of them was a real defect: a sensor that
    // answered NOTHING used to be reported as "not_open_firmware", which the docs describe as
    // "the stock Andivi firmware answered" - a positive identification of a unit that was not
    // there. Fixed 2026-10-28; these assertions are what keep them apart.

    // (a) Stock firmware POSITIVELY identified: detection succeeded and said ORIGINAL.
    Snapshot stock;
    stock.available = true;
    stock.ok = false;
    stock.firmwareKnown = true;
    stock.openFirmware = false;
    stock.firmwareVersion = 7;  // the Andivi firmware seen on these units
    TEST_ASSERT_GREATER_THAN(0u, format(buf, sizeof(buf), stock));
    TEST_ASSERT_NOT_NULL(strstr(buf, "not_open_firmware"));

    // (b) Nothing answered at all: absent, mid-reflash, or a broken RS485 pair.
    Snapshot dark;
    dark.available = true;
    dark.ok = false;
    dark.firmwareKnown = false;   // detection itself got no reply
    dark.openFirmware = false;
    TEST_ASSERT_GREATER_THAN(0u, format(buf, sizeof(buf), dark));
    TEST_ASSERT_NOT_NULL(strstr(buf, "no_answer"));
    TEST_ASSERT_NULL(strstr(buf, "not_open_firmware"));   // must NOT claim a stock unit

    // (c) Open firmware identified, but the register read itself failed.
    Snapshot failed;
    failed.available = true;
    failed.ok = false;
    failed.firmwareKnown = true;
    failed.openFirmware = true;
    failed.firmwareVersion = 0x8001;
    TEST_ASSERT_GREATER_THAN(0u, format(buf, sizeof(buf), failed));
    TEST_ASSERT_NOT_NULL(strstr(buf, "read_failed"));
    // The firmware version survives a failed read - it comes from a different register.
    TEST_ASSERT_NOT_NULL(strstr(buf, "0x8001"));
}

void test_report_publishes_negative_temperatures_signed() {
    // `tc` is register 57, a SIGNED int16 in 0.01 C. It was published through a uint16 cast,
    // so every sub-zero reading came out as 65536+value: -30.00 C read 62536 and -0.01 C read
    // 65535. The sensor accepts 500..3000 Ohm, which reaches far below freezing, so the input
    // is reachable on any cold night. The one test that set a negative value asserted nothing
    // about this key, which is exactly why it shipped. Found 2026-10-28.
    Snapshot s = healthySnapshot();
    char buf[PAYLOAD];

    s.temperatureCenti = -3000;                 // -30.00 C
    TEST_ASSERT_GREATER_THAN(0u, format(buf, sizeof(buf), s));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"tc\":-3000"));
    TEST_ASSERT_NULL(strstr(buf, "62536"));     // the old wrong rendering

    s.temperatureCenti = -1;                    // -0.01 C, the subtlest case
    TEST_ASSERT_GREATER_THAN(0u, format(buf, sizeof(buf), s));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"tc\":-1"));
    TEST_ASSERT_NULL(strstr(buf, "65535"));

    s.temperatureCenti = 2207;                  // and positives still render plainly
    TEST_ASSERT_GREATER_THAN(0u, format(buf, sizeof(buf), s));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"tc\":2207"));
}

void test_report_always_emits_zero_failures_explicitly() {
    // The whole question this report answers is "how many raw failures". Zero is the ANSWER,
    // so it must be present as a key - unlike the per-reason counters, where an absent key
    // reading as zero is fine.
    Snapshot s = healthySnapshot();
    char buf[PAYLOAD];
    TEST_ASSERT_GREATER_THAN(0u, format(buf, sizeof(buf), s));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"fail\":0"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"good\":61000"));
    // Zero per-reason counters are omitted.
    TEST_ASSERT_NULL(strstr(buf, "\"adct\""));
    TEST_ASSERT_NULL(strstr(buf, "\"rng\""));
    TEST_ASSERT_NULL(strstr(buf, "\"bnoi\""));
}

void test_report_carries_the_firmware_slot_and_uptime() {
    // Without these three the counters cannot be attributed: to which firmware build, which
    // slot, and over what period. Getting that wrong is what cost an out-of-band question.
    Snapshot s = healthySnapshot();
    char buf[PAYLOAD];
    TEST_ASSERT_GREATER_THAN(0u, format(buf, sizeof(buf), s));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"fw\":\"0x8001\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"slot\":\"B\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"up\":74000"));

    s.runningSlot = 0;
    TEST_ASSERT_GREATER_THAN(0u, format(buf, sizeof(buf), s));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"slot\":\"A\""));
}

void test_report_failure_breakdown_is_published() {
    Snapshot s = healthySnapshot();
    s.failedReadings = 7;
    s.adcTimeouts = 5;
    s.rangeErrors = 2;
    s.lastFailReason = 1;
    char buf[PAYLOAD];
    TEST_ASSERT_GREATER_THAN(0u, format(buf, sizeof(buf), s));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"fail\":7"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"adct\":5"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"rng\":2"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"lastfail\":1"));
    TEST_ASSERT_NULL(strstr(buf, "\"ovf\""));  // still zero
}

void test_report_fits_the_payload_with_every_counter_set() {
    // All 24 optional fields non-zero and wide. This MUST stay inside 320 B or be marked
    // truncated - never silently cut, which would be a diagnostic that reads as complete.
    Snapshot s = healthySnapshot();
    s.failedReadings = 65535;
    s.lastFailReason = 3;
    s.adcTimeouts = 65535;
    s.rangeErrors = 65535;
    s.adcOverflows = 65535;
    s.secondsSinceGood = 65535;
    s.status = 0xFFFF;
    s.resistance = 65535;
    s.temperatureCenti = -3000;
    s.busFrames = 65535;
    s.busHandled = 65535;
    s.busCrcErrors = 65535;
    s.busBadLength = 65535;
    s.busFramingErrors = 65535;
    s.busParityErrors = 65535;
    s.busNoiseErrors = 65535;
    s.busOverruns = 65535;
    s.busExceptions = 65535;
    s.resetCause = 0xFFFF;
    s.bootloaderVersion = 65535;

    char buf[PAYLOAD];
    const size_t n = format(buf, sizeof(buf), s);
    TEST_ASSERT_GREATER_THAN(0u, n);
    TEST_ASSERT_LESS_THAN(PAYLOAD, n);
    TEST_ASSERT_EQUAL_CHAR('}', buf[n - 1]);          // always closed
    TEST_ASSERT_EQUAL_UINT(n, (unsigned)strlen(buf)); // and NUL-terminated at that length
    // The fields that answer the question come first, so they survive any truncation.
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"fail\":65535"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"adct\":65535"));
}

void test_report_marks_truncation_instead_of_losing_the_tail() {
    Snapshot s = healthySnapshot();
    s.busFrames = 65535;
    s.busHandled = 65535;
    s.busCrcErrors = 65535;
    s.busExceptions = 65535;
    s.bootloaderVersion = 1;

    // A buffer deliberately too small for the optional fields but big enough for the prefix.
    char buf[80];
    const size_t n = format(buf, sizeof(buf), s);
    TEST_ASSERT_GREATER_THAN(0u, n);
    TEST_ASSERT_LESS_THAN(sizeof(buf), n);
    TEST_ASSERT_EQUAL_CHAR('}', buf[n - 1]);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"trunc\":1"));
    // The prefix is intact: the report still says which firmware and how many failures.
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"fw\":\"0x8001\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"fail\":0"));
}

void test_report_refuses_rather_than_emit_half_a_payload() {
    // Too small even for the prefix: 0, so the caller publishes nothing. Publishing a
    // fragment of JSON is worse than publishing nothing.
    Snapshot s = healthySnapshot();
    char buf[12];
    TEST_ASSERT_EQUAL_UINT(0u, format(buf, sizeof(buf), s));
    TEST_ASSERT_EQUAL_CHAR('\0', buf[0]);

    TEST_ASSERT_EQUAL_UINT(0u, format(nullptr, sizeof(buf), s));
    TEST_ASSERT_EQUAL_UINT(0u, format(buf, 0, s));
}
