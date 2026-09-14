/**
 * @file test_burner_transitions.cpp
 * @brief Scenario tests for BurnerTransitions::step() (burner state machine logic)
 *
 * Replays tick sequences through the real transition function. The simulator
 * mirrors StateMachine::update(): state timeouts (PRE_PURGE 2 s -> IGNITION,
 * IGNITION 5 s -> LOCKOUT) are checked before the step, and entering IGNITION
 * records the running mode like onEnterIgnition().
 */

#include <unity.h>
#include <cstdint>

#include "../../include/modules/control/BurnerTransitions.h"

// setUp and tearDown are defined in test_main.cpp

using namespace BurnerTransitions;

namespace {

constexpr uint32_t TICK_MS = 100;
constexpr uint32_t PRE_PURGE_TIME_MS = 2000;     // SystemConstants::Burner::PRE_PURGE_TIME_MS
constexpr uint32_t IGNITION_TIME_MS = 5000;      // SystemConstants::Burner::IGNITION_TIME_MS
constexpr uint32_t MIN_IGNITION_TIME_MS = 3000;  // SystemConstants::Timing::BURNER_MIN_IGNITION_TIME_MS

class FakeEnvironment : public Environment {
public:
    bool safety = true;
    bool flame = true;
    bool boilerEn = true;
    bool heatingEn = true;
    bool waterEn = true;
    bool heatingOnBit = false;
    bool waterOnBit = false;
    bool priority = false;
    bool heatingReq = false;
    bool waterReq = false;
    bool relaysWater = false;
    bool boilerHot = false;          // BurnerPowerController blocks high power
    bool heatingWanted = true;
    bool turnOnAllowed = true;
    bool turnOffAllowed = true;
    bool powerChangeAllowed = true;
    bool switchSucceeds = true;

    int safetyCalls = 0;
    int switchCalls = 0;

    bool safetyOk() override { safetyCalls++; return safety; }
    bool flameDetected() override { return flame; }
    bool boilerEnabled() override { return boilerEn; }
    bool heatingEnabled() override { return heatingEn; }
    bool waterEnabled() override { return waterEn; }
    bool heatingOn() override { return heatingOnBit; }
    bool waterOn() override { return waterOnBit; }
    bool waterPriority() override { return priority; }
    bool heatingRequested() override { return heatingReq; }
    bool waterRequested() override { return waterReq; }
    bool relaysInWaterMode() override { return relaysWater; }
    bool highPowerAllowed(bool requestedHighPower) override { return requestedHighPower && !boilerHot; }
    bool heatingLikelyWanted() override { return heatingWanted; }
    bool canTurnOn() override { return turnOnAllowed; }
    bool canTurnOff() override { return turnOffAllowed; }
    bool canChangePower(bool) override { return powerChangeAllowed; }
    bool switchMode(bool toWater) override {
        switchCalls++;
        if (switchSucceeds) {
            relaysWater = toWater;
        }
        return switchSucceeds;
    }
};

struct Simulator {
    FakeEnvironment env;
    Memory memory;
    Timing timing;
    BurnerSMState state;
    uint32_t nowMs;
    uint32_t entryMs;
    bool heatDemand;
    bool highPower;
    Decision last;
    int visits[9];

    Simulator()
        : memory(Memory()), timing(Timing()), state(BurnerSMState::IDLE),
          nowMs(1000), entryMs(1000), heatDemand(false), highPower(false), last(Decision()) {
        timing.ignitionMinTimeMs = MIN_IGNITION_TIME_MS;
        timing.ignitionTimeoutMs = IGNITION_TIME_MS;
        timing.maxIgnitionRetries = 3;
        timing.modeDemandLossGraceMs = MODE_DEMAND_LOSS_GRACE_MS;
        clearVisits();
    }

    void clearVisits() {
        for (int i = 0; i < 9; i++) {
            visits[i] = 0;
        }
    }

    int visitsOf(BurnerSMState s) const { return visits[static_cast<int>(s)]; }

    void enter(BurnerSMState s) {
        state = s;
        entryMs = nowMs;
        visits[static_cast<int>(s)]++;
        if (s == BurnerSMState::IGNITION) {
            // onEnterIgnition(): water if WATER_ON && (!HEATING_ON || WATER_PRIORITY)
            memory.runningModeIsWater = env.waterOnBit && (!env.heatingOnBit || env.priority);
            env.relaysWater = memory.runningModeIsWater;
        }
    }

