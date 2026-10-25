/**
 * @file test_bus_release_policy.cpp
 * @brief Unit tests for BusReleasePolicy (deliberate RS485 hand-over, 2026-10-25)
 */

#include <unity.h>
#include <cstdint>

#include "../../include/modules/control/BusReleasePolicy.h"

// setUp and tearDown are defined in test_main.cpp

using namespace BusReleasePolicy;

void test_bus_release_window_is_clamped_and_capped() {
    // 0 means "unspecified" and must take the default, NOT the minimum - otherwise a bare
    // "bus_release" would hand over a 30 s window and the tool run would be cut off.
    TEST_ASSERT_EQUAL_UINT32(DEFAULT_SECONDS, clampSeconds(0));
    TEST_ASSERT_EQUAL_UINT32(MIN_SECONDS, clampSeconds(1));
    TEST_ASSERT_EQUAL_UINT32(MIN_SECONDS, clampSeconds(MIN_SECONDS));
    TEST_ASSERT_EQUAL_UINT32(120, clampSeconds(120));
    // The cap is the point: no caller can park the controller out of service.
    TEST_ASSERT_EQUAL_UINT32(MAX_SECONDS, clampSeconds(MAX_SECONDS));
    TEST_ASSERT_EQUAL_UINT32(MAX_SECONDS, clampSeconds(MAX_SECONDS + 1));
    TEST_ASSERT_EQUAL_UINT32(MAX_SECONDS, clampSeconds(86400));
    TEST_ASSERT_EQUAL_UINT32(MAX_SECONDS, clampSeconds(0xFFFFFFFFu));
    TEST_ASSERT_TRUE_MESSAGE(MAX_SECONDS <= 900, "the hard cap must stay short");
}

void test_bus_release_refuses_while_the_boiler_is_busy() {
    // Entry REFUSES rather than orchestrating a shutdown: there is no new stop path to get
    // wrong. The relay mask is the hardware-verified one.
    const uint8_t burner = 1u << RelayIndex::BURNER_ENABLE;
    const uint8_t water  = 1u << RelayIndex::WATER_MODE;
    const uint8_t pump   = 1u << RelayIndex::HEATING_PUMP;

    TEST_ASSERT_EQUAL(Refusal::BURNER_ACTIVE, mayRelease(burner, false));
    TEST_ASSERT_EQUAL(Refusal::BURNER_ACTIVE, mayRelease(water, false));
    TEST_ASSERT_EQUAL(Refusal::BURNER_ACTIVE, mayRelease(burner | water | pump, false));

    // Demand precedes the relays by a pre-purge, so clear firing bits are not enough.
    TEST_ASSERT_EQUAL(Refusal::DEMAND_PRESENT, mayRelease(0, true));
    TEST_ASSERT_EQUAL(Refusal::DEMAND_PRESENT, mayRelease(pump, true));

    // Idle, or circulating only: allowed. A running pump is harmless and the window is short.
    TEST_ASSERT_EQUAL(Refusal::NONE, mayRelease(0, false));
    TEST_ASSERT_EQUAL(Refusal::NONE, mayRelease(pump, false));
    TEST_ASSERT_EQUAL(Refusal::NONE, mayRelease(1u << RelayIndex::WATER_PUMP, false));
}

void test_bus_release_firing_mask_tracks_the_relay_indices() {
    // The published gate on boiler/status/sensors is `r & 17`, which uses a DIFFERENT bit
    // order (bit0 burner, bit4 water mode) from RelayState (bit0 burner, bit2 water mode).
    // Deriving the mask from RelayIndex is what keeps the two from drifting apart, so pin
    // that it really is the burner-enable and water-mode bits and nothing else.
    TEST_ASSERT_EQUAL_UINT8((1u << RelayIndex::BURNER_ENABLE) | (1u << RelayIndex::WATER_MODE),
                            FIRING_MASK);
    TEST_ASSERT_EQUAL_UINT8(0, FIRING_MASK & (1u << RelayIndex::HEATING_PUMP));
    TEST_ASSERT_EQUAL_UINT8(0, FIRING_MASK & (1u << RelayIndex::WATER_PUMP));
    TEST_ASSERT_EQUAL_UINT8(0, FIRING_MASK & (1u << RelayIndex::POWER_BOOST));
}

void test_bus_release_expiry_survives_the_millis_rollover() {
    // THE LOAD-BEARING TEST. With the bus released there is no guaranteed path for a remote
    // "acquire", so the on-device deadline is the only thing that can end the window. A
    // plain `now >= deadline` breaks across the 49.7-day millis() rollover, and the way it
    // breaks matters: it would extend the window by ~49 days.
    const uint32_t nearEnd = 0xFFFFFF00u;          // 256 ms before the rollover
    const uint32_t deadline = nearEnd + 600000u;   // 10 minutes later, i.e. wrapped
    TEST_ASSERT_FALSE_MESSAGE(expired(nearEnd, deadline), "not expired before the wrap");
    TEST_ASSERT_FALSE_MESSAGE(expired(50u, deadline), "not expired just after the wrap");
    TEST_ASSERT_FALSE(expired(deadline - 1u, deadline));
    TEST_ASSERT_TRUE_MESSAGE(expired(deadline, deadline), "expires exactly at the deadline");
    TEST_ASSERT_TRUE(expired(deadline + 1u, deadline));

    // And the ordinary case, away from the boundary.
    TEST_ASSERT_FALSE(expired(1000u, 601000u));
    TEST_ASSERT_TRUE(expired(601000u, 601000u));
    TEST_ASSERT_TRUE(expired(601001u, 601000u));
}

void test_bus_release_seconds_left_rounds_up_and_never_contradicts_released() {
    // Reporting 0 while still released would make the status topic contradict itself, so a
    // part-second remainder rounds UP.
    TEST_ASSERT_EQUAL_UINT32(600, secondsLeft(0u, 600000u));
    TEST_ASSERT_EQUAL_UINT32(1, secondsLeft(599800u, 600000u));
    TEST_ASSERT_EQUAL_UINT32(1, secondsLeft(599999u, 600000u));
    TEST_ASSERT_EQUAL_UINT32(0, secondsLeft(600000u, 600000u));
    TEST_ASSERT_EQUAL_UINT32(0, secondsLeft(700000u, 600000u));
    // Rollover again: a wrapped deadline must not report a 49-day remainder.
    TEST_ASSERT_EQUAL_UINT32(600, secondsLeft(0xFFFFFF00u, 0xFFFFFF00u + 600000u));
}

void test_bus_release_safe_mask_clears_firing_and_keeps_pumps() {
    // Imposed before bus access resumes, so the first resumed write cannot energise the
    // burner on a decision taken from readings that went stale during the window.
    const uint8_t all = 0xFF;
    TEST_ASSERT_EQUAL_UINT8(0, safeDesiredMask(all) & FIRING_MASK);
    TEST_ASSERT_EQUAL_UINT8(1u << RelayIndex::HEATING_PUMP,
                            safeDesiredMask(1u << RelayIndex::HEATING_PUMP));
    TEST_ASSERT_EQUAL_UINT8(0, safeDesiredMask(FIRING_MASK));
    // Everything that is not a firing bit survives untouched.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(all & ~FIRING_MASK), safeDesiredMask(all));
    TEST_ASSERT_EQUAL_UINT8(0, safeDesiredMask(0));
}
