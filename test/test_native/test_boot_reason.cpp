/**
 * @file test_boot_reason.cpp
 * @brief Tests for BootReason - the reset-reason labelling added 2026-10-30.
 *
 * The firmware had no reset-reason reporting at all, so a restart during a review
 * session could not be explained. These pin the classification, because the whole
 * value of the facility is that the label is trustworthy when something does go
 * wrong at 03:00.
 */

#include <unity.h>
#include <cstring>
#include <cstdint>

#include "../../include/modules/diagnostics/BootReason.h"

// setUp and tearDown are defined in test_main.cpp

void test_boot_reason_names_every_code() {
    // No code may fall through to "invalid" - that string exists only for values
    // outside the enum.
    for (uint8_t c = BootReason::UNKNOWN; c <= BootReason::CPU_LOCKUP; ++c) {
        TEST_ASSERT_NOT_NULL(BootReason::name(c));
        TEST_ASSERT_FALSE_MESSAGE(strcmp(BootReason::name(c), "invalid") == 0,
                                  "a real enumerator must have its own label");
    }
    TEST_ASSERT_EQUAL_STRING("invalid", BootReason::name(200));
}

void test_boot_reason_commanded_restarts_are_not_abnormal() {
    // Every OTA and every cmd/diagnostics reboot lands here. Flagging them would
    // make the abnormal signal useless in the week you reboot most.
    TEST_ASSERT_FALSE_MESSAGE(BootReason::isAbnormal(BootReason::SW),
                              "esp_restart() is how we reboot on purpose");
    TEST_ASSERT_FALSE_MESSAGE(BootReason::isAbnormal(BootReason::POWERON),
                              "a power cut is a fact about the house, not a firmware fault");
}

void test_boot_reason_faults_are_abnormal() {
    TEST_ASSERT_TRUE(BootReason::isAbnormal(BootReason::PANIC));
    TEST_ASSERT_TRUE(BootReason::isAbnormal(BootReason::INT_WDT));
    TEST_ASSERT_TRUE(BootReason::isAbnormal(BootReason::TASK_WDT));
    TEST_ASSERT_TRUE(BootReason::isAbnormal(BootReason::WDT));
    TEST_ASSERT_TRUE_MESSAGE(BootReason::isAbnormal(BootReason::BROWNOUT),
                             "brownout is the one that looks like a power cut but is not");
    TEST_ASSERT_TRUE(BootReason::isAbnormal(BootReason::CPU_LOCKUP));
}

void test_boot_reason_unknown_is_abnormal() {
    // The case the facility was added for. Defaulting it to "fine" would reproduce
    // the silence it exists to break.
    TEST_ASSERT_TRUE_MESSAGE(BootReason::isAbnormal(BootReason::UNKNOWN),
                             "an unexplained restart must not read as healthy");
}

void test_boot_reason_payload_shape() {
    char buf[96];
    const size_t n = BootReason::format(buf, sizeof(buf), BootReason::TASK_WDT, 42);
    TEST_ASSERT_GREATER_THAN_UINT32(0, n);
    TEST_ASSERT_EQUAL_STRING(
        "{\"reason\":\"task_wdt\",\"code\":6,\"boots\":42,\"abnormal\":true}", buf);
    TEST_ASSERT_EQUAL_UINT32(strlen(buf), n);
}

void test_boot_reason_payload_worst_case_fits_96() {
    // The health topic has a live 256 B drop cliff, so a new payload states its
    // bound rather than hoping. Longest label + a saturated counter.
    char buf[96];
    const size_t n = BootReason::format(buf, sizeof(buf), BootReason::PWR_GLITCH, 4294967295u);
    TEST_ASSERT_GREATER_THAN_UINT32(0, n);
    TEST_ASSERT_LESS_THAN_UINT32(96, n);
}

void test_boot_reason_overflow_clears_the_buffer() {
    // snprintf leaves a truncated but valid-LOOKING string on overflow, which would
    // publish half a JSON document. format() must return 0 and empty the buffer.
    char buf[8];
    memset(buf, 'x', sizeof(buf));
    const size_t n = BootReason::format(buf, sizeof(buf), BootReason::PANIC, 1);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, n, "overflow must report failure");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", buf, "and must not leave a partial payload");
}

void test_boot_reason_rejects_null_and_zero_len() {
    TEST_ASSERT_EQUAL_UINT32(0, BootReason::format(nullptr, 32, BootReason::SW, 1));
    char buf[4];
    TEST_ASSERT_EQUAL_UINT32(0, BootReason::format(buf, 0, BootReason::SW, 1));
}