    void tick() {
        nowMs += TICK_MS;
        const uint32_t inState = nowMs - entryMs;
        // StateMachine::update(): timeout first (strictly greater), then the handler
        if (state == BurnerSMState::PRE_PURGE && inState > PRE_PURGE_TIME_MS) {
            enter(BurnerSMState::IGNITION);
            return;
        }
        if (state == BurnerSMState::IGNITION && inState > IGNITION_TIME_MS) {
            enter(BurnerSMState::LOCKOUT);
            return;
        }
        const Context ctx = { state, inState, nowMs, heatDemand, highPower };
        last = step(ctx, timing, env, memory);
        if (last.next != state) {
            enter(last.next);
        }
    }

    void run(uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += TICK_MS) {
            tick();
        }
    }
};

Simulator sim;

void requestHeating(bool on) {
    sim.env.heatingOnBit = on;
    sim.env.heatingReq = on;
}

void requestWater(bool on) {
    sim.env.waterOnBit = on;
    sim.env.waterReq = on;
}

bool isRunning() {
    return sim.state == BurnerSMState::RUNNING_LOW || sim.state == BurnerSMState::RUNNING_HIGH;
}

// Fresh simulator, start the burner for one mode and run until it is lit.
void startBurner(bool water, bool highPower) {
    sim = Simulator();
    if (water) {
        requestWater(true);
        sim.env.priority = true;
    } else {
        requestHeating(true);
    }
    sim.heatDemand = true;
    sim.highPower = highPower;
    for (int i = 0; i < 100 && !isRunning(); i++) {
        sim.tick();
    }
    TEST_ASSERT_TRUE(sim.state == (highPower ? BurnerSMState::RUNNING_HIGH : BurnerSMState::RUNNING_LOW));
    TEST_ASSERT_EQUAL(water, sim.memory.runningModeIsWater);
    sim.clearVisits();
}

} // namespace

// --- Start ------------------------------------------------------------------

void test_bsm_step_idle_without_demand_skips_safety_check() {
    sim = Simulator();
    requestHeating(true);  // mode active, but no heat demand from BoilerTempControl
    sim.run(5000);
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::IDLE);
    // performSafetyCheck() can emergency-shutdown; it is only probed with demand
    TEST_ASSERT_EQUAL_INT(0, sim.env.safetyCalls);
}

void test_bsm_step_stale_demand_never_starts_burner() {
    // 2026-09-12/13: latched demand after ERROR recovery, no mode request
    sim = Simulator();
    sim.heatDemand = true;
    sim.run(60000);
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::IDLE);
    TEST_ASSERT_TRUE(sim.last.reason == Reason::STALE_DEMAND);
    TEST_ASSERT_EQUAL_INT(0, sim.env.safetyCalls);
}

void test_bsm_step_heating_start_sequence() {
    sim = Simulator();
    requestHeating(true);
    sim.heatDemand = true;
    sim.highPower = true;
    sim.tick();
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::PRE_PURGE);
    sim.run(PRE_PURGE_TIME_MS + TICK_MS);
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::IGNITION);
    sim.run(MIN_IGNITION_TIME_MS - TICK_MS);
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::IGNITION);
    sim.tick();
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::RUNNING_HIGH);
    TEST_ASSERT_TRUE(sim.last.reason == Reason::IGNITION_OK);
    TEST_ASSERT_FALSE(sim.memory.runningModeIsWater);
}

void test_bsm_step_start_waits_for_minimum_off_time() {
    sim = Simulator();
    requestHeating(true);
    sim.heatDemand = true;
    sim.env.turnOnAllowed = false;
    sim.run(1000);
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::IDLE);
    TEST_ASSERT_TRUE(sim.last.reason == Reason::START_DELAYED);
    sim.env.turnOnAllowed = true;
    sim.tick();
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::PRE_PURGE);
}

void test_bsm_step_request_withdrawn_during_pre_purge_aborts() {
    sim = Simulator();
    requestHeating(true);
    sim.heatDemand = true;
    sim.tick();
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::PRE_PURGE);
    requestHeating(false);
    sim.tick();
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::IDLE);
    TEST_ASSERT_TRUE(sim.last.reason == Reason::MODE_WITHDRAWN);
    TEST_ASSERT_EQUAL_INT(0, sim.visitsOf(BurnerSMState::IGNITION));
}

// --- Stop -------------------------------------------------------------------

