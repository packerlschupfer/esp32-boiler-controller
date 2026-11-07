/**
 * @file test_syslog_window_policy.cpp
 * @brief Unit tests for SyslogWindowPolicy (bounded remote-logging window)
 */

#include <unity.h>
#include <cstdint>

#include "../../include/modules/diagnostics/SyslogWindowPolicy.h"

// setUp and tearDown are defined in test_main.cpp

using SyslogWindowPolicy::Refusal;
using SyslogWindowPolicy::clampSeconds;
using SyslogWindowPolicy::expired;
using SyslogWindowPolicy::isLater;
using SyslogWindowPolicy::mayOpen;
using SyslogWindowPolicy::secondsLeft;

// The values the policy is deployed with, restated so a test fails if one moves silently.
static constexpr uint32_t DEF = 600;
static constexpr uint32_t MIN = 60;
static constexpr uint32_t MAX = 1800;
static constexpr size_t   BLOCK_OK = SyslogWindowPolicy::REQUIRED_DRAM_BLOCK;

void test_syslog_window_clamp_zero_takes_the_default_not_the_minimum() {
    // 0 means "unspecified". Taking MIN here would silently give an operator who typed
    // nothing a 60 s window, which is shorter than most burns and would look like a bug.
    TEST_ASSERT_EQUAL_UINT32(DEF, clampSeconds(0));
}

void test_syslog_window_clamp_bounds() {
    TEST_ASSERT_EQUAL_UINT32(MIN, clampSeconds(1));
    TEST_ASSERT_EQUAL_UINT32(MIN, clampSeconds(MIN - 1));
    TEST_ASSERT_EQUAL_UINT32(MIN, clampSeconds(MIN));
    TEST_ASSERT_EQUAL_UINT32(MAX, clampSeconds(MAX));
    TEST_ASSERT_EQUAL_UINT32(MAX, clampSeconds(MAX + 1));
    TEST_ASSERT_EQUAL_UINT32(MAX, clampSeconds(0xFFFFFFFFu));
}

void test_syslog_window_clamp_passes_values_in_range_unchanged() {
    TEST_ASSERT_EQUAL_UINT32(120u, clampSeconds(120));
    TEST_ASSERT_EQUAL_UINT32(DEF, clampSeconds(DEF));
    TEST_ASSERT_EQUAL_UINT32(1200u, clampSeconds(1200));
}

void test_syslog_window_opens_when_all_conditions_are_met() {
    TEST_ASSERT_EQUAL(Refusal::NONE, mayOpen(true, false, BLOCK_OK, true));
    TEST_ASSERT_EQUAL(Refusal::NONE, mayOpen(true, false, BLOCK_OK * 4, true));
}

void test_syslog_window_refused_during_ota() {
    // The condition that matters most: the OTA path drops the largest free block to ~1.5 kB,
    // and taking the window's ~1.4 kB from the same pool at the same moment reproduces the
    // September 2026 OTA watchdog reboot on purpose.
    TEST_ASSERT_EQUAL(Refusal::OTA_ACTIVE, mayOpen(true, true, BLOCK_OK * 4, true));
}

void test_syslog_window_refused_when_no_server_is_configured() {
    // Opening a window aimed at 0.0.0.0 would allocate, log, send into nothing and report
    // success - the exact silent-no-op shape this project keeps finding.
    TEST_ASSERT_EQUAL(Refusal::NOT_CONFIGURED, mayOpen(true, false, BLOCK_OK * 4, false));
}

void test_syslog_window_refused_when_dram_block_too_small() {
    TEST_ASSERT_EQUAL(Refusal::LOW_DRAM, mayOpen(true, false, BLOCK_OK - 1, true));
    TEST_ASSERT_EQUAL(Refusal::LOW_DRAM, mayOpen(true, false, 0, true));
}

void test_syslog_window_dram_threshold_is_inclusive() {
    // Stated explicitly because the difference between >= and > here is one refusal at the
    // exact boundary, and that is precisely where a hand-tuned threshold gets tested.
    TEST_ASSERT_EQUAL(Refusal::NONE, mayOpen(true, false, BLOCK_OK, true));
    TEST_ASSERT_EQUAL(Refusal::LOW_DRAM, mayOpen(true, false, BLOCK_OK - 1, true));
}

void test_syslog_window_refused_when_master_switch_is_off() {
    // syslog/enabled is the persistent off-switch. Without this check the parameter would be
    // accepted, persisted and published while governing nothing.
    TEST_ASSERT_EQUAL(Refusal::SWITCHED_OFF, mayOpen(false, false, BLOCK_OK * 4, true));
}

void test_syslog_window_disabled_outranks_every_other_refusal() {
    // An operator who switched the feature off should be told THAT, not told about DRAM.
    TEST_ASSERT_EQUAL(Refusal::SWITCHED_OFF, mayOpen(false, true, 0, false));
}

