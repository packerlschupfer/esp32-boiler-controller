// test/test_native/test_heating_curve_anchoring.cpp
// HeatingCurve::anchoring() - where the weather curve is anchored, and the room deviation.
//
// These tests exist because of a sign error that survived a live validation. Until
// 2026-10-26 the live ROOM reading was passed as the curve's own `insideTenths` as well as
// being used for the Parallelverschiebung, so it fed the curve twice. The net effect was
// that a COLDER house got a LOWER flow target - the opposite of the documented intent, and
// invisible whenever the room sits at target, which is where the 2026-10-18 validation
// against 7 live targets was done.
//
// Every test below computes BOTH formulations from the same inputs and asserts on the
// difference, so the old behaviour cannot return unnoticed.

#include <unity.h>
#include "modules/control/HeatingCurve.h"

namespace {

// Live configuration at the time of the fix.
constexpr int16_t TARGET   = 194;  // 19.4 C
constexpr int16_t SHIFT    = 44;   // curveShift 4.4
constexpr int16_t COEFF    = 140;  // curveCoeff 1.4
constexpr int16_t FACTOR10 = 20;   // roomCurveShiftFactor 2.0
constexpr int16_t LO       = 380;  // h/sLo 38.0
constexpr int16_t HI       = 750;

// THE OLD, BROKEN FORMULATION, kept here on purpose as the thing we compare against:
// anchor on the live room reading AND add its deviation to the shift.
int16_t legacyTarget(int16_t outside, int16_t room) {
    const int32_t deviation = static_cast<int32_t>(TARGET) - room;
    const int16_t shift = static_cast<int16_t>(SHIFT + (deviation * FACTOR10) / 10);
    int32_t raw = 0;
    HeatingCurve::target(room, outside, COEFF, shift, LO, HI, &raw);
    return static_cast<int16_t>(raw);
}

// The fixed formulation, through the policy the firmware now calls.
// NOTE both helpers return the PRE-CLAMP raw value. That is the right oracle for the
// sensitivity property, which is a property of the curve - but it is NOT what the burner
// receives. See commandedTarget() and the clamp tests below: the 2026-10-28 review pointed
// out that every assertion here ignored the LO/HI arguments, so the file's own claim of
// "-factor at EVERY outside temperature" was true of raw and false of the commanded flow.
int16_t fixedTarget(int16_t outside, int16_t room, bool weather = true,
                    bool measured = true, int16_t target = TARGET) {
    const HeatingCurve::Anchoring a =
        HeatingCurve::anchoring(SHIFT, target, room, FACTOR10, weather, measured);
    int32_t raw = 0;
    HeatingCurve::target(a.inside, outside, COEFF, a.shift, LO, HI, &raw);
    return static_cast<int16_t>(raw);
}

// What the BURNER is actually commanded: the clamped return value, not rawOut.
int16_t commandedTarget(int16_t outside, int16_t room) {
    const HeatingCurve::Anchoring a =
        HeatingCurve::anchoring(SHIFT, TARGET, room, FACTOR10, true, true);
    return HeatingCurve::target(a.inside, outside, COEFF, a.shift, LO, HI, nullptr);
}

}  // namespace

void test_anchor_room_at_target_is_identical_to_the_old_behaviour() {
    // The two formulations agree EXACTLY when the room is at target. This is why the
    // 2026-10-18 validation against 7 live targets could not see the defect, and it is the
    // compatibility guarantee: nothing changes for a house that is at temperature.
    for (int16_t outside = -150; outside <= 150; outside += 10) {
        TEST_ASSERT_EQUAL_INT16(legacyTarget(outside, TARGET), fixedTarget(outside, TARGET));
    }
}

void test_anchor_colder_room_now_raises_the_flow_target() {
    // The actual defect. Old: colder room -> LOWER target. New: colder room -> HIGHER.
    const int16_t outside = 60;  // 6.0 C
    const int16_t atTarget = fixedTarget(outside, TARGET);
    const int16_t cold3K   = fixedTarget(outside, TARGET - 30);

    TEST_ASSERT_GREATER_THAN_INT16(atTarget, cold3K);          // fixed: more heat
    TEST_ASSERT_LESS_THAN_INT16(legacyTarget(outside, TARGET),
                                legacyTarget(outside, TARGET - 30));  // old: LESS heat
}