void test_bsm_step_heating_disable_stops_during_min_on_time() {
    startBurner(false, false);
    sim.env.turnOffAllowed = false;  // inside the 2 min minimum on-time
    sim.env.heatingEn = false;
    sim.tick();
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::POST_PURGE);
    TEST_ASSERT_TRUE(sim.last.reason == Reason::EXPLICIT_DISABLE);
    TEST_ASSERT_FALSE(sim.last.fromWater);
}

void test_bsm_step_water_disable_does_not_stop_heating() {
    startBurner(false, false);
    sim.env.turnOffAllowed = false;
    sim.env.waterEn = false;
    sim.run(5000);
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::RUNNING_LOW);
}

void test_bsm_step_demand_end_respects_min_on_time() {
    startBurner(false, false);
    sim.env.turnOffAllowed = false;
    sim.heatDemand = false;
    sim.run(5000);
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::RUNNING_LOW);
    TEST_ASSERT_TRUE(sim.last.stopDelayed);
    sim.env.turnOffAllowed = true;
    sim.tick();
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::POST_PURGE);
    TEST_ASSERT_TRUE(sim.last.reason == Reason::DEMAND_ENDED);
}

void test_bsm_step_lost_mode_request_stops_after_grace() {
    startBurner(false, false);
    sim.env.turnOffAllowed = false;
    sim.env.heatingReq = false;  // HEATING_ON still set, request withdrawn
    sim.run(MODE_DEMAND_LOSS_GRACE_MS);
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::RUNNING_LOW);
    sim.run(2 * TICK_MS);
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::POST_PURGE);
    TEST_ASSERT_TRUE(sim.last.reason == Reason::NO_MODE_DEMAND);
}

void test_bsm_step_flame_loss_bypasses_min_on_time() {
    startBurner(false, false);
    sim.env.turnOffAllowed = false;
    sim.env.flame = false;
    sim.tick();
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::POST_PURGE);
    TEST_ASSERT_TRUE(sim.last.reason == Reason::FLAME_LOST);
}

// --- Mode switching ---------------------------------------------------------

void test_bsm_step_heating_to_water_is_seamless() {
    // 2026-09-14 16:09:19: hot water preempts low-power heating
    startBurner(false, false);
    requestHeating(false);
    requestWater(true);
    sim.env.priority = true;
    sim.highPower = true;
    sim.tick();
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::MODE_SWITCHING);
    TEST_ASSERT_TRUE(sim.last.reason == Reason::SWITCH_SEAMLESS);
    sim.tick();
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::RUNNING_HIGH);
    TEST_ASSERT_TRUE(sim.last.reason == Reason::SWITCH_DONE);
    TEST_ASSERT_TRUE(sim.env.relaysWater);
    TEST_ASSERT_TRUE(sim.memory.runningModeIsWater);
    sim.run(5000);
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::RUNNING_HIGH);
    TEST_ASSERT_EQUAL_INT(1, sim.env.switchCalls);
    TEST_ASSERT_EQUAL_INT(0, sim.visitsOf(BurnerSMState::POST_PURGE));
}

void test_bsm_step_water_to_heating_waits_for_heating_request() {
    // 2026-09-14 16:09:58: tank charged, HeatingControlTask requests ~1 s later
    startBurner(true, true);
    requestWater(false);
    sim.env.priority = false;
    sim.env.heatingWanted = true;
    sim.tick();
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::MODE_SWITCHING);
    sim.run(1000);
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::MODE_SWITCHING);
    TEST_ASSERT_TRUE(sim.last.reason == Reason::SWITCH_WAIT_FOR_HEATING);
    TEST_ASSERT_EQUAL_INT(0, sim.env.switchCalls);
    requestHeating(true);
    sim.highPower = false;
    sim.tick();
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::RUNNING_LOW);
    TEST_ASSERT_TRUE(sim.last.reason == Reason::SWITCH_DONE);
    TEST_ASSERT_EQUAL_INT(1, sim.env.switchCalls);
    TEST_ASSERT_FALSE(sim.env.relaysWater);
    TEST_ASSERT_EQUAL_INT(0, sim.visitsOf(BurnerSMState::POST_PURGE));
}

void test_bsm_step_handover_stops_when_heating_not_wanted() {
    startBurner(true, true);
    requestWater(false);
    sim.env.heatingWanted = false;  // heating disabled / warm outside
    sim.tick();
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::MODE_SWITCHING);
    sim.tick();
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::POST_PURGE);
    TEST_ASSERT_TRUE(sim.last.reason == Reason::SWITCH_NO_DEMAND);
    TEST_ASSERT_EQUAL_INT(0, sim.env.switchCalls);
}

