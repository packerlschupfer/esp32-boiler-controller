// test/test_native/test_water_charge_policy.cpp
// WaterChargePolicy guards that stop a DHW charge locking out space heating and frost.
//
// A running charge has five exits - tank above the stop limit, water OFF override, water
// disabled, boiler disabled, sensor loss - and NOT ONE of them is time- or convergence-based.
// The 10-minute burner request watchdog cannot end it either, because WheaterControlTask
// re-asserts the request every 5 minutes. So an unfinishable charge runs forever, and with
// WATER_PRIORITY on (the live configuration) that locks out space heating AND frost
// protection for as long as it stands. Found by the 2026-10-28 behavioural review.

#include <unity.h>
#include "modules/control/WaterChargePolicy.h"

void test_charge_tank_limit_reachability() {
    using namespace WaterChargePolicy;
    const int16_t head = static_cast<int16_t>(CHARGE_DELTA_MIN * 10.0f);  // 50 tenths

    // THE DEFECT, and it needs no hardware fault. There was no cross-field check between
    // wheater/tempLimitHigh (registered 500-850) and w/hHi (600-1000), so this pair was
    // accepted and the charge could never finish - and with WATER_PRIORITY on that locks out
    // space heating AND frost protection indefinitely, because a charge has five exits and
    // not one of them is time- or convergence-based.
    TEST_ASSERT_FALSE(tankLimitReachable(/*tankStop*/650, /*w/hHi*/600, head));

    // Reachable only when the flow ceiling clears the tank stop limit by the coil head: the
    // tank is heated through a coil, so flow must sit above tank temperature for any transfer.
    TEST_ASSERT_FALSE(tankLimitReachable(650, 699, head));
    TEST_ASSERT_TRUE (tankLimitReachable(650, 700, head));
    TEST_ASSERT_TRUE (tankLimitReachable(650, 900, head));

    // The shipped defaults must pass, or the gate would refuse every normal charge.
    TEST_ASSERT_TRUE(tankLimitReachable(500, 800, head));
}

void test_charge_overrun_is_bounded() {
    using namespace WaterChargePolicy;
    // The reachability check rules out the CONFIGURATION case; this bounds the physical one -
    // a fouled coil, a tank sensor reading low, or the boiler's own thermostat cutting the
    // flame below the commanded flow. A measured full charge is ~40 min, so the cap is far
    // beyond any legitimate charge and exists only so heating and frost get the burner back.
    TEST_ASSERT_FALSE(chargeOverran(0));
    TEST_ASSERT_FALSE(chargeOverran(40u * 60u * 1000u));          // a normal full charge
    TEST_ASSERT_FALSE(chargeOverran(MAX_CHARGE_MS - 1));
    TEST_ASSERT_TRUE (chargeOverran(MAX_CHARGE_MS));
    TEST_ASSERT_TRUE (chargeOverran(MAX_CHARGE_MS * 2));
    // Caller-supplied cap, for tests and for any future per-install tuning.
    TEST_ASSERT_TRUE (chargeOverran(6000, 5000));
    TEST_ASSERT_FALSE(chargeOverran(4999, 5000));
}

// ---------------------------------------------------------------------------
// chargeHeldAfterAbandon(): the half of the 3 h cap that was missing.
//
// 374b5dc ended an overrunning charge but nothing stopped the next tick from
// starting it again, because the START branch is only "tank below the low limit"
// and an unfinishable charge leaves it that way by definition.
// ---------------------------------------------------------------------------

void test_abandon_hold_blocks_the_immediate_retry() {
    // The regression: tank still cold after an abandon. Starting again would burn
    // another 3 h and keep heating and frost locked out.
    TEST_ASSERT_TRUE_MESSAGE(
        WaterChargePolicy::chargeHeldAfterAbandon(true, 400, 450),
        "abandoned and still below the start limit must hold");
}

void test_abandon_hold_releases_when_the_tank_recovers() {
    // At or above the low limit, whatever prevented the charge has gone.
    TEST_ASSERT_FALSE_MESSAGE(
        WaterChargePolicy::chargeHeldAfterAbandon(true, 450, 450),
        "exactly at the start limit is no longer below it - release");
    TEST_ASSERT_FALSE_MESSAGE(
        WaterChargePolicy::chargeHeldAfterAbandon(true, 520, 450),
        "above the start limit must release");
}

void test_abandon_hold_is_inert_without_an_abandon() {
    // A cold tank with no prior abandon is the normal reason to charge, and this
    // predicate must never block it.
    TEST_ASSERT_FALSE_MESSAGE(
        WaterChargePolicy::chargeHeldAfterAbandon(false, 300, 450),
        "no abandon means no hold, however cold the tank");
}

void test_abandon_hold_then_normal_start_still_works() {
    // The sequence that matters end to end: hold while cold, release on recovery,
    // and nextChargeNeeded() then free to ask for a charge again on the next dip.
    TEST_ASSERT_TRUE(WaterChargePolicy::chargeHeldAfterAbandon(true, 400, 450));
    TEST_ASSERT_FALSE(WaterChargePolicy::chargeHeldAfterAbandon(true, 460, 450));
    // latch cleared by the task at that point; a later dip is a fresh charge
    TEST_ASSERT_TRUE_MESSAGE(
        WaterChargePolicy::nextChargeNeeded(false, 440, 450, 550),
        "once the hold is gone a cold tank charges normally again");
}