void test_syslog_window_refusal_precedence_ota_first() {
    // With everything wrong at once the operator should be told about the OTA, because that
    // is the one with a real deadline and the one that makes the others temporary.
    TEST_ASSERT_EQUAL(Refusal::OTA_ACTIVE, mayOpen(true, true, 0, false));
    TEST_ASSERT_EQUAL(Refusal::NOT_CONFIGURED, mayOpen(true, false, 0, false));
}

void test_syslog_window_expiry_basic() {
    const uint32_t deadline = 11000u;
    TEST_ASSERT_FALSE(expired(1000u, deadline));
    TEST_ASSERT_FALSE(expired(10999u, deadline));
    TEST_ASSERT_TRUE(expired(11000u, deadline));
    TEST_ASSERT_TRUE(expired(50000u, deadline));
}

void test_syslog_window_expiry_survives_millis_rollover() {
    // millis() wraps every ~49.7 days and this controller has run longer. A plain
    // `now >= deadline` either ends the window instantly or extends it by 49 days across the
    // wrap; the long way round is the one that matters, because the subscriber stays
    // registered and every log line keeps paying for it.
    const uint32_t deadline = 0x00000800u;   // just AFTER the wrap
    TEST_ASSERT_FALSE(expired(0xFFFFF000u, deadline));  // before the wrap, not yet due
    TEST_ASSERT_FALSE(expired(0x000007FFu, deadline));  // wrapped, 1 ms to go
    TEST_ASSERT_TRUE(expired(0x00000800u, deadline));   // exactly due
    TEST_ASSERT_TRUE(expired(0x00000900u, deadline));   // past due
}

void test_syslog_window_seconds_left_rounds_up() {
    // Rounding DOWN would report 0 s left while the window is still open, contradicting the
    // open flag published next to it.
    TEST_ASSERT_EQUAL_UINT32(30u, secondsLeft(5000u, 35000u));
    TEST_ASSERT_EQUAL_UINT32(1u, secondsLeft(34800u, 35000u));   // 200 ms left -> 1, not 0
    TEST_ASSERT_EQUAL_UINT32(0u, secondsLeft(35000u, 35000u));
    TEST_ASSERT_EQUAL_UINT32(0u, secondsLeft(99000u, 35000u));
}

void test_syslog_window_seconds_left_survives_rollover() {
    const uint32_t deadline = 0x00000800u;
    // 0x800 - 0xFFFFF000 wraps to 6144 ms, which rounds UP to 7 s.
    TEST_ASSERT_EQUAL_UINT32(7u, secondsLeft(0xFFFFF000u, deadline));
    TEST_ASSERT_EQUAL_UINT32(0u, secondsLeft(0x00000900u, deadline));
}

void test_syslog_window_relater_extends_never_shortens() {
    // A second request must EXTEND an open window, never cut it short under whoever is using
    // it. BusReleasePolicy had exactly that bug: the comment claimed extend-only while the
    // code stored the new deadline unconditionally.
    TEST_ASSERT_TRUE(isLater(20000u, 10000u));
    TEST_ASSERT_FALSE(isLater(10000u, 20000u));
    TEST_ASSERT_FALSE(isLater(10000u, 10000u));
    // and across the wrap, where a plain `>` calls the later deadline "earlier"
    TEST_ASSERT_TRUE(isLater(0x00000100u, 0xFFFFFF00u));
    TEST_ASSERT_FALSE(isLater(0xFFFFFF00u, 0x00000100u));
}

void test_syslog_window_refusal_labels_are_stable() {
    // These are published and greppable; a reword is an interface change.
    TEST_ASSERT_EQUAL_STRING("ok", SyslogWindowPolicy::toString(Refusal::NONE));
    TEST_ASSERT_EQUAL_STRING("ota_active", SyslogWindowPolicy::toString(Refusal::OTA_ACTIVE));
    TEST_ASSERT_EQUAL_STRING("low_dram", SyslogWindowPolicy::toString(Refusal::LOW_DRAM));
    TEST_ASSERT_EQUAL_STRING("not_configured",
                             SyslogWindowPolicy::toString(Refusal::NOT_CONFIGURED));
}

void test_syslog_window_cap_can_span_a_real_burn() {
    // The window exists to catch a burn decision. Measured burns ran 16-53 minutes, so a cap
    // shorter than a burn start would make the feature useless for its own purpose. This is a
    // design assertion, not an arithmetic one: if someone lowers MAX, they should see why.
    TEST_ASSERT_TRUE(MAX >= 1800u);
    TEST_ASSERT_TRUE(SyslogWindowPolicy::REQUIRED_DRAM_BLOCK >
                     SyslogWindowPolicy::WINDOW_DRAM_NEED);
}
