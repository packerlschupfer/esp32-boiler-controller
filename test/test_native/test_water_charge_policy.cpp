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