void test_bsm_step_handover_wait_is_bounded() {
    startBurner(true, true);
    requestWater(false);
    sim.env.heatingWanted = true;  // wanted, but the request never comes
    sim.tick();
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::MODE_SWITCHING);
    sim.run(BurnerTransitionPolicy::MODE_SWITCH_MAX_WAIT_MS - TICK_MS);
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::MODE_SWITCHING);
    sim.tick();
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::POST_PURGE);
    TEST_ASSERT_TRUE(sim.last.reason == Reason::SWITCH_NO_DEMAND);
}

void test_bsm_step_failed_mode_switch_stops_burner() {
    startBurner(false, false);
    sim.env.switchSucceeds = false;
    requestHeating(false);
    requestWater(true);
    sim.env.priority = true;
    sim.tick();
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::MODE_SWITCHING);
    sim.tick();
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::POST_PURGE);
    TEST_ASSERT_TRUE(sim.last.reason == Reason::SWITCH_FAILED);
    TEST_ASSERT_FALSE(sim.memory.runningModeIsWater);
}

void test_bsm_step_revert_without_on_bit_does_not_bounce() {
    // WATER_OFF_OVERRIDE clears WATER_ON while the water request is still set
    startBurner(true, true);
    sim.env.waterOnBit = false;
    sim.tick();
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::MODE_SWITCHING);
    sim.run(BurnerTransitionPolicy::MODE_SWITCH_MAX_WAIT_MS - TICK_MS);
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::MODE_SWITCHING);
    TEST_ASSERT_TRUE(sim.last.reason == Reason::SWITCH_REVERT_WAIT);
    TEST_ASSERT_EQUAL_INT(1, sim.visitsOf(BurnerSMState::MODE_SWITCHING));
    TEST_ASSERT_EQUAL_INT(0, sim.visitsOf(BurnerSMState::RUNNING_LOW));
    TEST_ASSERT_EQUAL_INT(0, sim.visitsOf(BurnerSMState::RUNNING_HIGH));
    sim.tick();
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::POST_PURGE);
    TEST_ASSERT_TRUE(sim.last.reason == Reason::SWITCH_REVERT_STOP);
}

void test_bsm_step_revert_with_on_bit_resumes_low_power() {
    startBurner(true, true);
    sim.env.waterOnBit = false;
    sim.tick();
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::MODE_SWITCHING);
    sim.env.waterOnBit = true;
    sim.tick();
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::RUNNING_LOW);
    TEST_ASSERT_TRUE(sim.last.reason == Reason::SWITCH_RESUME);
    TEST_ASSERT_EQUAL_INT(0, sim.env.switchCalls);
}

void test_bsm_step_mode_change_without_flame_stops() {
    startBurner(false, false);
    sim.env.flame = false;
    requestHeating(false);
    requestWater(true);
    sim.env.priority = true;
    sim.tick();
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::POST_PURGE);
    TEST_ASSERT_TRUE(sim.last.reason == Reason::SWITCH_NEEDS_STOP);
    TEST_ASSERT_EQUAL_INT(0, sim.visitsOf(BurnerSMState::MODE_SWITCHING));
}

void test_bsm_step_safety_failure_during_mode_switch_errors() {
    startBurner(false, false);
    requestHeating(false);
    requestWater(true);
    sim.env.priority = true;
    sim.tick();
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::MODE_SWITCHING);
    sim.env.safety = false;
    sim.tick();
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::ERROR);
    TEST_ASSERT_TRUE(sim.last.reason == Reason::SAFETY_FAILED);
}

// --- Power level ------------------------------------------------------------

void test_bsm_step_power_level_follows_request_with_anti_flapping() {
    startBurner(false, false);
    sim.highPower = true;
    sim.env.powerChangeAllowed = false;
    sim.run(1000);
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::RUNNING_LOW);
    TEST_ASSERT_TRUE(sim.last.reason == Reason::POWER_UP_DELAYED);
    sim.env.powerChangeAllowed = true;
    sim.tick();
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::RUNNING_HIGH);
    sim.highPower = false;
    sim.tick();
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::RUNNING_LOW);
    TEST_ASSERT_TRUE(sim.last.reason == Reason::POWER_DOWN);
    // Hot boiler: a high power request does not raise the power
    sim.env.boilerHot = true;
    sim.highPower = true;
    sim.run(1000);
    TEST_ASSERT_TRUE(sim.state == BurnerSMState::RUNNING_LOW);
}