void test_anchor_sensitivity_is_exactly_minus_the_factor() {
    // Anchoring on the target makes d(RAW)/d(room) exactly -roomCurveShiftFactor at EVERY
    // outside temperature. Under the old formulation it was weather-dependent AND positive
    // (+0.70 K/K at 12 C, +0.20 K/K at 0 C), which is why no value of the factor could fix
    // it - the value needed merely to reach zero varied from 2.71 to 2.11.
    //
    // THIS IS A PROPERTY OF THE CURVE, NOT OF THE COMMANDED FLOW. The commanded value is
    // clamped to [h/sLo, h/sHi], so where the curve sits on the floor the burner sees less
    // than this - see test_anchor_commanded_flow_is_clamped_not_the_raw_sensitivity.
    const int16_t outsides[] = {120, 80, 40, 0, -50, -100};
    for (int16_t outside : outsides) {
        const int16_t warm = fixedTarget(outside, TARGET + 10);  // 1 K warm
        const int16_t cold = fixedTarget(outside, TARGET - 10);  // 1 K cold
        // 2 K of room span x factor 2.0 = 40 tenths, and the sign is "cold -> hotter".
        TEST_ASSERT_EQUAL_INT16(40, cold - warm);

        // And the old one really did have the wrong sign at every one of these points.
        TEST_ASSERT_LESS_THAN_INT16(legacyTarget(outside, TARGET + 10),
                                    legacyTarget(outside, TARGET - 10));
    }
}

void test_anchor_recovery_case_measured_on_the_live_config() {
    // The numbers that made the case concrete: a house 3 K down at 6.0 C outside was
    // commanded 44.8, LESS than the 46.2 it would get at temperature. Intent is 52.2.
    // (These are the firmware's integer results. My first pass asserted 449/463/523 from a
    // Python model that FLOORED the divisions where C truncates toward zero - a tenth out on
    // every negative-diff term. The C values are the authority here, which is the point of
    // computing them through the real header.)
    TEST_ASSERT_EQUAL_INT16(448, legacyTarget(60, 164));
    TEST_ASSERT_EQUAL_INT16(462, legacyTarget(60, TARGET));   // warmer house, MORE heat
    TEST_ASSERT_EQUAL_INT16(522, fixedTarget(60, 164));

    // At 4.0 C outside the fix also clears the h/boostMin 50.0 stage-2 gate, which the old
    // formulation did not - the two defects compounded in the same direction.
    TEST_ASSERT_LESS_THAN_INT16(500, legacyTarget(40, 164));
    TEST_ASSERT_GREATER_OR_EQUAL_INT16(500, fixedTarget(40, 164));
}

void test_anchor_room_mode_is_untouched() {
    // weatherMode false: anchor on whatever room value is in use, no deviation. Room mode
    // must be byte-identical to before, because this fix is about the weather curve only.
    const HeatingCurve::Anchoring a =
        HeatingCurve::anchoring(SHIFT, TARGET, 170, FACTOR10, /*weather=*/false, true);
    TEST_ASSERT_EQUAL_INT16(170, a.inside);
    TEST_ASSERT_EQUAL_INT16(SHIFT, a.shift);
}

void test_anchor_substituted_room_gets_no_deviation() {
    // Not a measurement (hold expired -> TARGET or NOMINAL substitution): no deviation, and
    // anchor on the substitute. With a TARGET substitution the value IS the target, so the
    // result is the same either way - assert that explicitly so the equivalence is recorded
    // rather than assumed.
    const HeatingCurve::Anchoring sub =
        HeatingCurve::anchoring(SHIFT, TARGET, TARGET, FACTOR10, true, /*measured=*/false);
    TEST_ASSERT_EQUAL_INT16(TARGET, sub.inside);
    TEST_ASSERT_EQUAL_INT16(SHIFT, sub.shift);

    // NOMINAL substitution with no target configured: nothing to anchor on, pass it through.
    const HeatingCurve::Anchoring nom = HeatingCurve::anchoring(
        SHIFT, /*roomTarget=*/0, HeatingCurve::NOMINAL_ROOM_TENTHS, FACTOR10, true, true);
    TEST_ASSERT_EQUAL_INT16(HeatingCurve::NOMINAL_ROOM_TENTHS, nom.inside);
    TEST_ASSERT_EQUAL_INT16(SHIFT, nom.shift);
}

void test_anchor_zero_factor_disables_only_the_deviation() {
    // Factor 0 must still anchor on the target - that is the curve's parameterisation, not
    // part of the room compensation. Getting this wrong would make factor 0 a THIRD
    // formulation rather than "no room compensation".
    const HeatingCurve::Anchoring a =
        HeatingCurve::anchoring(SHIFT, TARGET, 160, /*factorX10=*/0, true, true);
    TEST_ASSERT_EQUAL_INT16(TARGET, a.inside);
    TEST_ASSERT_EQUAL_INT16(SHIFT, a.shift);
}

void test_anchor_shift_cannot_overflow_int16() {
    // The shift feeds the polynomial through `inside + shift`. A huge deviation must clamp
    // here rather than wrap - the curve clamps its OUTPUT, which does not protect its input.
    const HeatingCurve::Anchoring a =
        HeatingCurve::anchoring(32000, 32767, -32768, 100, true, true);
    TEST_ASSERT_EQUAL_INT16(INT16_MAX, a.shift);
    const HeatingCurve::Anchoring b =
        HeatingCurve::anchoring(-32000, 0 + 1, 32767, 100, true, true);
    TEST_ASSERT_EQUAL_INT16(INT16_MIN, b.shift);
}

void test_anchor_commanded_flow_is_clamped_not_the_raw_sensitivity() {
    // THE CORRECTION THE 2026-10-28 REVIEW FORCED. Every other test in this file asserts on
    // the pre-clamp rawOut, which makes the LO/HI arguments inert - so the file could claim
    // "-2.0 K/K at every outside temperature" while the value the burner is handed moves by
    // much less wherever the curve is below the h/sLo floor. A reader concluded the house gets
    // +4.0 K of flow for a 2 K deficit at 12 C outside; it gets +1.1 K.
    //
    // Assert the real behaviour at both ends so neither can drift:
    //  - mild weather, curve under the floor -> commanded is pinned at LO and the room
    //    deviation buys less than the factor suggests;
    //  - cold weather, curve above the floor -> commanded tracks raw exactly.
    const int16_t mild = 120;  // 12.0 C outside

    // Raw moves the full 2 K per K of room, by construction.
    TEST_ASSERT_EQUAL_INT16(40, fixedTarget(mild, TARGET - 10) - fixedTarget(mild, TARGET + 10));

    // The commanded value cannot go below the floor, so it moves LESS. This is the assertion
    // that would have caught the overstated claim.
    const int16_t cmdWarm = commandedTarget(mild, TARGET + 10);
    const int16_t cmdCold = commandedTarget(mild, TARGET - 10);
    TEST_ASSERT_EQUAL_INT16(LO, cmdWarm);                 // pinned at the floor
    TEST_ASSERT_LESS_THAN_INT16(40, cmdCold - cmdWarm);   // strictly less than raw's 40
    TEST_ASSERT_GREATER_THAN_INT16(0, cmdCold - cmdWarm); // but still the right SIGN

    // Cold enough that the curve is clear of the floor: commanded == raw, so the full
    // sensitivity does reach the burner there.
    const int16_t cold = 0;  // 0.0 C outside
    TEST_ASSERT_EQUAL_INT16(fixedTarget(cold, TARGET - 10), commandedTarget(cold, TARGET - 10));
    TEST_ASSERT_EQUAL_INT16(40, commandedTarget(cold, TARGET - 10) - commandedTarget(cold, TARGET + 10));

    // And the upper clamp is live on this path too, so a huge deficit cannot exceed h/sHi.
    TEST_ASSERT_EQUAL_INT16(HI, commandedTarget(-200, 0));
}
